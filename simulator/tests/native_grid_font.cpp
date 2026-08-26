#include "NativeGridEpdFont.h"
#include "fontdata/m4_native_grid_15x16.h"
#include "util/M4FontPolicy.h"

#include <cstdint>
#include <cstdio>
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

bool pixelOn(const uint8_t* bmp, int w, int x, int y) {
  const uint32_t idx = static_cast<uint32_t>(y) * static_cast<uint32_t>(w) + static_cast<uint32_t>(x);
  return ((bmp[idx / 8u] >> (7u - (idx % 8u))) & 1u) != 0;
}

struct VisualBox {
  int width = 0;
  int height = 0;
  int left = 0;
  int advance = 0;
  int ink0 = 0;
  int ink1 = -1;
  int lsb = 0;
  int rsb = 0;
  int inkW = 0;
  bool any = false;
};

VisualBox measure(const NativeGridEpdFont& font, uint32_t cp) {
  VisualBox b;
  const EpdGlyph* glyph = font.getGlyph(cp);
  if (!glyph) fail("missing glyph");
  b.width = glyph->width;
  b.height = glyph->height;
  b.left = glyph->left;
  b.advance = glyph->advanceX;
  const uint8_t* bmp = font.loadGlyphBitmap(glyph, nullptr);
  if (!bmp) fail("missing bitmap");
  int x0 = b.width;
  int x1 = -1;
  for (int y = 0; y < b.height; ++y) {
    for (int x = 0; x < b.width; ++x) {
      if (pixelOn(bmp, b.width, x, y)) {
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
      }
    }
  }
  if (x1 >= 0) {
    b.any = true;
    b.ink0 = x0;
    b.ink1 = x1;
    b.inkW = x1 - x0 + 1;
    b.lsb = b.left + x0;
    b.rsb = b.advance - (b.left + x1 + 1);
  }
  return b;
}

uint32_t nextCp(const char*& p) {
  const unsigned char c = static_cast<unsigned char>(*p);
  if (c == 0) return 0;
  if (c < 0x80) {
    ++p;
    return c;
  }
  if ((c & 0xE0) == 0xC0) {
    const uint32_t cp = (static_cast<uint32_t>(c & 0x1F) << 6) |
                        (static_cast<unsigned char>(p[1]) & 0x3F);
    p += 2;
    return cp;
  }
  if ((c & 0xF0) == 0xE0) {
    const uint32_t cp = (static_cast<uint32_t>(c & 0x0F) << 12) |
                        ((static_cast<unsigned char>(p[1]) & 0x3F) << 6) |
                        (static_cast<unsigned char>(p[2]) & 0x3F);
    p += 3;
    return cp;
  }
  if ((c & 0xF8) == 0xF0) {
    const uint32_t cp = (static_cast<uint32_t>(c & 0x07) << 18) |
                        ((static_cast<unsigned char>(p[1]) & 0x3F) << 12) |
                        ((static_cast<unsigned char>(p[2]) & 0x3F) << 6) |
                        (static_cast<unsigned char>(p[3]) & 0x3F);
    p += 4;
    return cp;
  }
  fail("invalid utf-8");
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
  CHECK(M4FontPolicy::kLogicalCellPx == 16);
  CHECK(NativeGridEpdFont::kLogicalCellPx == 16);
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

  static constexpr uint32_t kCjk[] = {
      0x4E2D, 0x56FD, 0x7684, 0x4F60, 0x6211, 0x8BFB, 0x4E66,
      0x7AE0, 0x8282, 0x7F51, 0x7EDC, 0x8BBE, 0x7F6E, 0x53E3,
      0x3001, 0x3002, 0xFF01, 0xFF1F,
  };
  for (uint32_t cp : kCjk) {
    CHECK(NativeGridEpdFont::metricKind(cp) == NativeGridEpdFont::MetricKind::Cjk);
    const EpdGlyph* glyph = font.getGlyph(cp);
    CHECK(glyph != nullptr);
    CHECK(glyph->width == 15);
    CHECK(glyph->height == 16);
    CHECK(glyph->left == 0);
    CHECK(glyph->advanceX == NativeGridEpdFont::kLogicalCellPx);
    CHECK(glyph->advanceX == 16);
    CHECK(glyph->dataLength == 30);
    const uint8_t* bmp = font.loadGlyphBitmap(glyph, nullptr);
    CHECK(bmp != nullptr);
    CHECK(bitmapHasInk(bmp, glyph->dataLength));
  }

  CHECK(NativeGridEpdFont::metricKind(' ') == NativeGridEpdFont::MetricKind::Space);
  const VisualBox space = measure(font, ' ');
  CHECK(space.left == 0);
  CHECK(space.advance == NativeGridEpdFont::kSpaceAdvance);
  CHECK(!space.any);

  // Latin letters/digits/punctuation: compact ink + equal 1px side bearings.
  // Source rasters are left-packed in the 15-cell; xOffset must move ink off
  // the cursor so glyphs are not glued to the left of a wide advance.
  static constexpr uint32_t kLatin[] = {
      'A', 'B', 'C', 'a', 'b', 'c', 'i', 'W', 'm', '1', '2', '3',
      ',', '.', ';', ':', '!', '?', '\'', '"',
  };
  int latinPen = 0;
  for (uint32_t cp : kLatin) {
    CHECK(NativeGridEpdFont::metricKind(cp) == NativeGridEpdFont::MetricKind::Latin);
    const VisualBox b = measure(font, cp);
    CHECK(b.width == 15);
    CHECK(b.any);
    CHECK(b.lsb == NativeGridEpdFont::kLatinSideBearing);
    CHECK(b.rsb == NativeGridEpdFont::kLatinSideBearing);
    CHECK(b.advance == NativeGridEpdFont::kLatinSideBearing + b.inkW + NativeGridEpdFont::kLatinSideBearing);
    CHECK(b.advance < 15);
    CHECK(b.left == NativeGridEpdFont::kLatinSideBearing - b.ink0);
    latinPen += b.advance;
  }
  // 'ABC' must be far narrower than three full 15-cells.
  CHECK(latinPen < 15 * static_cast<int>(sizeof(kLatin) / sizeof(kLatin[0])));

  // Directional pairs (not mechanically centered). Straight ASCII ' " stay
  // Latin/symmetric above because they are the same glyph for open and close.
  static constexpr uint32_t kOpen[] = {
      '(', '[', '{', '<', 0x2018u, 0x201Cu, 0x3008u, 0x300Au, 0x300Cu, 0x300Eu,
      0x3010u, 0x3014u, 0x3016u, 0xFF08u, 0xFF3Bu, 0xFF5Bu, 0xFF1Cu,
  };
  static constexpr uint32_t kClose[] = {
      ')', ']', '}', '>', 0x2019u, 0x201Du, 0x3009u, 0x300Bu, 0x300Du, 0x300Fu,
      0x3011u, 0x3015u, 0x3017u, 0xFF09u, 0xFF3Du, 0xFF5Du, 0xFF1Eu,
  };
  for (uint32_t cp : kOpen) {
    CHECK(NativeGridEpdFont::metricKind(cp) == NativeGridEpdFont::MetricKind::PairOpen);
    const VisualBox b = measure(font, cp);
    CHECK(b.any);
    CHECK(b.lsb == NativeGridEpdFont::kPairOuterBearing);
    CHECK(b.rsb == NativeGridEpdFont::kPairInnerBearing);
    CHECK(b.lsb > b.rsb);
    CHECK(b.advance < 15);
    CHECK(b.left == NativeGridEpdFont::kPairOuterBearing - b.ink0);
  }
  for (uint32_t cp : kClose) {
    CHECK(NativeGridEpdFont::metricKind(cp) == NativeGridEpdFont::MetricKind::PairClose);
    const VisualBox b = measure(font, cp);
    CHECK(b.any);
    CHECK(b.lsb == NativeGridEpdFont::kPairInnerBearing);
    CHECK(b.rsb == NativeGridEpdFont::kPairOuterBearing);
    CHECK(b.lsb < b.rsb);
    CHECK(b.advance < 15);
    CHECK(b.left == NativeGridEpdFont::kPairInnerBearing - b.ink0);
  }

  auto walk = [&](const char* s, const char* tag) {
    int pen = 0;
    int n = 0;
    const char* p = s;
    std::cout << "native-grid string " << tag << ":";
    while (*p) {
      const uint32_t cp = nextCp(p);
      const NativeGridEpdFont::MetricKind kind = NativeGridEpdFont::metricKind(cp);
      if (kind == NativeGridEpdFont::MetricKind::Space) {
        const VisualBox b = measure(font, cp);
        CHECK(b.advance == NativeGridEpdFont::kSpaceAdvance);
        pen += b.advance;
        ++n;
        continue;
      }
      const VisualBox b = measure(font, cp);
      CHECK(b.advance > 0);
      if (kind == NativeGridEpdFont::MetricKind::Latin) {
        CHECK(b.lsb == NativeGridEpdFont::kLatinSideBearing);
        CHECK(b.rsb == NativeGridEpdFont::kLatinSideBearing);
        CHECK(b.advance < 15);
      } else if (kind == NativeGridEpdFont::MetricKind::PairOpen) {
        CHECK(b.lsb > b.rsb);
        CHECK(b.advance < 15);
      } else if (kind == NativeGridEpdFont::MetricKind::PairClose) {
        CHECK(b.lsb < b.rsb);
        CHECK(b.advance < 15);
      } else {
        CHECK(b.left == 0);
        CHECK(b.advance == NativeGridEpdFont::kLogicalCellPx);
      }
      pen += b.advance;
      ++n;
      char buf[48];
      std::snprintf(buf, sizeof(buf), " U+%04X(l=%d a=%d)", cp, b.left, b.advance);
      std::cout << buf;
    }
    std::cout << " glyphs=" << n << " advance=" << pen << "\n";
    CHECK(n > 0);
    CHECK(pen > 0);
  };
  walk("设置 阅读 ABC abc 123,.;:!?()[]", "latin-cjk");
  walk("“测试” ‘ABC’ （测试） 《书名》 【章节】", "pairs");
  walk("「引」『书』〈标〉【章节】", "cjk-quotes");
  walk("Hello（世界）\"test\"(ok)[A]<B> ‘C’ “测” 「引」『书』〈标〉《名》【章】", "mixed-punct");

  auto inkGap = [&](uint32_t a, uint32_t b) {
    const VisualBox ba = measure(font, a);
    const VisualBox bb = measure(font, b);
    const int aEnd = ba.lsb + ba.inkW;
    const int bStart = ba.advance + bb.lsb;
    CHECK(bStart >= aEnd);
    return bStart - aEnd;
  };
  // Opening sits toward following text (inner gap); closing sits toward
  // preceding text (inner LSB) with extra trailing outer bearing.
  CHECK(inkGap(0x201Cu, 0x6D4Bu) == NativeGridEpdFont::kPairInnerBearing);  // “测
  CHECK(inkGap(0x2018u, 'A') == NativeGridEpdFont::kPairInnerBearing + NativeGridEpdFont::kLatinSideBearing);
  CHECK(inkGap(0xFF08u, 0x6D4Bu) == NativeGridEpdFont::kPairInnerBearing);  // （测
  CHECK(inkGap(0x300Au, 0x4E66u) == NativeGridEpdFont::kPairInnerBearing);  // 《书
  CHECK(inkGap(0x3010u, 0x7AE0u) == NativeGridEpdFont::kPairInnerBearing);  // 【章
  CHECK(inkGap('(', 'A') == NativeGridEpdFont::kPairInnerBearing + NativeGridEpdFont::kLatinSideBearing);
  const VisualBox shi = measure(font, 0x8BD5u);
  CHECK(inkGap(0x8BD5u, 0x201Du) == shi.rsb + NativeGridEpdFont::kPairInnerBearing);  // 试”
  const VisualBox ming = measure(font, 0x540Du);
  CHECK(inkGap(0x540Du, 0x300Bu) == ming.rsb + NativeGridEpdFont::kPairInnerBearing);  // 名》
  CHECK(measure(font, 0x201Cu).lsb > measure(font, 0x201Cu).rsb);
  CHECK(measure(font, 0x201Du).lsb < measure(font, 0x201Du).rsb);

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
    CHECK(NativeGridEpdFont::metricKind(cp) != NativeGridEpdFont::MetricKind::Cjk ||
          glyph->advanceX == NativeGridEpdFont::kLogicalCellPx);
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
