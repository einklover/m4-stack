#include <cassert>
#include <iostream>
#include <fstream>
#include <string>

static std::string readFile(const std::string& path) {
  std::ifstream f(path);
  if (!f) return {};
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Test 1: true async gap — fast publish then vTaskDelay then refresh
void testAsyncGap() {
  std::string cpp = readFile("firmware/src/activities/home/HomeActivity.cpp");
  assert(!cpp.empty());
  size_t pubPos = cpp.find("void HomeActivity::publishHomeSceneFromBackendCtx");
  assert(pubPos != std::string::npos);
  std::string slice = cpp.substr(pubPos, 8000);
  // Must contain fast publish
  assert(slice.find("publishHomeSceneWithAssetsFastCtx") != std::string::npos);
  // Must contain vTaskDelay between fast and refresh
  size_t fastPos = slice.find("publishHomeSceneWithAssetsFastCtx");
  size_t delayPos = slice.find("vTaskDelay");
  size_t refreshPos = slice.find("refreshMissingCoversInCtx");
  assert(delayPos != std::string::npos && "must have vTaskDelay for true async gap");
  assert(refreshPos != std::string::npos);
  assert(fastPos < delayPos && delayPos < refreshPos && "delay must be between fast publish and refresh");
  // Must check cancelled after delay
  std::string between = slice.substr(delayPos, refreshPos - delayPos);
  assert(between.find("isCancelled()") != std::string::npos);
  std::cout << "asyncGap PASS\n";
}

// Test 2: policy A — refresh can acquire when URL+wifi allow, Wi-Fi gated, cancel respected
void testPolicyAAcquire() {
  std::string cpp = readFile("firmware/src/activities/home/HomeActivity.cpp");
  assert(!cpp.empty());
  size_t refreshPos = cpp.find("void HomeActivity::refreshMissingCoversInCtx");
  assert(refreshPos != std::string::npos);
  std::string refresh = cpp.substr(refreshPos, 8000);
  // Must contain acquireProviderCover
  assert(refresh.find("acquireProviderCover") != std::string::npos);
  // Must be Wi-Fi gated
  assert(refresh.find("homeWifiConnected") != std::string::npos);
  // Must respect cancel
  assert(refresh.find("isCancelled()") != std::string::npos);
  // Must cap hero + 3 minis (loop itemIndex < 3)
  assert(refresh.find("itemIndex < 3") != std::string::npos);
  // Must parse history URI for providerId/bookId
  assert(refresh.find("parseHistoryUri") != std::string::npos);
  // Must not be in fast publish
  size_t fastPos = cpp.find("bool HomeActivity::publishHomeSceneWithAssetsFastCtx");
  size_t fastEnd = cpp.find("void HomeActivity::refreshMissingCoversInCtx", fastPos);
  std::string fastSlice = cpp.substr(fastPos, fastEnd - fastPos);
  assert(fastSlice.find("acquireProviderCover") == std::string::npos && "fast publish must NOT call acquire");
  std::cout << "policyAAcquire PASS\n";
}

// Test 3: coverUrl resolution helpers exist and prioritize shelf then detail
void testCoverUrlResolvers() {
  std::string cpp = readFile("firmware/src/activities/home/HomeActivity.cpp");
  assert(!cpp.empty());
  // Must have resolveCoverUrlFromShelf and resolveCoverUrlViaDetail
  assert(cpp.find("resolveCoverUrlFromShelf") != std::string::npos);
  assert(cpp.find("resolveCoverUrlViaDetail") != std::string::npos);
  assert(cpp.find("resolveCoverUrlForHistory") != std::string::npos);
  // Shelf resolver must read shelf_rows.tsv
  assert(cpp.find("shelf_rows.tsv") != std::string::npos);
  // Detail resolver must call M4NativeProviderBookDetail::fetch
  assert(cpp.find("M4NativeProviderBookDetail::fetch") != std::string::npos);
  // Must handle legado proxy
  assert(cpp.find("coverProxyUrl") != std::string::npos);
  // Must include necessary headers
  assert(cpp.find("M4NativeProviderBookDetail.h") != std::string::npos);
  assert(cpp.find("M4LegadoBridge.h") != std::string::npos);
  assert(cpp.find("M4QemuNet.h") != std::string::npos);
  std::cout << "coverUrlResolvers PASS\n";
}

// Test 4: fast publish still does not block on ensureSized (re-check after policy A)
void testFastStillNonBlocking() {
  std::string cpp = readFile("firmware/src/activities/home/HomeActivity.cpp");
  size_t fastPos = cpp.find("bool HomeActivity::publishHomeSceneWithAssetsFastCtx");
  size_t fastEnd = cpp.find("void HomeActivity::refreshMissingCoversInCtx", fastPos);
  std::string fastSlice = cpp.substr(fastPos, fastEnd - fastPos);
  assert(fastSlice.find("tryDecodeCoverThumbIfExists") != std::string::npos);
  assert(fastSlice.find("ensureSizedCoverFromSource") == std::string::npos);
  assert(fastSlice.find("acquireProviderCover") == std::string::npos);
  std::cout << "fastStillNonBlocking PASS\n";
}

int main() {
  testAsyncGap();
  testPolicyAAcquire();
  testCoverUrlResolvers();
  testFastStillNonBlocking();
  std::cout << "ALL async_acquire tests PASS\n";
  return 0;
}
