// TtfReader — streamed TrueType/OpenType glyf parser + 2-bit rasterizer.
//
// Pure C++11, no Arduino / SdFat / FreeRTOS dependencies so it can be compiled
// and unit-tested on the host and inside the murphy_m4 firmware build.
//
// Design mirrors Murphy M4's native TTF path: the font file is read from a
// seekable stream ON DEMAND — only cmap and a few header fields are resident;
// `glyf` slices and `loca`/`hmtx` entries are read per glyph. A 18-20MB CJK
// font is never loaded into RAM whole. `init(stream, faceOffset)` can point
// directly at a glyf face inside TTC/OTC, whose table offsets remain absolute
// to the collection file; no virtual sfnt or collection copy is required.
//
// Supported: TrueType/OpenType glyf outlines (sfnt 0x00010000 / 'true' and
// OTTO when glyf+loca are present), cmap formats 4 and 12, simple glyphs,
// compound glyphs including XY and point-index attachment, hhea/hmtx metrics,
// and variable TrueType at its default/base instance (fvar/gvar axes ignored).
// WOFF/WOFF2 and CFF/CFF2 outlines are handled/rejected outside this backend.

#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace ttf {

class TtfStream {
 public:
  virtual ~TtfStream() = default;
  virtual uint32_t size() const = 0;
  virtual bool seek(uint32_t pos) = 0;
  virtual uint32_t read(void* dst, uint32_t n) = 0;
};

struct Pt {
  float x, y;
  bool on;
};
struct Contour {
  std::vector<Pt> pts;
};
struct Xform {
  float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
};

struct GlyphBitmap {
  const uint8_t* data = nullptr;
  int16_t width = 0;
  int16_t height = 0;
  int16_t xoff = 0;
  int16_t yoff = 0;
  int16_t advance = 0;
  uint16_t packedLen = 0;
};

class TtfFont {
 public:
  TtfFont();
  TtfFont(const TtfFont&) = delete;
  TtfFont& operator=(const TtfFont&) = delete;
  ~TtfFont();

  // Legacy standalone-face entry. Kept explicit so existing callers and the
  // original implementation stay source-compatible while collection support
  // is introduced without a large parser rewrite.
  bool init(TtfStream& s);

  // Parse + validate one glyf sfnt face at an absolute file offset. This is
  // the zero-copy TTC/OTC entry: TableRecord offsets remain absolute from the
  // beginning of the collection, exactly as stored in OpenType collections.
  bool init(TtfStream& s, uint32_t faceOffset);

  bool ready() const { return ready_; }
  const char* lastError() const { return lastError_; }

  uint16_t unitsPerEm() const { return unitsPerEm_; }
  int32_t numGlyphs() const { return numGlyphs_; }

  bool findGlyph(uint32_t cp, uint16_t& gid) const;
  bool collectGlyph(uint16_t gid, const Xform& xf, std::vector<Contour>& out, int depth = 0) const;
  bool glyphHMetrics(uint16_t gid, int32_t& advUnits, int32_t& lsbUnits) const;
  void fontVMetrics(int32_t& ascUnits, int32_t& descUnits, int32_t& gapUnits) const;
  int32_t fontBBoxYMax() const { return bboxYMax_; }
  bool glyphPixelBox(uint16_t gid, uint16_t sizePx, int& x0, int& y0, int& x1, int& y1) const;
  bool rasterize(uint16_t gid, uint16_t sizePx, GlyphBitmap& out);
  void clearScratch();

 private:
  struct Table {
    uint32_t off = 0;
    uint32_t len = 0;
    bool present = false;
  };

  static uint32_t tagKey(const char tag[4]);
  Table findTable(uint32_t key) const;
  bool readAt(uint32_t off, void* dst, uint32_t n) const;
  bool initCmap();
  bool collectGlyphInternal(uint16_t gid, const Xform& xf, std::vector<Contour>& out, int depth) const;

  TtfStream* s_ = nullptr;
  uint32_t fileSize_ = 0;
  bool ready_ = false;
  const char* lastError_ = "not initialized";

  Table head_, maxp_, loca_, cmap_, hhea_, hmtx_, glyf_, kern_, gvar_, fvar_;
  uint16_t unitsPerEm_ = 0;
  bool longLoca_ = false;
  int32_t numGlyphs_ = 0;
  int32_t ascender_ = 0;
  int32_t descender_ = 0;
  int32_t lineGap_ = 0;
  int32_t bboxYMax_ = 0;
  int32_t numHMetrics_ = 0;

  uint8_t* cmapData_ = nullptr;
  uint32_t cmapLen_ = 0;
  bool cmapIs12_ = false;
  uint32_t nGroups_ = 0;

  mutable uint8_t* glyfScratch_ = nullptr;
  mutable uint32_t glyfScratchCap_ = 0;
  uint8_t* covScratch_ = nullptr;
  uint32_t covScratchCap_ = 0;
  uint8_t* packedScratch_ = nullptr;
  uint32_t packedScratchCap_ = 0;
};

}  // namespace ttf
