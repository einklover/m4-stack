// Host contract tests for the AppList display-task/activity handoff.
// Build from the repository root with:
//   /opt/homebrew/bin/g++-14 -std=c++17 \
//     firmware/tests/native_app/test_app_drawer_handoff.cpp \
//     -o /tmp/test_app_drawer_handoff

#include <cassert>
#include <fstream>
#include <iostream>
#include <string>

namespace {

std::string readAppListSource() {
  for (const char* path : {
           "firmware/src/activities/apps/AppListActivity.cpp",
           "../firmware/src/activities/apps/AppListActivity.cpp",
           "../../firmware/src/activities/apps/AppListActivity.cpp",
       }) {
    std::ifstream file(path);
    if (!file) continue;
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  }
  return {};
}

std::string functionBody(const std::string& source, const std::string& signature, const std::string& nextSignature) {
  const size_t start = source.find(signature);
  assert(start != std::string::npos);
  const size_t end = source.find(nextSignature, start + signature.size());
  assert(end != std::string::npos);
  return source.substr(start, end - start);
}

void testWorkerNeverSubmits(const std::string& source) {
  // Cache verify and cooperative exit only. Paint moved to the UI thread, so
  // this span must not find a snapshot or a submit that now lives later.
  const std::string body =
      functionBody(source, "void AppListActivity::displayTaskLoop()", "bool sameInstalledApp(");
  assert(body.find("M4RenderGuard") == std::string::npos);
  assert(body.find("render();") == std::string::npos);
  assert(body.find("setMask") == std::string::npos);
  assert(body.find("snapshot.items") == std::string::npos);
  assert(body.find("subActivity") == std::string::npos);
  assert(body.find("clearScreen") == std::string::npos);
  assert(body.find("childScreenOwned_.load") != std::string::npos);
  assert(body.find("rememberDrawer") == std::string::npos);
  assert(body.find("updateRequired_") == std::string::npos);
  assert(body.find("acceptPendingDrawer") == std::string::npos);
  assert(body.find("apps_ =") == std::string::npos);
  assert(body.find("items_ =") == std::string::npos);
  assert(body.find("selectedIndex_ =") == std::string::npos);
  assert(body.find("mode_ = 0") == std::string::npos);
  assert(body.find("pendingReady_") != std::string::npos);
  const size_t take = body.find("xSemaphoreTake(renderingMutex_");
  const size_t mode = body.find("mode_ ==", take);
  const size_t give = body.find("xSemaphoreGive(renderingMutex_)", take);
  assert(take != std::string::npos && mode != std::string::npos && give != std::string::npos);
  assert(take < mode && mode < give);
  const size_t published = body.find("displayTaskExited_.store(true");
  const size_t discard = body.find("discardPendingDrawer()");
  assert(published != std::string::npos && discard != std::string::npos);
  assert(discard < published);
  assert(body.find("displayTaskHandle_ = nullptr") < published);
  assert(published < body.find("M4Psram::deleteTask(nullptr)"));
}

void testMainLoopSubmitsBeforeInput(const std::string& source) {
  const std::string submit =
      functionBody(source, "void AppListActivity::submitDirtyFrame()", "bool AppListActivity::reload(");
  const size_t snap = submit.find("snapshot.items = items_");
  const size_t clearUpdate = submit.find("updateRequired_ = false;");
  const size_t mask = submit.find("setMask");
  const size_t give = submit.find("xSemaphoreGive(renderingMutex_)", snap);
  const size_t guard = submit.find("M4RenderGuard");
  const size_t paint = submit.find("render();");
  assert(snap != std::string::npos && clearUpdate != std::string::npos && mask != std::string::npos);
  assert(give != std::string::npos && guard != std::string::npos && paint != std::string::npos);
  assert(snap < clearUpdate && clearUpdate < mask && mask < give && give < guard && guard < paint);
  assert(submit.find("childScreenOwned_", guard) != std::string::npos);
  assert(submit.find("subActivity") != std::string::npos);
  assert(submit.find("AppListFrameSnapshot& snapshot = snapshot_;") != std::string::npos);
  assert(submit.find("AppListFrameSnapshot snapshot;") == std::string::npos);
  assert(submit.find("snapshot_ = snapshot") == std::string::npos);
  assert(submit.find("xSemaphoreTake(renderingMutex_", guard) != std::string::npos);

  const std::string loop = functionBody(source, "void AppListActivity::loop()", "void AppListActivity::render()");
  const size_t child = loop.find("if (subActivity)");
  const size_t ret = loop.find("return;", child);
  const size_t accept = loop.find("acceptPendingDrawer()", ret);
  const size_t called = loop.find("submitDirtyFrame()", accept);
  const size_t input = loop.find("mappedInput", called);
  assert(child != std::string::npos && ret != std::string::npos && accept != std::string::npos &&
         called != std::string::npos && input != std::string::npos);
  assert(child < ret && ret < accept && accept < called && called < input);

  const std::string reload = functionBody(source, "bool AppListActivity::reload(", "void AppListActivity::onEnter()");
  assert(reload.find("setMask") == std::string::npos);
  assert(reload.find("M4RenderGuard") == std::string::npos);
  assert(reload.find("render(") == std::string::npos);
  assert(reload.find("rememberDrawer") == std::string::npos);
  assert(reload.find("apps_ = apps") == std::string::npos);
  assert(reload.find("items_ = std::move(items)") == std::string::npos);
  assert(reload.find("mode_ = 0") == std::string::npos);
  assert(reload.find("selectedIndex_ =") == std::string::npos);
  assert(reload.find("updateRequired_") == std::string::npos);
  assert(reload.find("pendingApps_ = std::move(apps)") != std::string::npos);
  assert(reload.find("pendingItems_ = std::move(items)") != std::string::npos);
  assert(reload.find("pendingReady_.store(true") != std::string::npos);
  const size_t load = reload.find("M4xRegistry::load()");
  assert(load != std::string::npos);
  assert(load < reload.find("exitDisplayTask_", load));
  assert(reload.find("exitDisplayTask_", load) < reload.find("addBuiltin", load));
}

void testDrawerHasWifiTransfer(const std::string& source) {
  assert(source.find("BuiltinAction::FileTransfer") != std::string::npos);
  assert(source.find("onFileTransferOpen") != std::string::npos);
  assert(source.find("L(Str::kWifiTransfer)") != std::string::npos);
  assert(source.find("UIIcon::WifiTransfer") != std::string::npos);
  assert(source.find("WifiTransferIcon") != std::string::npos);
}

void testDrawerPagesWithScrollBar(const std::string& source) {
  assert(source.find("void AppListActivity::pageBy(const int pages)") != std::string::npos);
  assert(source.find("drawDrawerScrollBar") != std::string::npos);
  assert(source.find("M4ListTouchPolicy::applyPage") != std::string::npos);
  assert(source.find("SwipeDir::Up: pageBy(1)") != std::string::npos);
  assert(source.find("SwipeDir::Down: pageBy(-1)") != std::string::npos);
  assert(source.find("showScrollBar") != std::string::npos);
}

void testPluginEnterIsSerialized(const std::string& source) {
  const std::string body = functionBody(source, "void AppListActivity::openSelected()", "void AppListActivity::openInstall()");
  const size_t take = body.find("xSemaphoreTake(renderingMutex_");
  const size_t enter = body.find("enterNewActivity(");
  const size_t give = body.find("xSemaphoreGive(renderingMutex_)");
  assert(take != std::string::npos && enter != std::string::npos && give != std::string::npos);
  assert(take < give && give < enter);
  assert(body.find("childScreenOwned_.store(true") != std::string::npos);
  assert(body.find("M4xRuntimeKind::Native") != std::string::npos);
  assert(body.find("new NativeAppActivity") != std::string::npos);
}

}  // namespace

int main() {
  const std::string source = readAppListSource();
  assert(!source.empty());
  testWorkerNeverSubmits(source);
  testMainLoopSubmitsBeforeInput(source);
  testPluginEnterIsSerialized(source);
  testDrawerPagesWithScrollBar(source);
  testDrawerHasWifiTransfer(source);
  std::cout << "app drawer handoff contracts: ALL PASS\n";
  return 0;
}
