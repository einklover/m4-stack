#include "ScaledEpdFont.h"
#include "fontdata/m4_compact_cjk_16.h"
#include "util/M4FontPolicy.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {

[[noreturn]] void fail(const char* msg) {
  std::cerr << "scaled_epd_font FAIL: " << msg << "\n";
  std::exit(1);
}

#define CHECK(cond) \
  do {              \
    if (!(cond)) fail(#cond); \
  } while (0)

const EpdGlyph* findGlyph(uint32_t cp) {
  int left = 0;
  int right = static_cast<int>(sizeof(m4_compact_cjk_16Intervals) /
                               sizeof(m4_compact_cjk_16Intervals[0])) -
              1;
  while (left <= right) {
    const int mid = left + (right - left) / 2;
    const EpdUnicodeInterval& interval = m4_compact_cjk_16Intervals[mid];
    if (cp < interval.first) {
      right = mid - 1;
    } else if (cp > interval.last) {
      left = mid + 1;
    } else {
      return &m4_compact_cjk_16Glyphs[interval.offset + cp - interval.first];
    }
  }
  return nullptr;
}

class CompactSource final : public EpdFont {
 public:
  CompactSource() : EpdFont(&m4_compact_cjk_16) {}

  const EpdGlyph* getGlyph(uint32_t cp,
                           const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const override {
    (void)style;
    return findGlyph(cp);
  }

  const uint8_t* loadGlyphBitmap(const EpdGlyph* glyph, uint8_t* buffer,
                                 const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const override {
    (void)buffer;
    (void)style;
    if (!glyph) return nullptr;
    return m4_compact_cjk_16Bitmaps + glyph->dataOffset;
  }

  int glyphAdvanceX(uint32_t cp,
                    const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const override {
    const EpdGlyph* glyph = getGlyph(cp, style);
    if (!glyph) glyph = getGlyph('?', style);
    return glyph ? glyph->advanceX : 0;
  }
};

uint8_t expectedDim(uint8_t v, float scale) {
  if (v == 0) return 0;
  int n = static_cast<int>(std::lround(v * scale));
  if (n < 1) n = 1;
  if (n > 255) n = 255;
  return static_cast<uint8_t>(n);
}

bool bitmapHasInk(const uint8_t* bmp, uint32_t dataLength, bool is2Bit) {
  if (!bmp || dataLength == 0) return false;
  for (uint32_t i = 0; i < dataLength; ++i) {
    if (is2Bit) {
      if (bmp[i] != 0) return true;
    } else if (bmp[i] != 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

static_assert(M4FontPolicy::kCompactCjkSourcePx == 14,
              "compact builtin CJK rasterizes at 14px despite the 16pt generation size");

int main() {
  // Production divisor selection: the compact 2-bit source divides by its real
  // raster pixels (14); canonical SD epdfont sources keep the nominal 16px.
  CHECK(M4FontPolicy::systemReaderSourcePx(/*compact2BitSource=*/true) ==
        M4FontPolicy::kCompactCjkSourcePx);
  CHECK(M4FontPolicy::systemReaderSourcePx(/*compact2BitSource=*/false) ==
        M4FontPolicy::kCanonicalEpdfontPixelSize);
  CHECK(M4FontPolicy::systemReaderSourcePx(false) == 16);

  CompactSource source;
  ScaledEpdFont scaled;
  static constexpr uint32_t kProbe = 0x4E2D;  // 中
  const EpdGlyph* srcGlyph = source.getGlyph(kProbe);
  CHECK(srcGlyph != nullptr);
  CHECK(srcGlyph->width > 0);
  CHECK(srcGlyph->height > 0);
  CHECK(srcGlyph->advanceX > 0);

  const float compactSourcePx =
      static_cast<float>(M4FontPolicy::systemReaderSourcePx(/*compact2BitSource=*/true));
  static constexpr int kSizes[] = {14, 15, 16, 17, 20, 22};
  int prevAdvance = 0;
  int prevLine = 0;
  for (int px : kSizes) {
    const float scale = static_cast<float>(px) / compactSourcePx;
    scaled.bind(&source, scale);
    CHECK(std::fabs(scaled.scale() - scale) < 0.0001f);

    const EpdFontData* data = scaled.getData();
    CHECK(data != nullptr);
    CHECK(data->is2Bit);
    CHECK(data->advanceY == expectedDim(m4_compact_cjk_16.advanceY, scale));
    CHECK(data->ascender == static_cast<int>(std::lround(m4_compact_cjk_16.ascender * scale)));

    const EpdGlyph* glyph = scaled.getGlyph(kProbe);
    CHECK(glyph != nullptr);
    CHECK(glyph->width == expectedDim(srcGlyph->width, scale));
    CHECK(glyph->height == expectedDim(srcGlyph->height, scale));
    CHECK(glyph->advanceX == expectedDim(srcGlyph->advanceX, scale));

    if (px == 14) {
      // Requested size equals the compact face's real raster: exact unity
      // passthrough without resampling.
      CHECK(scaled.isUnityScale());
      CHECK(glyph == srcGlyph);
      CHECK(data->advanceY == m4_compact_cjk_16.advanceY);
    } else {
      // Practical sizes must not snap back to the 14px source face — including
      // 16px, which historically divided by the 16pt nominal and rendered the
      // unscaled (undersized) source.
      CHECK(glyph->width != srcGlyph->width);
      CHECK(glyph->height != srcGlyph->height);
      CHECK(glyph->advanceX != srcGlyph->advanceX);
      CHECK(data->advanceY != m4_compact_cjk_16.advanceY);
    }
    if (px == 16) {
      // Regression: requested 16px must upscale by 16/14 (~1.143). Dividing by
      // the 16pt nominal gave unity scale and ~12% undersize vs custom TTF at
      // the same reader pixel size.
      CHECK(std::fabs(scaled.scale() - 16.0f / 14.0f) < 0.0001f);
      CHECK(glyph->width > srcGlyph->width);
      CHECK(glyph->height > srcGlyph->height);
      CHECK(glyph->advanceX > srcGlyph->advanceX);
      CHECK(data->advanceY > m4_compact_cjk_16.advanceY);
    }

    const uint8_t* bmp = scaled.loadGlyphBitmap(glyph, nullptr);
    CHECK(bmp != nullptr);
    CHECK(bitmapHasInk(bmp, glyph->dataLength, true));

    // 14 < 15 < 16 < 17 < 20 < 22 on both glyph advance and line pitch.
    if (prevAdvance != 0) {
      CHECK(glyph->advanceX > prevAdvance);
      CHECK(data->advanceY > prevLine);
    }
    prevAdvance = glyph->advanceX;
    prevLine = data->advanceY;

    std::cout << "scaled compact CJK @" << px << "px advance=" << static_cast<int>(glyph->advanceX)
              << " line=" << static_cast<int>(data->advanceY)
              << " glyph=" << static_cast<int>(glyph->width) << "x"
              << static_cast<int>(glyph->height) << "\n";
  }

  // Canonical/16px sources still divide by 16: a true 16px raster binds at
  // unity for the same requested size and upscales beyond it.
  {
    ScaledEpdFont canonicalScaled;
    const float divisor =
        static_cast<float>(M4FontPolicy::systemReaderSourcePx(/*compact2BitSource=*/false));
    canonicalScaled.bind(&source, 16.0f / divisor);
    CHECK(canonicalScaled.isUnityScale());
    canonicalScaled.bind(&source, 20.0f / divisor);
    CHECK(std::fabs(canonicalScaled.scale() - 1.25f) < 0.0001f);
    CHECK(!canonicalScaled.isUnityScale());
  }

  std::cout << "scaled EpdFont compact CJK metrics: PASS\n";
  return 0;
}
