#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <climits>

#include "activities/home/HomeSceneAssetDecoder.h"
#include "ui/pages/HomeSceneModel.h"

using namespace HomeScene;
using namespace HomeSceneAssetDecoder;

// Helper to build minimal BMP
static std::vector<uint8_t> makeBmp(uint16_t w, uint16_t h, bool topDown, uint8_t bpp, uint32_t comp) {
  uint32_t rowBytes = (static_cast<size_t>(w) * bpp + 31) / 32 * 4;
  size_t pixelData = rowBytes * h;
  uint32_t offBits = 14+40+ (bpp<=8 ? (1u<<bpp)*4 : 0);
  uint32_t fileSize = offBits + pixelData;
  std::vector<uint8_t> out(fileSize, 0);
  out[0]='B'; out[1]='M';
  out[2]=fileSize &0xFF; out[3]=(fileSize>>8)&0xFF; out[4]=(fileSize>>16)&0xFF; out[5]=(fileSize>>24)&0xFF;
  out[10]=offBits &0xFF; out[11]=(offBits>>8)&0xFF; out[12]=(offBits>>16)&0xFF; out[13]=(offBits>>24)&0xFF;
  out[14]=40;
  out[18]= w &0xFF; out[19]=(w>>8)&0xFF;
  int32_t rawH = topDown ? -static_cast<int32_t>(h) : static_cast<int32_t>(h);
  out[22]= rawH &0xFF; out[23]=(rawH>>8)&0xFF; out[24]=(rawH>>16)&0xFF; out[25]=(rawH>>24)&0xFF;
  out[26]=1; out[28]=bpp; out[30]= comp &0xFF;
  // palette for 1bpp
  if (bpp==1) {
    size_t palOff=14+40;
    out[palOff+0]=0; out[palOff+1]=0; out[palOff+2]=0;
    out[palOff+4]=255; out[palOff+5]=255; out[palOff+6]=255;
  }
  for (size_t y=0;y<h;++y) {
    uint8_t* row = out.data()+offBits+ y*rowBytes;
    for (size_t i=0;i<rowBytes;++i) row[i]=0xAA;
  }
  return out;
}

static std::vector<uint8_t> make1BitBmp(uint16_t w,uint16_t h,bool topDown=false) {
  return makeBmp(w,h,topDown,1,0);
}

void testRejectInt32MinHeight() {
  auto bmp = make1BitBmp(kHomeCurrentCoverW, kHomeCurrentCoverH);
  // Patch rawH to INT32_MIN
  bmp[22]=0x00; bmp[23]=0x00; bmp[24]=0x00; bmp[25]=0x80; // 0x80000000
  std::vector<uint8_t> out(kHomeCurrentCoverBytes,0);
  bool ok = decodeBmpBytesTo1Bit(bmp.data(), bmp.size(), out.data(), kHomeCurrentCoverW, kHomeCurrentCoverH, kHomeCurrentCoverStride);
  assert(!ok && "INT32_MIN height must be rejected to avoid overflow");
}

void testExplicitWidthHeightLimits() {
  // Create BMP claiming huge dimensions (e.g., 2000x2000) but file truncated to correct exp; should reject before reading.
  // We use exp 110x180 but craft BMP with w=2000.
  auto bmp = makeBmp(2000,2000,false,1,0);
  std::vector<uint8_t> out(kHomeCurrentCoverBytes,0);
  bool ok = decodeBmpBytesTo1Bit(bmp.data(), bmp.size(), out.data(), kHomeCurrentCoverW, kHomeCurrentCoverH, kHomeCurrentCoverStride);
  assert(!ok && "huge dimensions must be rejected by explicit limits");
  // Also test w=1025 just over limit (exp still 110, but w=1025 will be rejected due to limit 1024)
  auto bmp2 = makeBmp(1025,180,false,1,0);
  // Need to ensure file size matches 1025 width, but decode should reject due to w limit before exp mismatch.
  // Since limit is 1024, 1025 >1024 => reject.
  bool ok2 = decodeBmpBytesTo1Bit(bmp2.data(), bmp2.size(), out.data(), kHomeCurrentCoverW, kHomeCurrentCoverH, kHomeCurrentCoverStride);
  assert(!ok2);
}

void testCheckedMultiplicationStrideOverflow() {
  // Craft BMP with bpp=32 and w=110, h=180 correct, but corrupt biSize to cause overflow? Instead test rowBytes overflow via large w*bpp.
  // Use w=110, bpp=32 => rowBytes = (110*32+31)/32*4 = 440. Not overflow.
  // To test overflow detection, we need w that causes w*bpp overflow size_t? But w limited to 1024, so w*bpp max 32768, not overflow.
  // Instead we test that malformed bfOffBits + rowBytes*h overflow is caught: create BMP with bfOffBits near max.
  auto bmp = make1BitBmp(kHomeCurrentCoverW, kHomeCurrentCoverH);
  // Set bfOffBits to 0xFFFFFF00 (huge) but keep file size small => should reject due to bfOffBits > bmpLen or overflow.
  bmp[10]=0x00; bmp[11]=0xFF; bmp[12]=0xFF; bmp[13]=0xFF;
  std::vector<uint8_t> out(kHomeCurrentCoverBytes,0);
  bool ok = decodeBmpBytesTo1Bit(bmp.data(), bmp.size(), out.data(), kHomeCurrentCoverW, kHomeCurrentCoverH, kHomeCurrentCoverStride);
  assert(!ok);
}

void testShortRows() {
  auto bmp = make1BitBmp(kHomeCurrentCoverW, kHomeCurrentCoverH);
  // Truncate file by 10 bytes (short rows)
  std::vector<uint8_t> truncated(bmp.size()-10);
  memcpy(truncated.data(), bmp.data(), truncated.size());
  std::vector<uint8_t> out(kHomeCurrentCoverBytes,0);
  bool ok = decodeBmpBytesTo1Bit(truncated.data(), truncated.size(), out.data(), kHomeCurrentCoverW, kHomeCurrentCoverH, kHomeCurrentCoverStride);
  assert(!ok && "short rows must be rejected");
}

void testMalformedSeparatorsAndPathTraversal() {
  // Valid icon path
  assert(resolveAppIconPath("/apps/com.example", "icon.bmp") == "/apps/com.example/icon.bmp");
  assert(resolveAppIconPath("/apps/com.example/", "sub/icon.bmp") == "/apps/com.example/sub/icon.bmp");
  // Traversal
  assert(resolveAppIconPath("/apps/com.example", "../evil.bmp") == "");
  assert(resolveAppIconPath("/apps/com.example", "a/../../b") == "");
  assert(resolveAppIconPath("/apps/com.example", "/etc/passwd") == "");
  assert(resolveAppIconPath("/apps/com.example", "a//b.bmp") == "");
  assert(resolveAppIconPath("/apps/com.example", "a\\b.bmp") == "");
  assert(resolveAppIconPath("/apps/com.example", "a:b.bmp") == "");
  assert(resolveAppIconPath("/apps/com.example", "") == "");
  assert(resolveAppIconPath("", "icon.bmp") == "");
  // Control char
  std::string ctrl = std::string("icon") + char(1) + ".bmp";
  assert(resolveAppIconPath("/apps/com.example", ctrl) == "");
  // Constrain beneath installPath: installPath itself must be validated
  assert(resolveAppIconPath("/apps/com.example/../evil", "icon.bmp") == "");
  assert(resolveAppIconPath("/apps//com.example", "icon.bmp") == "");
  // Long path
  std::string longIcon(100,'a');
  longIcon += ".bmp";
  assert(resolveAppIconPath("/apps/com.example", longIcon).size() == 0 || resolveAppIconPath("/apps/com.example", longIcon).size() <=200);
  std::string tooLong(201,'a');
  assert(resolveAppIconPath("/apps/com.example", tooLong) == "");
  // Beneath check: resolved must be under base
  auto full = resolveAppIconPath("/apps/com.example", "icon.bmp");
  assert(full.rfind("/apps/com.example/",0)==0);
  // Try to escape via subdir with ".." component hidden
  assert(resolveAppIconPath("/apps/com.example", "sub/../icon.bmp") == "");
}

void testGracefulPlaceholderFallback() {
  HomeScenePublication pub{};
  pub.snapshot.state = UiScene::DataState::Ready;
  UiScene::AssetKey key{ kBindingItemIcon, kBindingApps, 0 };
  // Invalid icon field should degrade to missing asset, not crash, and homeAdd should not add.
  bool ok = decodeAppIconForPublication(pub, "/apps/com.example", "../evil.bmp", key);
  assert(!ok);
  assert(pub.assetCount==0);
  // Malformed BMP should also degrade
  auto bmp = makeBmp(kHomeAppIconW, kHomeAppIconH, false, 16, 0); // unsupported bpp 16
  std::vector<uint8_t> out(kHomeAppIconBytes,0);
  bool bmpOk = decodeBmpBytesTo1Bit(bmp.data(), bmp.size(), out.data(), kHomeAppIconW, kHomeAppIconH, kHomeAppIconStride);
  assert(!bmpOk);
}

void testCoverPathHardening() {
  HomeScenePublication pub{};
  UiScene::AssetKey key{ kBindingCurrentCover, UiScene::kInvalidBindingId, UiScene::kInvalidAssetItemIndex };
  // Path with traversal should be rejected gracefully (return false, no asset added)
  bool ok = decodeCoverForPublication(pub, "/evil/../cover.bmp", key);
  assert(!ok);
  assert(pub.assetCount==0);
  bool ok2 = decodeCoverForPublication(pub, "/valid/path/cover.bmp", key);
  // On host, decodeCover will fail because no file, but should not crash and should degrade (return false)
  assert(!ok2);
}

int main() {
  testRejectInt32MinHeight();
  testExplicitWidthHeightLimits();
  testCheckedMultiplicationStrideOverflow();
  testShortRows();
  testMalformedSeparatorsAndPathTraversal();
  testGracefulPlaceholderFallback();
  testCoverPathHardening();
  return 0;
}
