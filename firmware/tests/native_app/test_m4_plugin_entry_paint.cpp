// M4 AppList -> plugin entry-paint contract.
//
// RED until the handoff paints cleanly:
//   1. After AppList claims the screen for a child, the parent display task
//      must stop without reading the unsynchronized child pointer.
//   2. Startup painting must happen on the child render owner, not concurrently
//      from onEnter and the runtime task.
//   3. Every first child frame must serialize its clear/full submit with the
//      process-wide render guard.
//
// Build:
//   /opt/homebrew/bin/g++-14 -std=c++17 -I firmware/src \
//     firmware/tests/native_app/test_m4_plugin_entry_paint.cpp \
//     -o /tmp/test_m4_plugin_entry_paint && /tmp/test_m4_plugin_entry_paint
// Run from the workspace root (/tmp/m4-ui-redesign-apple-phase1).

#include <cassert>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string readFile(const char* path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string bodyOf(const std::string& src, const std::string& marker) {
  const size_t sig = src.find(marker);
  if (sig == std::string::npos) return {};
  const size_t open = src.find('{', sig);
  if (open == std::string::npos) return {};
  int depth = 0;
  for (size_t i = open; i < src.size(); ++i) {
    if (src[i] == '{') ++depth;
    if (src[i] == '}') {
      if (--depth == 0) return src.substr(open, i - open + 1);
    }
  }
  return {};
}

size_t countOcc(const std::string& s, const std::string& sub) {
  size_t n = 0;
  for (size_t p = s.find(sub); p != std::string::npos; p = s.find(sub, p + sub.size())) ++n;
  return n;
}

}  // namespace

int main() {
  const std::string apps =
      readFile("firmware/src/activities/apps/AppListActivity.cpp");
  const std::string appsHeader =
      readFile("firmware/src/activities/apps/AppListActivity.h");
  const std::string rt =
      readFile("firmware/src/activities/apps/AppRuntimeActivity.cpp");
  const std::string native =
      readFile("firmware/src/activities/apps/NativeAppActivity.cpp");
  assert(!apps.empty() && !appsHeader.empty() && !rt.empty() && !native.empty());

  const std::string openFn = bodyOf(apps, "void AppListActivity::openSelected()");
  const std::string loopFn = bodyOf(apps, "void AppListActivity::displayTaskLoop()");
  const std::string enterFn = bodyOf(rt, "void AppRuntimeActivity::onEnter()");
  const std::string runtimeTaskFn = bodyOf(rt, "void AppRuntimeActivity::runtimeTaskMain()");
  const std::string startupFn = bodyOf(rt, "void AppRuntimeActivity::renderStartupPage()");
  const std::string nativeEnterFn = bodyOf(native, "void NativeAppActivity::onEnter()");
  const std::string nativeStartupFn = bodyOf(native, "void NativeAppActivity::renderStartupPage()");
  const std::string nativeRenderFn = bodyOf(native, "void NativeAppActivity::render()");
  assert(!openFn.empty() && !loopFn.empty() && !enterFn.empty() &&
         !runtimeTaskFn.empty() && !startupFn.empty() && !nativeEnterFn.empty() &&
         !nativeStartupFn.empty() && !nativeRenderFn.empty());

  // Start-case region: from its case label to the next case label.
  const size_t startCase = rt.find("case EventType::Start:");
  const size_t stopCase = rt.find("case EventType::Stop:");
  assert(startCase != std::string::npos && stopCase != std::string::npos);
  assert(startCase < stopCase);
  const std::string startRegion = rt.substr(startCase, stopCase - startCase);

  // 1a. The atomic screen-owner claim and stale repaint clear happen BEFORE
  //     installing the child, so the parent cannot win the handoff window.
  {
    const size_t enter = openFn.find("enterNewActivity(new");
    const size_t clear = openFn.find("updateRequired_ = false");
    const size_t claim = openFn.find("childScreenOwned_.store(true");
    assert(enter != std::string::npos && clear != std::string::npos &&
           claim != std::string::npos);
    assert(clear < enter && claim < enter);
  }

  // 1b. The display task uses an atomic owner bit for all handoff checks and
  //     never dereferences ActivityWithSubactivity::subActivity.
  assert(appsHeader.find("std::atomic<bool> childScreenOwned_") != std::string::npos);
  assert(countOcc(loopFn, "childScreenOwned_") >= 3);
  assert(loopFn.find("subActivity") == std::string::npos);
  assert(loopFn.find("M4RenderGuard") != std::string::npos);
  const size_t guard = loopFn.find("M4RenderGuard");
  assert(loopFn.find("childScreenOwned_", guard) != std::string::npos);
  assert(apps.find("childScreenOwned_.store(false") != std::string::npos);

  // 2. Runtime startup page is painted once by the runtime owner before
  //    host_.start(); onEnter itself must not race it with a second submit.
  assert(enterFn.find("clearScreen") == std::string::npos);
  assert(enterFn.find("FULL_REFRESH") == std::string::npos);
  assert(startupFn.find("M4RenderGuard") != std::string::npos);
  assert(startupFn.find("clearScreen") != std::string::npos);
  assert(startupFn.find("FULL_REFRESH") != std::string::npos);
  const size_t startup = runtimeTaskFn.find("renderStartupPage()");
  const size_t start = runtimeTaskFn.find("handleEventOnOwner(M4xRuntime::Event::makeStart())");
  assert(startup != std::string::npos && start != std::string::npos && startup < start);

  // Native document plugins get the same clean startup page before the
  // synchronous SD/XML load, and normal rendering is serialized as well.
  const size_t nativeStartup = nativeEnterFn.find("renderStartupPage()");
  const size_t load = nativeEnterFn.find("loadDocument()");
  assert(nativeStartup != std::string::npos && load != std::string::npos && nativeStartup < load);
  assert(nativeStartupFn.find("M4RenderGuard") != std::string::npos);
  assert(nativeRenderFn.find("M4RenderGuard") != std::string::npos);
  {
    // Genuine startup copy, not a recycled error/loading string.
    const bool hasStartingCopy = startupFn.find("正在启动") != std::string::npos &&
                                 nativeStartupFn.find("正在启动") != std::string::npos;
    assert(hasStartingCopy);
  }

  // 3. First produced plugin frame: clean framebuffer + full flush before
  //    resuming the normal policy, under the same process-wide guard.
  assert(startRegion.find("clearScreen") != std::string::npos);
  assert(startRegion.find("FULL_REFRESH") != std::string::npos);
  assert(startRegion.find("M4RenderGuard") != std::string::npos);

  std::puts("PLUGIN_ENTRY_PAINT_CONTRACT_OK");
  return 0;
}
