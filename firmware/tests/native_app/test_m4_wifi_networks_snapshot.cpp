// M4 Wi-Fi settings — scan/result snapshot contract.
//
// RED until WifiSelectionActivity publishes scan results atomically:
// the display task renders `networks` under renderingMutex while the loop
// thread clears/rebuilds it. The contract pins:
//   * processWifiScanResults stages into a LOCAL vector and publishes with
//     swap under renderingMutex (no member clear/push_back/sort outside it),
//     and does no other work (e.g. auto-connect) while holding the lock;
//   * every loop-thread read of the shared list model goes through the same
//     lock (selectNetwork / checkConnectionStatus / startWifiScan / the
//     NETWORK_LIST input branch in loop());
//   * no function ever nests renderingMutex takes (non-recursive mutex).
//
// Build:
//   /opt/homebrew/bin/g++-14 -std=c++17 -I firmware/src \
//     firmware/tests/native_app/test_m4_wifi_networks_snapshot.cpp \
//     -o /tmp/test_m4_wifi_networks_snapshot && /tmp/test_m4_wifi_networks_snapshot
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

// Extract the braced body of `marker` (a function signature prefix).
// Returns empty when the signature or balanced close is missing.
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

// Max nesting depth of renderingMutex takes inside one function body.
// The mutex is non-recursive: depth must never exceed 1.
int maxTakeDepth(const std::string& body) {
  int depth = 0;
  int peak = 0;
  size_t i = 0;
  while (i < body.size()) {
    const size_t take = body.find("xSemaphoreTake(renderingMutex", i);
    const size_t give = body.find("xSemaphoreGive(renderingMutex", i);
    if (take == std::string::npos && give == std::string::npos) break;
    if (take != std::string::npos && (give == std::string::npos || take < give)) {
      if (++depth > peak) peak = depth;
      i = take + 1;
    } else {
      --depth;
      i = give + 1;
    }
  }
  return peak;
}

}  // namespace

int main() {
  const std::string src =
      readFile("firmware/src/activities/network/WifiSelectionActivity.cpp");
  assert(!src.empty());

  const std::string scanFn = bodyOf(src, "void WifiSelectionActivity::processWifiScanResults()");
  const std::string selectFn = bodyOf(src, "void WifiSelectionActivity::selectNetwork(");
  const std::string checkFn = bodyOf(src, "void WifiSelectionActivity::checkConnectionStatus()");
  const std::string startFn = bodyOf(src, "void WifiSelectionActivity::startWifiScan()");
  const std::string loopFn = bodyOf(src, "void WifiSelectionActivity::loop()");
  const std::string taskFn = bodyOf(src, "void WifiSelectionActivity::displayTaskLoop()");
  assert(!scanFn.empty() && !selectFn.empty() && !checkFn.empty());
  assert(!startFn.empty() && !loopFn.empty() && !taskFn.empty());

  // 1. Atomic publish: staged locally, swapped in under the mutex.
  assert(scanFn.find("networks.clear()") == std::string::npos);
  assert(scanFn.find("networks.push_back") == std::string::npos);
  assert(scanFn.find("networks.swap(") != std::string::npos);
  assert(scanFn.find("xSemaphoreTake(renderingMutex") != std::string::npos);
  assert(scanFn.find("xSemaphoreGive(renderingMutex") != std::string::npos);

  // 2. Nothing else runs under that lock: auto-connect happens after release.
  const size_t lastGive = scanFn.rfind("xSemaphoreGive(renderingMutex");
  const size_t autoConn = scanFn.find("maybeAutoConnectKnown();");
  assert(autoConn != std::string::npos && lastGive != std::string::npos);
  assert(autoConn > lastGive);

  // 3. selectNetwork copies its row under the lock, before any member read.
  {
    const size_t take = selectFn.find("xSemaphoreTake(renderingMutex");
    const size_t size = selectFn.find("networks.size()");
    const size_t elem = selectFn.find("networks[");
    assert(take != std::string::npos);
    if (size != std::string::npos) assert(take < size);
    if (elem != std::string::npos) assert(take < elem);
  }

  // 4. startWifiScan clears the shared model under the lock.
  {
    const size_t take = startFn.find("xSemaphoreTake(renderingMutex");
    const size_t clear = startFn.find("networks.clear()");
    assert(take != std::string::npos && clear != std::string::npos);
    assert(take < clear);
  }

  // 5. checkConnectionStatus mutates the saved-password flag under the lock.
  assert(checkFn.find("xSemaphoreTake(renderingMutex") != std::string::npos);

  // 6. The NETWORK_LIST input branch snapshots the model under the lock
  //    (distinctive snapshot locals, read before first member access).
  {
    const size_t snap = loopFn.find("snapScanned");
    const size_t size = loopFn.find("networks.size()");
    assert(snap != std::string::npos && size != std::string::npos);
    assert(snap < size);
    assert(loopFn.find("snapSelected") != std::string::npos);
  }

  // 7. Display path still renders under the mutex.
  {
    const size_t take = taskFn.find("xSemaphoreTake(renderingMutex");
    const size_t render = taskFn.find("render();");
    const size_t give = taskFn.find("xSemaphoreGive(renderingMutex");
    assert(take != std::string::npos && render != std::string::npos);
    assert(give != std::string::npos && take < render && render < give);
  }

  // 8. Takes/gives balance and never nest (non-recursive mutex).
  for (const std::string* fn : {&scanFn, &selectFn, &checkFn, &startFn, &loopFn}) {
    assert(countOcc(*fn, "xSemaphoreTake(renderingMutex") ==
           countOcc(*fn, "xSemaphoreGive(renderingMutex"));
    assert(maxTakeDepth(*fn) <= 1);
  }

  std::puts("WIFI_NETWORKS_SNAPSHOT_CONTRACT_OK");
  return 0;
}
