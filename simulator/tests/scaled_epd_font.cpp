#include "NativeGridEpdFont.h"
#include "ScaledEpdFont.h"
#include "util/M4FontPolicy.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

[[noreturn]] void fail(const char* msg) {
  std::cerr << "scaled_epd_font FAIL: " << msg << "\n";
  std::exit(1);
}

#define CHECK(cond) \
  do {              \
    if (!(cond)) fail(#cond); \
  } while (0)

uint8_t expectedDim(uint8_t v, float scale) {
  if (v == 0) return 0;
  int n = static_cast<int>(std::lround(v * scale));
  if (n < 1) n = 1;
  if (n > 255) n = 255;
  return static_cast<uint8_t>(n);
}

bool bitmapHasInk(const uint8_t* bmp, uint32_t dataLength) {
  if (!bmp || dataLength == 0) return false;
  for (uint32_t i = 0; i < dataLength; ++i) {
    if (bmp[i] != 0) return true;
  }
  return false;
}

std::vector<uint8_t> readFile(const char* path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) fail("cannot open native-grid blob");
  in.seekg(0, std::ios::end);
  const std::streamoff n = in.tellg();
  if (n <= 0) fail("empty native-grid blob");
  in.seekg(0, std::ios::beg);
  std::vector<uint8_t> buf(static_cast<size_t>(n));
  in.read(reinterpret_cast<char*>(buf.data()), n);
  if (!in) fail("short native-grid blob read");
  return buf;
}

}  // namespace

static_assert(M4FontPolicy::kNativeGridSourcePx == 16,
              "native-grid system face is a 16-row raster");
static_assert(M4FontPolicy::kCanonicalEpdfontPixelSize == 16,
              "canonical SD epdfont is a 16px artifact");

int main(int argc, char** argv) {
  // Reader scaler divides by the builtin 16px raster. Chrome IDs are never
  // scaled; this test covers reader NOTOSANS_16 sizing only.
  CHECK(M4FontPolicy::systemReaderSourcePx() == M4FontPolicy::kNativeGridSourcePx);
  CHECK(M4FontPolicy::systemReaderSourcePx() == M4FontPolicy::kCanonicalEpdfontPixelSize);
  CHECK(M4FontPolicy::systemReaderSourcePx() == 16);

  const char* path = argc > 1 ? argv[1] : "../firmware/src/fontdata/m4_native_grid_15x16.bin";
  const std::vector<uint8_t> blob = readFile(path);
  NativeGridEpdFont source;
  CHECK(source.bind(blob.data(), blob.size()));
  CHECK(source.valid());

  ScaledEpdFont scaled;
  static constexpr uint32_t kProbe = 0x4E2D;  // 中
  const EpdGlyph* srcGlyph = source.getGlyph(kProbe);
  CHECK(srcGlyph != nullptr);
  CHECK(srcGlyph->width > 0);
  CHECK(srcGlyph->height > 0);
  CHECK(srcGlyph->advanceX > 0);
  // NativeGridEpdFont scratch glyphs are a small ring; copy metrics before
  // later getGlyph calls rotate that ring.
  const uint8_t srcW = srcGlyph->width;
  const uint8_t srcH = srcGlyph->height;
  const uint8_t srcAX = srcGlyph->advanceX;

  const EpdFontData* srcData = source.getData();
  CHECK(srcData != nullptr);
  CHECK(srcData->is2Bit == false);
  CHECK(srcData->advanceY == 16);
  const uint8_t srcAdvanceY = srcData->advanceY;
  const int srcAscender = srcData->ascender;

  const float sourcePx = static_cast<float>(M4FontPolicy::systemReaderSourcePx());
  static constexpr int kSizes[] = {14, 15, 16, 17, 20, 22};
  int prevAdvance = 0;
  int prevLine = 0;
  for (int px : kSizes) {
    const float scale = static_cast<float>(px) / sourcePx;
    scaled.bind(&source, scale);
    CHECK(std::fabs(scaled.scale() - scale) < 0.0001f);

    const EpdFontData* data = scaled.getData();
    CHECK(data != nullptr);
    CHECK(data->is2Bit == false);
    CHECK(data->advanceY == expectedDim(srcAdvanceY, scale));
    CHECK(data->ascender == static_cast<int>(std::lround(srcAscender * scale)));

    const EpdGlyph* glyph = scaled.getGlyph(kProbe);
    CHECK(glyph != nullptr);
    CHECK(glyph->width == expectedDim(srcW, scale));
    CHECK(glyph->height == expectedDim(srcH, scale));
    CHECK(glyph->advanceX == expectedDim(srcAX, scale));

    if (px == 16) {
      CHECK(scaled.isUnityScale());
      CHECK(glyph->width == srcW);
      CHECK(glyph->height == srcH);
      CHECK(glyph->advanceX == srcAX);
      CHECK(data->advanceY == srcAdvanceY);
    } else {
      CHECK(!scaled.isUnityScale());
      CHECK(glyph->width != srcW || glyph->height != srcH);
    }

    const uint8_t* bmp = scaled.loadGlyphBitmap(glyph, nullptr);
    CHECK(bmp != nullptr);
    CHECK(bitmapHasInk(bmp, glyph->dataLength));

    if (prevAdvance != 0) {
      CHECK(glyph->advanceX > prevAdvance);
      CHECK(data->advanceY > prevLine);
    }
    prevAdvance = glyph->advanceX;
    prevLine = data->advanceY;

    std::cout << "scaled native-grid @" << px << "px advance=" << static_cast<int>(glyph->advanceX)
              << " line=" << static_cast<int>(data->advanceY)
              << " glyph=" << static_cast<int>(glyph->width) << "x"
              << static_cast<int>(glyph->height) << "\n";
  }

  std::cout << "scaled EpdFont native-grid reader metrics: PASS\n";
  return 0;
}
