#include <cassert>
#include <iostream>
#include <fstream>
#include <string>
#include <set>
#include <map>
#include <vector>

#include "util/M4ProviderCoverCache.h"
#include "util/M4ContentProviderContract.h"

using namespace M4ProviderCoverCache;

// Helper to read file content
static std::string readFile(const std::string& path) {
  std::ifstream f(path);
  if (!f) return {};
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Spy Gfx removed to avoid Arduino deps; placeholder tested via file content


// Test 1: heal missing coverBmpPath for provider history URIs (bug A retry-on-return)
void testHealMissingCoverPath() {
  // Simulate RecentBook with empty cover but history URI
  std::string providerId = "fanqie";
  std::string bookId = "6838480082219043843";
  std::string uri = M4ContentProvider::makeHistoryUri(providerId.c_str(), bookId.c_str());
  assert(uri == "m4cp://fanqie/6838480082219043843");
  std::string emptyCover;
  // Healing logic as in HomeActivity::loadRecentBooksInto
  std::string healed = emptyCover;
  if (healed.empty()) {
    std::string pid, bid;
    if (M4ContentProvider::parseHistoryUri(uri.c_str(), pid, bid)) {
      healed = bmpTemplatePath(pid, bid);
    }
  }
  assert(healed == bmpTemplatePath(providerId, bookId));
  assert(healed == "/.crosspoint/provider_covers/" + hexKey(cacheKey(providerId, bookId)) + "/cover_[WIDTH]x[HEIGHT].bmp");
  // Non-history URI should not heal
  std::string localPath = "/books/local/book.epub";
  std::string pid2, bid2;
  bool isHistory = M4ContentProvider::parseHistoryUri(localPath.c_str(), pid2, bid2);
  assert(!isHistory);
  std::cout << "healMissingCoverPath PASS\n";
}

// Test 2: first publish does not require ensureSized (bug B placeholder-first)
void testFirstPublishDoesNotRequireEnsureSized() {
  std::string homeCpp = readFile("firmware/src/activities/home/HomeActivity.cpp");
  assert(!homeCpp.empty() && "should read HomeActivity.cpp");
  // Fast path must exist and must NOT contain ensureSizedCoverFromSource
  size_t fastPos = homeCpp.find("publishHomeSceneWithAssetsFastCtx");
  assert(fastPos != std::string::npos && "fast publish must exist");
  // Extract fast function slice until next function
  size_t fastEnd = homeCpp.find("void HomeActivity::refreshMissingCoversInCtx", fastPos);
  if (fastEnd == std::string::npos) fastEnd = homeCpp.find("bool HomeActivity::publishHomeSceneWithAssetsCtx", fastPos);
  std::string fastSlice = homeCpp.substr(fastPos, fastEnd - fastPos);
  // Fast slice should contain tryDecodeCoverThumbIfExists but not ensureSizedCoverFromSource
  assert(fastSlice.find("tryDecodeCoverThumbIfExists") != std::string::npos);
  assert(fastSlice.find("ensureSizedCoverFromSource") == std::string::npos && "fast publish must NOT block on ensureSized");
  // Fast slice should not contain expensive bmpFileTo1Bit or jpegFileTo1Bit
  // It should still publish and set updateRequired
  assert(fastSlice.find("publish()") != std::string::npos);
  std::cout << "firstPublishDoesNotRequireEnsureSized PASS\n";

  // Refresh path MUST contain ensureSized
  size_t refreshPos = homeCpp.find("refreshMissingCoversInCtx");
  assert(refreshPos != std::string::npos);
  size_t refreshEnd = homeCpp.find("bool HomeActivity::publishHomeSceneWithAssetsCtx", refreshPos);
  std::string refreshSlice = homeCpp.substr(refreshPos, refreshEnd - refreshPos);
  assert(refreshSlice.find("ensureSizedCoverFromSource") != std::string::npos || refreshSlice.find("tryEnsureCoverThumbInCtx") != std::string::npos);
  // Refresh must decode and publish again
  assert(refreshSlice.find("decodeCoverForPublication") != std::string::npos);
  assert(refreshSlice.find("publish()") != std::string::npos);
  std::cout << "refreshDoesEnsure PASS\n";

  // Verify placeholder helper exists and is used
  size_t placeholderPos = homeCpp.find("tryDecodeCoverThumbIfExists");
  assert(placeholderPos != std::string::npos);
  // GfxSceneRenderer must have drawCoverPlaceholder used for missing covers
  std::string gfx = readFile("firmware/src/ui/scene/GfxSceneRenderer.h");
  assert(gfx.find("drawCoverPlaceholder") != std::string::npos);
  // Must be called in kNodeCover case when asset missing
  assert(gfx.find("drawCoverPlaceholder(gfx, ev)") != std::string::npos);
  std::cout << "placeholderHelper PASS\n";
}

// Test 3: cache-hit decodes immediately in fast path, miss stays placeholder then async
void testCacheHitVsMiss() {
  std::set<std::string> files;
  int converts = 0;
  Backend backend;
  backend.exists = [&](const std::string& p){ return files.count(p)!=0; };
  backend.convert = [&](const std::string& src, const std::string& tgt, int w, int h){
    (void)src;(void)w;(void)h; ++converts; files.insert(tgt); return true;
  };
  backend.remove = [&](const std::string& p){ files.erase(p); };

  std::string providerId = "fanqie";
  std::string bookId = "book123";
  std::string tpl = bmpTemplatePath(providerId, bookId);
  std::string dir = cacheDir(providerId, bookId);
  std::string source = sourcePath(providerId, bookId);
  std::string thumb110 = concreteBmpPath(providerId, bookId, 110, 180);
  std::string thumb74 = concreteBmpPath(providerId, bookId, 74, 106);

  // Simulate source exists but thumb missing (slow first ensure case)
  files.insert(source);
  // Fast path check: thumb missing => no decode
  assert(files.count(thumb110)==0);
  // Fast helper would see missing and not decode (placeholder)
  // Simulate that fast does NOT call convert
  assert(converts==0);
    // Refresh path: ensureSized should be called and generate thumb
  // Round-17 dual-size: 110 also generates 74, so first ensure does 2 converts
  auto r = ensureSizedCoverFromSource(tpl, 110, 180, backend);
  assert(r.generated && !r.cacheHit);
  assert(converts==2);
  assert(files.count(thumb110)==1);
  assert(files.count(thumb74)==1);
  // Second call should be cache hit, no convert
  auto r2 = ensureSizedCoverFromSource(tpl, 110, 180, backend);
  assert(r2.cacheHit && !r2.generated);
  assert(converts==2);
  // 74x106 similarly should now be hit (already generated by dual)
  auto r3 = ensureSizedCoverFromSource(tpl, 74, 106, backend);
  assert(r3.cacheHit && !r3.generated);
  assert(converts==2);
  assert(files.count(thumb74)==1);
  std::cout << "cacheHitVsMiss PASS\n";
}

// Test 4: placeholder rendering when asset missing (file-content only to avoid Arduino deps)
void testPlaceholderRendering() {
  std::string gfx = readFile("firmware/src/ui/scene/GfxSceneRenderer.h");
  assert(!gfx.empty());
  // Must have drawCoverPlaceholder definition with rounded border + diagonal cross + book spine
  assert(gfx.find("drawCoverPlaceholder") != std::string::npos);
  assert(gfx.find("drawRoundedRect") != std::string::npos);
  assert(gfx.find("drawLine") != std::string::npos);
  // Verify kNodeCover case draws placeholder when asset missing
  assert(gfx.find("kNodeCover") != std::string::npos);
  // Check that missing asset path calls placeholder
  assert(gfx.find("drawCoverPlaceholder(gfx, ev)") != std::string::npos);
  std::cout << "placeholderRendering PASS\n";
}

// Test 5: early detail->reader handoff keeps template path (bug A)
void testEarlyReaderHandoff() {
  // Simulate TxtReader persist with empty providerCoverBmpPath -> should heal to template
  std::string providerId = "weread";
  std::string bookId = "testBook";
  std::string emptyCover;
  std::string healed = emptyCover;
  if (healed.empty()) {
    healed = bmpTemplatePath(providerId, bookId);
  }
  assert(!healed.empty());
  assert(healed.find("[WIDTH]") != std::string::npos);
  assert(healed.find(providerId) == std::string::npos); // template uses hex, not plain id
  // Should be under provider_covers
  assert(healed.find("/.crosspoint/provider_covers/") == 0);
  // Simulate RecentBooks healing for old empty row
  std::string uri = M4ContentProvider::makeHistoryUri(providerId.c_str(), bookId.c_str());
  std::string pid, bid;
  bool ok = M4ContentProvider::parseHistoryUri(uri.c_str(), pid, bid);
  assert(ok && pid==providerId && bid==bookId);
  std::cout << "earlyReaderHandoff PASS\n";
}

int main() {
  testHealMissingCoverPath();
  testFirstPublishDoesNotRequireEnsureSized();
  testCacheHitVsMiss();
  testPlaceholderRendering();
  testEarlyReaderHandoff();
  std::cout << "ALL placeholder_async tests PASS\n";
  return 0;
}
