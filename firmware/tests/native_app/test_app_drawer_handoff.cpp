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

void testDisplayTaskRechecksChildUnderMutex(const std::string& source) {
  const std::string body = functionBody(source, "void AppListActivity::displayTaskLoop()", "void AppListActivity::reload()");
  const size_t take = body.find("xSemaphoreTake(renderingMutex_, portMAX_DELAY);");
  const size_t childGate = body.find("!subActivity");
  const size_t clearUpdate = body.find("updateRequired_ = false;");
  const size_t paint = body.find("render();");

  // The child gate and the one-shot flag must be evaluated only after the
  // mutex handoff. Otherwise a task that observed no child can paint the old
  // drawer after openSelected() has installed NativeAppActivity.
  assert(take != std::string::npos);
  assert(childGate != std::string::npos && take < childGate);
  assert(clearUpdate != std::string::npos && take < clearUpdate);
  assert(paint != std::string::npos && clearUpdate < paint);
}

void testPluginEnterIsSerialized(const std::string& source) {
  const std::string body = functionBody(source, "void AppListActivity::openSelected()", "void AppListActivity::openInstall()");
  const size_t take = body.find("xSemaphoreTake(renderingMutex_, portMAX_DELAY);");
  const size_t enter = body.find("enterNewActivity(");
  const size_t give = body.find("xSemaphoreGive(renderingMutex_);");
  assert(take != std::string::npos && enter != std::string::npos && give != std::string::npos);
  assert(take < enter && enter < give);
  assert(body.find("M4xRuntimeKind::Native") != std::string::npos);
  assert(body.find("new NativeAppActivity") != std::string::npos);
}

}  // namespace

int main() {
  const std::string source = readAppListSource();
  assert(!source.empty());
  testDisplayTaskRechecksChildUnderMutex(source);
  testPluginEnterIsSerialized(source);
  std::cout << "app drawer handoff contracts: ALL PASS\n";
  return 0;
}
