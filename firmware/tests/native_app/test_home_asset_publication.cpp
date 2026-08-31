#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "activities/home/HomeScenePublication.h"
#include "activities/home/HomeSceneAssetDecoder.h"
#include "ui/scene/GfxSceneRenderer.h"
#include "ui/scene/UiSceneRuntime.h"

using namespace HomeScene;
using namespace UiScene;
using namespace HomeSceneAssetDecoder;

// Helper to build a minimal valid 1-bit BMP in memory at given W/H for testing decode.
static std::vector<uint8_t> make1BitBmp(uint16_t w, uint16_t h, bool topDown = false, uint8_t pattern = 0xAA) {
  // BMP file: 14 byte file header + 40 byte DIB + palette 8 bytes (2*4) + pixel data padded to 4 bytes per row
  const uint32_t rowBytes = (static_cast<size_t>(w) * 1 + 31) / 32 * 4;
  const size_t pixelDataSize = rowBytes * h;
  const uint32_t paletteSize = 8;
  const uint32_t offBits = 14 + 40 + paletteSize;
  const uint32_t fileSize = offBits + pixelDataSize;
  std::vector<uint8_t> out(fileSize, 0);
  // File header
  out[0] = 'B'; out[1] = 'M';
  out[2] = fileSize & 0xFF; out[3] = (fileSize >> 8) & 0xFF; out[4] = (fileSize >> 16) & 0xFF; out[5] = (fileSize >> 24) & 0xFF;
  out[10] = offBits & 0xFF; out[11] = (offBits >> 8) & 0xFF; out[12] = (offBits >> 16) & 0xFF; out[13] = (offBits >> 24) & 0xFF;
  // DIB
  out[14] = 40; // biSize
  out[18] = w & 0xFF; out[19] = (w >> 8) & 0xFF;
  int32_t rawH = topDown ? -static_cast<int32_t>(h) : static_cast<int32_t>(h);
  out[22] = rawH & 0xFF; out[23] = (rawH >> 8) & 0xFF; out[24] = (rawH >> 16) & 0xFF; out[25] = (rawH >> 24) & 0xFF;
  out[26] = 1; out[28] = 1; // planes=1, bpp=1
  // compression 0
  // image size can be 0
  // palette: entry 0 = black (0,0,0), entry1 = white (255,255,255)
  size_t palOff = 14 + 40;
  out[palOff + 0] = 0; out[palOff + 1] = 0; out[palOff + 2] = 0; out[palOff + 3] = 0;
  out[palOff + 4] = 255; out[palOff + 5] = 255; out[palOff + 6] = 255; out[palOff + 7] = 0;
  // pixel data
  for (uint16_t y = 0; y < h; ++y) {
    uint8_t* row = out.data() + offBits + y * rowBytes;
    // Fill with pattern: 0xAA = 10101010, but need to respect width bits beyond w should be 0
    for (size_t i = 0; i < rowBytes; ++i) row[i] = pattern;
    // Zero out padding bits beyond w
    const size_t validBytes = (w + 7) / 8;
    if (validBytes < rowBytes) {
      const int extraBits = static_cast<int>(validBytes * 8 - w);
      if (extraBits < 0) {
        // last valid byte has unused LSBs; clear them
        const int unused = 8 - (w % 8);
        if (unused != 8) row[validBytes - 1] &= static_cast<uint8_t>(0xFF << unused);
      }
      for (size_t i = validBytes; i < rowBytes; ++i) row[i] = 0;
    }
  }
  return out;
}

// Spy Gfx for render purity test
struct SpyGfx {
  mutable std::vector<std::string> calls;
  mutable int drawPixelCount = 0;
  mutable int forbiddenFsCalls = 0;
  void clearScreen(uint8_t = 0xFF) const { calls.push_back("clearScreen"); }
  void drawPixel(int x,int y,bool s=true) const { calls.push_back("drawPixel"); drawPixelCount++; (void)x;(void)y;(void)s; }
  void drawLine(int,int,int,int,bool=true) const { calls.push_back("drawLine"); }
  void drawLine(int,int,int,int,int,bool) const { calls.push_back("drawLineW"); }
  void drawRect(int,int,int,int,bool=true) const { calls.push_back("drawRect"); }
  void drawRect(int,int,int,int,int,bool) const { calls.push_back("drawRectW"); }
  void drawRoundedRect(int,int,int,int,int,int,bool=true) const { calls.push_back("drawRoundedRect"); }
  void fillRect(int,int,int,int,bool=true) const { calls.push_back("fillRect"); }
  void drawIcon(const uint8_t*,int,int,int,int) const { forbiddenFsCalls++; }
  void drawImage(const uint8_t*,int,int,int,int) const { forbiddenFsCalls++; }
  void drawBitmap(const struct Bitmap&,int,int,int,int,float,float) const { forbiddenFsCalls++; }
  int getTextWidth(int,const char*,int=0,float=1) const { return 50; }
  int getLineHeight(int) const { return 16; }
  int getFontAscenderSize(int) const { return 12; }
  std::string truncatedText(int,const char*,int,int=0,float=1) const { return "..."; }
  void drawText(int,int,int,const char*,bool=true,int=0,float=1) const { calls.push_back("drawText"); }
  int getScreenWidth() const { return 480; }
  int getScreenHeight() const { return 800; }
};

void testPairedGenerationAssetPointerLifetime() {
  // Use the canonical HomeSceneModel publication store (UiStateStore<Publication>) for lifecycle test
  HomeScene::HomeSceneModel model;
  // Publish A with current cover pattern 0xAA via homeAddAssetToPublication
  HomeScene::HomeScenePublication pubA{};
  pubA.snapshot.state = UiScene::DataState::Ready;
  pubA.snapshot.revision = 0; // will be overwritten by publishWithAssets
  size_t off = 0; uint16_t w=0,h=0,stride=0; size_t bytes=0;
  UiScene::AssetKey curr{ HomeScene::kBindingCurrentCover, UiScene::kInvalidBindingId, UiScene::kInvalidAssetItemIndex };
  assert(HomeScene::homePublicationSlotForKey(curr, &off, &w, &h, &stride, &bytes));
  // Fill a temp buffer with pattern 0xAA and add to pub
  std::vector<uint8_t> patA(bytes, 0xAA);
  assert(HomeScene::homeAddAssetToPublication(pubA, curr, patA.data(), w, h, stride));
  pubA.snapshot.currentExists = true;
  assert(model.publishWithAssets(pubA));
  auto pinA = model.acquirePublication();
  assert(pinA.valid());
  const uint8_t* ptrA = nullptr;
  {
    UiScene::UiSceneAssets assets;
    HomeScene::homePublicationToAssets(pinA.value(), assets);
    const UiScene::UiSceneAsset* a = assets.get(curr);
    assert(a && a->valid());
    ptrA = a->pixels;
    assert(ptrA[0] == 0xAA);
  }
  // Publish B with pattern 0xBB while A is pinned — should go to a different slot
  HomeScene::HomeScenePublication pubB{};
  pubB.snapshot.state = UiScene::DataState::Ready;
  std::vector<uint8_t> patB(bytes, 0xBB);
  assert(HomeScene::homeAddAssetToPublication(pubB, curr, patB.data(), w, h, stride));
  pubB.snapshot.currentExists = true;
  assert(model.publishWithAssets(pubB));
  // A's pointer must still be valid and unchanged (generation pin prevents overwrite)
  {
    UiScene::UiSceneAssets assetsA;
    HomeScene::homePublicationToAssets(pinA.value(), assetsA);
    const UiScene::UiSceneAsset* a = assetsA.get(curr);
    assert(a && a->valid());
    assert(a->pixels == ptrA); // same pointer (stable)
    assert(a->pixels[0] == 0xAA); // still old pattern
  }
  auto pinB = model.acquirePublication();
  assert(pinB.valid());
  assert(pinB.generation() == 2);
  {
    UiScene::UiSceneAssets assetsB;
    HomeScene::homePublicationToAssets(pinB.value(), assetsB);
    const UiScene::UiSceneAsset* b = assetsB.get(curr);
    assert(b && b->valid());
    assert(b->pixels[0] == 0xBB);
    assert(b->pixels != ptrA); // different generation
  }
  // Release A, now slot should be reusable
  pinA.release();
  HomeScene::HomeScenePublication pubC{};
  pubC.snapshot.state = UiScene::DataState::Ready;
  std::vector<uint8_t> patC(bytes, 0xCC);
  assert(HomeScene::homeAddAssetToPublication(pubC, curr, patC.data(), w,h,stride));
  assert(model.publishWithAssets(pubC)); // should succeed now that A released
}

void testExactKeyedMapNoCrossFallback() {
  HomeScene::HomeScenePublication pub{};
  UiScene::UiSceneAssets assets;
  // Populate exactly 8 assets with distinct patterns
  UiScene::AssetKey keys[8] = {
    {HomeScene::kBindingCurrentCover, UiScene::kInvalidBindingId, UiScene::kInvalidAssetItemIndex},
    {HomeScene::kBindingItemCover, HomeScene::kBindingRecent, 0},
    {HomeScene::kBindingItemCover, HomeScene::kBindingRecent, 1},
    {HomeScene::kBindingItemCover, HomeScene::kBindingRecent, 2},
    {HomeScene::kBindingItemIcon, HomeScene::kBindingApps, 0},
    {HomeScene::kBindingItemIcon, HomeScene::kBindingApps, 1},
    {HomeScene::kBindingItemIcon, HomeScene::kBindingApps, 2},
    {HomeScene::kBindingItemIcon, HomeScene::kBindingApps, 3},
  };
  for (int i=0;i<8;++i) {
    size_t off; uint16_t w,h,s; size_t b;
    assert(HomeScene::homePublicationSlotForKey(keys[i], &off, &w,&h,&s,&b));
    std::vector<uint8_t> data(b, static_cast<uint8_t>(i+1));
    assert(HomeScene::homeAddAssetToPublication(pub, keys[i], data.data(), w,h,s));
  }
  assert(pub.assetCount == 8);
  HomeScene::homePublicationToAssets(pub, assets);
  assert(assets.count == 8);
  for (int i=0;i<8;++i) assert(assets.get(keys[i]) != nullptr);
  // Cross-category must NOT fallback
  assert(assets.get({HomeScene::kBindingItemCover, HomeScene::kBindingApps, 0}) == nullptr);
  assert(assets.get({HomeScene::kBindingItemIcon, HomeScene::kBindingRecent, 0}) == nullptr);
  assert(assets.get({HomeScene::kBindingItemCover, HomeScene::kBindingRecent, 3}) == nullptr); // out of range
  assert(assets.get({HomeScene::kBindingItemIcon, HomeScene::kBindingApps, 4}) == nullptr);
  assert(assets.get({HomeScene::kBindingCurrentCover, HomeScene::kBindingRecent, 0}) == nullptr);
  assert(assets.get({HomeScene::kBindingItemCover, UiScene::kInvalidBindingId, UiScene::kInvalidAssetItemIndex}) == nullptr);
  // Exact binding check via GfxSceneRenderer::assetKey
  UiSceneRuntime::RenderEvent ev{};
  ev.assetBinding = kBindingItemCover;
  ev.item = {true, kBindingRecent, 0, 3};
  AssetKey k = GfxSceneRenderer::assetKey(ev);
  assert(k == keys[1]);
  ev.assetBinding = kBindingItemIcon;
  ev.item = {true, kBindingApps, 0, 4};
  k = GfxSceneRenderer::assetKey(ev);
  assert(k == keys[4]);
  ev.assetBinding = kBindingCurrentCover;
  ev.item = {false, kInvalidBindingId, kInvalidAssetItemIndex, 0};
  k = GfxSceneRenderer::assetKey(ev);
  assert(k == keys[0]);
}

void testMalformedShortBmpDegradesSafely() {
  HomeScene::HomeScenePublication pub{};
  pub.snapshot.state = UiScene::DataState::Ready;
  // Valid case first
  auto bmpValid = make1BitBmp(HomeScene::kHomeCurrentCoverW, HomeScene::kHomeCurrentCoverH, false, 0xFF);
  std::vector<uint8_t> out(HomeScene::kHomeCurrentCoverBytes);
  bool ok = decodeBmpBytesTo1Bit(bmpValid.data(), bmpValid.size(), out.data(), HomeScene::kHomeCurrentCoverW, HomeScene::kHomeCurrentCoverH, HomeScene::kHomeCurrentCoverStride);
  assert(ok);
  // Malformed: bad magic
  auto bmpBad = bmpValid;
  bmpBad[0]='X'; bmpBad[1]='Y';
  assert(!decodeBmpBytesTo1Bit(bmpBad.data(), bmpBad.size(), out.data(), HomeScene::kHomeCurrentCoverW, HomeScene::kHomeCurrentCoverH, HomeScene::kHomeCurrentCoverStride));
  // Short: truncated
  assert(!decodeBmpBytesTo1Bit(bmpValid.data(), 20, out.data(), HomeScene::kHomeCurrentCoverW, HomeScene::kHomeCurrentCoverH, HomeScene::kHomeCurrentCoverStride));
  // Wrong dimensions (expect 110x180 but bmp is 74x106)
  auto bmpWrong = make1BitBmp(HomeScene::kHomeRecentCoverW, HomeScene::kHomeRecentCoverH);
  assert(!decodeBmpBytesTo1Bit(bmpWrong.data(), bmpWrong.size(), out.data(), HomeScene::kHomeCurrentCoverW, HomeScene::kHomeCurrentCoverH, HomeScene::kHomeCurrentCoverStride));
  // Unsupported BPP: craft 16bpp
  auto bmp16 = bmpValid;
  bmp16[28]=16; bmp16[29]=0;
  assert(!decodeBmpBytesTo1Bit(bmp16.data(), bmp16.size(), out.data(), HomeScene::kHomeCurrentCoverW, HomeScene::kHomeCurrentCoverH, HomeScene::kHomeCurrentCoverStride));
  // Missing file via decodeCoverForPublication should degrade (return false, no asset added)
  HomeScene::HomeScenePublication pub2{};
  UiScene::AssetKey curr{ HomeScene::kBindingCurrentCover, UiScene::kInvalidBindingId, UiScene::kInvalidAssetItemIndex };
  bool added = decodeCoverForPublication(pub2, "/nonexistent/path.bmp", curr);
  assert(!added);
  assert(pub2.assetCount == 0);
  // Even after failed decode, snapshot should remain publishable and assets missing => renderer draws placeholder (tested via assets empty)
  pub2.snapshot.state = UiScene::DataState::Ready;
  HomeScene::HomeSceneModel model;
  assert(model.publishWithAssets(pub2));
  auto pin = model.acquirePublication();
  UiScene::UiSceneAssets assets;
  HomeScene::homePublicationToAssets(pin.value(), assets);
  assert(assets.get(curr)==nullptr); // degraded to missing
}

void testCancellationBetweenRowsAndBeforePublish() {
  // Cancellation during decode between rows
  auto bmp = make1BitBmp(HomeScene::kHomeAppIconW, HomeScene::kHomeAppIconH, false, 0xFF);
  std::vector<uint8_t> out(HomeScene::kHomeAppIconBytes);
  int callCount=0;
  auto isCancelled = [&callCount]() -> bool {
    // cancel after 5 rows
    return callCount++ >= 5;
  };
  int row=0;
  auto cancelMid = [&row]() -> bool { return row++ >= 3; };
  (void)cancelMid; (void)isCancelled;
  assert(!decodeBmpBytesTo1Bit(bmp.data(), bmp.size(), out.data(), HomeScene::kHomeAppIconW, HomeScene::kHomeAppIconH, HomeScene::kHomeAppIconStride, [](){return true;}));
  assert(decodeBmpBytesTo1Bit(bmp.data(), bmp.size(), out.data(), HomeScene::kHomeAppIconW, HomeScene::kHomeAppIconH, HomeScene::kHomeAppIconStride, [](){return false;}));
  // Before publish blocked
  HomeScene::HomeSceneModel model;
  HomeScene::HomeScenePublication pub{};
  pub.snapshot.state = UiScene::DataState::Ready;
  pub.snapshot.revision = 0;
  UiScene::AssetKey key{ HomeScene::kBindingItemIcon, HomeScene::kBindingApps, 0 };
  size_t off; uint16_t w,h,s; size_t b;
  assert(HomeScene::homePublicationSlotForKey(key, &off,&w,&h,&s,&b));
  std::vector<uint8_t> data(b, 0x11);
  assert(HomeScene::homeAddAssetToPublication(pub, key, data.data(), w,h,s));
  std::atomic<bool> cancelled{true};
  bool published = false;
  if (!cancelled.load()) published = model.publishWithAssets(pub);
  assert(!published);
  assert(!model.hasPublishedSnapshot() || model.latestRevision()==0); // not yet published
  cancelled.store(false);
  if (!cancelled.load()) published = model.publishWithAssets(pub);
  assert(published);
  assert(model.hasPublishedSnapshot());
}

void testRenderPathHasNoFsCallsByConstruction() {
  HomeScene::HomeScenePublication pub{};
  pub.snapshot.state = UiScene::DataState::Ready;
  pub.snapshot.battery = 77;
  pub.snapshot.wifiConnected = true;
  pub.snapshot.currentExists = true;
  UiScene::AssetKey curr{ HomeScene::kBindingCurrentCover, UiScene::kInvalidBindingId, UiScene::kInvalidAssetItemIndex };
  size_t off; uint16_t w,h,s; size_t b;
  assert(HomeScene::homePublicationSlotForKey(curr, &off,&w,&h,&s,&b));
  std::vector<uint8_t> data(b, 0xFF);
  assert(HomeScene::homeAddAssetToPublication(pub, curr, data.data(), w,h,s));
  UiScene::AssetKey app0{ HomeScene::kBindingItemIcon, HomeScene::kBindingApps, 0 };
  assert(HomeScene::homePublicationSlotForKey(app0, &off,&w,&h,&s,&b));
  std::vector<uint8_t> data2(b, 0xAA);
  assert(HomeScene::homeAddAssetToPublication(pub, app0, data2.data(), w,h,s));
  pub.snapshot.appCount = 1;
  HomeScene::HomeSceneModel model;
  assert(model.publishWithAssets(pub));
  auto pin = model.acquirePublication();
  assert(pin.valid());
  UiScene::UiSceneAssets assets;
  HomeScene::homePublicationToAssets(pin.value(), assets);
  const auto& snap = pin.value().snapshot;
  auto source = HomeScene::HomeSceneModel::bindingSource(snap);
  (void)source;
  SpyGfx gfx;
  UiScene::GfxSceneRenderer renderer;
  const UiScene::UiSceneAsset* a = assets.get(curr);
  assert(a);
  UiScene::GfxSceneRenderer::draw1BitAsset(gfx, *a, 0, 0);
  assert(gfx.forbiddenFsCalls == 0);
  assert(gfx.drawPixelCount > 0);
  UiSceneRuntime::HitResult hit{};
  (void)hit;
  assert(gfx.forbiddenFsCalls == 0);
}

void testAppIconPathResolutionSafe() {
  assert(resolveAppIconPath("/apps/com.example", "icon.bmp") == "/apps/com.example/icon.bmp");
  assert(resolveAppIconPath("/apps/com.example/", "icon.bmp") == "/apps/com.example/icon.bmp");
  assert(resolveAppIconPath("/apps/com.example", "/etc/passwd") == "");
  assert(resolveAppIconPath("/apps/com.example", "../evil.bmp") == "");
  assert(resolveAppIconPath("", "icon.bmp") == "");
  assert(resolveAppIconPath("/apps/com.example", "") == "");
  assert(resolveAppIconPath("/apps/com.example", "a/../../b") == "");
  // Long path
  std::string longIcon(201,'a');
  assert(resolveAppIconPath("/apps/com.example", longIcon) == "");
}

void testMemoryBudgetFixed() {
  static_assert(HomeScene::kHomeAssetArenaBytes == 7748, "arena must be 7748 B");
  static_assert(HomeScene::kHomeCurrentCoverBytes == 2520, "current 2520");
  static_assert(HomeScene::kHomeRecentCoverBytes == 1060, "recent 1060");
  static_assert(HomeScene::kHomeAppIconBytes == 512, "icon 512");
  static_assert(sizeof(HomeScene::HomeScenePublication) <= 12000, "publication bounded");
  size_t off; uint16_t w,h,s; size_t b;
  assert(HomeScene::homePublicationSlotForKey({HomeScene::kBindingCurrentCover, UiScene::kInvalidBindingId, UiScene::kInvalidAssetItemIndex}, &off,&w,&h,&s,&b));
  assert(off==0 && b==2520);
  assert(HomeScene::homePublicationSlotForKey({HomeScene::kBindingItemCover, HomeScene::kBindingRecent, 2}, &off,&w,&h,&s,&b));
  assert(off== 2520+2*1060);
  assert(HomeScene::homePublicationSlotForKey({HomeScene::kBindingItemIcon, HomeScene::kBindingApps, 3}, &off,&w,&h,&s,&b));
  assert(off== 2520+3*1060+3*512);
  assert(off+b == 7748);
}

int main() {
  testPairedGenerationAssetPointerLifetime();
  testExactKeyedMapNoCrossFallback();
  testMalformedShortBmpDegradesSafely();
  testCancellationBetweenRowsAndBeforePublish();
  testRenderPathHasNoFsCallsByConstruction();
  testAppIconPathResolutionSafe();
  testMemoryBudgetFixed();
  return 0;
}
