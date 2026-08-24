#include "NativeGridEpdFont.h"
#include "fontdata/m4_native_grid_15x16.h"
#include "util/M4FontPolicy.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

[[noreturn]] void fail(const char* msg) {
  std::cerr << "native_grid_font FAIL: " << msg << "\n";
  std::exit(1);
}

#define CHECK(cond) \
  do {              \
    if (!(cond)) fail(#cond); \
  } while (0)

bool bitmapHasInk(const uint8_t* bmp, uint32_t n) {
  if (!bmp || n == 0) return false;
  for (uint32_t i = 0; i < n; ++i) {
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

int main(int argc, char** argv) {
  CHECK(M4NativeGridFont::kGlyphCount == 28953);
  CHECK(M4NativeGridFont::kOutlierCount == 32);
  CHECK(M4NativeGridFont::kGridWidth == 15);
  CHECK(M4NativeGridFont::kGridHeight == 16);
  CHECK(M4NativeGridFont::kSourcePx == 16);
  CHECK(M4FontPolicy::kNativeGridSourcePx == 16);
  CHECK(M4FontPolicy::systemReaderSourcePx() == M4NativeGridFont::kSourcePx);
  CHECK(M4FontPolicy::systemReaderSourcePx() == 16);

  const char* path = argc > 1 ? argv[1] : "../firmware/src/fontdata/m4_native_grid_15x16.bin";
  const std::vector<uint8_t> blob = readFile(path);
  CHECK(blob.size() == M4NativeGridFont::kBlobBytes);
  CHECK(blob.size() >= 4 && blob[0] == 'M' && blob[1] == '4' && blob[2] == 'N' && blob[3] == 'G');

  NativeGridEpdFont font;
  CHECK(font.bind(blob.data(), blob.size()));
  CHECK(font.valid());
  CHECK(font.glyphCount() == M4NativeGridFont::kGlyphCount);
  CHECK(font.outlierCount() == M4NativeGridFont::kOutlierCount);

  const EpdFontData* data = font.getData();
  CHECK(data != nullptr);
  CHECK(data->is2Bit == false);
  CHECK(data->glyph == nullptr);
  CHECK(data->intervals == nullptr);
  CHECK(data->intervalCount == 0);
  CHECK(data->advanceY == 16);
  CHECK(data->ascender == 16);

  static constexpr uint32_t kCommon[] = {
      0x4E2D, 0x56FD, 0x7684, 0x4F60, 0x6211, 0x8BFB, 0x4E66,
      0x7AE0, 0x8282, 0x7F51, 0x7EDC, 0x8BBE, 0x7F6E, 0x53E3,
      'A', '?', 0x3001, 0x3002, 0xFF01, 0xFF1F, ' ',
  };
  for (uint32_t cp : kCommon) {
    const EpdGlyph* glyph = font.getGlyph(cp);
    CHECK(glyph != nullptr);
    CHECK(glyph->width == 15);
    CHECK(glyph->height == 16);
    CHECK(glyph->advanceX == 15);
    CHECK(glyph->dataLength == 30);
    const uint8_t* bmp = font.loadGlyphBitmap(glyph, nullptr);
    CHECK(bmp != nullptr);
    if (cp != ' ') CHECK(bitmapHasInk(bmp, glyph->dataLength));
  }

  // Unsupported codepoints stay null so GfxRenderer/EpdFont can use '?'.
  CHECK(font.getGlyph(0xFFFF) == nullptr);
  CHECK(font.getGlyph(0x10FFFE) == nullptr);
  const EpdGlyph* fallback = font.getGlyph('?');
  CHECK(fallback != nullptr);

  uint32_t strict = 0;
  uint32_t outliers = 0;
  uint32_t present = 0;
  for (uint32_t cp = 0; cp < 65536u; ++cp) {
    const EpdGlyph* glyph = font.getGlyph(cp);
    if (!glyph) continue;
    ++present;
    if (glyph->width == 16) {
      CHECK(glyph->height == 16);
      CHECK(glyph->dataLength == 32);
      ++outliers;
    } else {
      CHECK(glyph->width == 15);
      CHECK(glyph->dataLength == 30);
      ++strict;
    }
  }
  CHECK(strict == M4NativeGridFont::kGlyphCount);
  CHECK(outliers == M4NativeGridFont::kOutlierCount);
  CHECK(present == strict + outliers);

  // Ranked lookup must not allocate; scratch glyphs are a tiny ring.
  CHECK(sizeof(NativeGridEpdFont) < 512u);

  std::cout << "native-grid 15x16 ranked-bitset: PASS ("
            << blob.size() << " bytes, glyphs=" << strict
            << " outliers=" << outliers << ")\n";
  return 0;
}
