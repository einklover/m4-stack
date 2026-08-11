// TtfReader — streamed TrueType (glyf) parser + 2-bit rasterizer.
//
// Pure C++11, no Arduino / SdFat / FreeRTOS dependencies so it can be compiled
// and unit-tested on the host and inside the murphy_m4 firmware build.
//
// Design mirrors Murphy M4's native TTF path (see
// vendor/Murphy/m4/findings/murphy_reader_ttf_fonts.md): the font file is read
// from a seekable stream ON DEMAND — only the table directory, cmap subtable,
// and a few header fields are resident; `glyf` slices and `loca`/`hmtx` entries
// are read per glyph. A 18-20MB CJK font is never loaded into RAM whole.
//
// Supported: plain TrueType glyf outlines (sfnt 0x00010000 / 'true'), cmap
// format 4 and 12, simple glyphs, compound glyphs using XY component offsets,
// hhea/hmtx metrics, and variable TrueType at its default/base instance (fvar/
// gvar axes are intentionally ignored). Explicitly rejected: OTTO/CFF/CFF2,
// ttcf collections, WOFF/WOFF2, cmap formats other than 4/12. Compound glyphs
// that position components by matching point indices are still unsupported.

#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace ttf {

// Minimal seekable byte stream. TtfFont only calls these three methods.
class TtfStream {
 public:
  virtual ~TtfStream() = default;
  virtual uint32_t size() const = 0;
  virtual bool seek(uint32_t pos) = 0;
  virtual uint32_t read(void* dst, uint32_t n) = 0;  // returns bytes read
};

// Outline primitives (used internally; exposed only for host unit tests).
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

// Result of rasterize(): a 2-bit packed bitmap (0=white .. 3=black, 4 px/byte,
// MSB-first) matching GfxRenderer's is2Bit layout. `data` points into an
// internal scratch owned by TtfFont; it stays valid until the next call to
// rasterize() or init().
struct GlyphBitmap {
  const uint8_t* data = nullptr;
  int16_t width = 0;   // bitmap width in pixels
  int16_t height = 0;  // bitmap height in pixels
  int16_t xoff = 0;    // left bearing in pixels (relative to pen origin)
  int16_t yoff = 0;    // distance from baseline up to bitmap top (>=0)
  int16_t advance = 0; // advance width in pixels (scaled)
  uint16_t packedLen = 0;  // bytes used in data
};

class TtfFont {
 public:
  TtfFont();
  TtfFont(const TtfFont&) = delete;
  TtfFont& operator=(const TtfFont&) = delete;
  ~TtfFont();

  // Parse + validate the font from `s`. Returns false (with lastError()) on any
  // unsupported / malformed input. `s` must outlive this object.
  bool init(TtfStream& s);

  bool ready() const { return ready_; }
  const char* lastError() const { return lastError_; }

  uint16_t unitsPerEm() const { return unitsPerEm_; }
  int32_t numGlyphs() const { return numGlyphs_; }

  // cmap codepoint -> glyph id (0 = .notdef / missing).
  bool findGlyph(uint32_t cp, uint16_t& gid) const;

  // Collect the outline contours of a glyph (used by rasterize; public for
  // host unit tests / debugging).
  bool collectGlyph(uint16_t gid, const Xform& xf, std::vector<Contour>& out, int depth = 0) const;

  // hmtx advance width + left side bearing in font units.
  bool glyphHMetrics(uint16_t gid, int32_t& advUnits, int32_t& lsbUnits) const;

  // hhea vertical metrics in font units (descent is negative).
  void fontVMetrics(int32_t& ascUnits, int32_t& descUnits, int32_t& gapUnits) const;

  // Global outline top from the TrueType head table.  The renderer's
  // EpdFontData::ascender is a baseline-to-actual-top distance, not merely
  // hhea.ascender; CJK fonts commonly have glyphs above the hhea metric.
  int32_t fontBBoxYMax() const { return bboxYMax_; }

  // Scaled outline bounding box for `gid` at `sizePx` (y0 is the baseline-relative
  // top, negative; pixel rows increase downward). Returns false for glyphs that
  // cannot be rasterized (malformed / unsupported compound args).
  bool glyphPixelBox(uint16_t gid, uint16_t sizePx, int& x0, int& y0, int& x1, int& y1) const;

  // Rasterize `gid` at `sizePx` (scale = sizePx / unitsPerEm). Returns false if
  // the glyph cannot be rasterized or the bitmap would exceed 255px in either
  // dimension (EpdGlyph stores width/height as uint8_t).
  bool rasterize(uint16_t gid, uint16_t sizePx, GlyphBitmap& out);

  // Release transient scratch allocations (call between pages to cap peak RAM).
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

  // Headers resident in RAM.
  Table head_, maxp_, loca_, cmap_, hhea_, hmtx_, glyf_, kern_, gvar_, fvar_;
  uint16_t unitsPerEm_ = 0;
  bool longLoca_ = false;
  int32_t numGlyphs_ = 0;
  int32_t ascender_ = 0;
  int32_t descender_ = 0;
  int32_t lineGap_ = 0;
  int32_t bboxYMax_ = 0;
  int32_t numHMetrics_ = 0;

  // Chosen cmap subtable, resident.
  uint8_t* cmapData_ = nullptr;
  uint32_t cmapLen_ = 0;
  bool cmapIs12_ = false;
  uint32_t nGroups_ = 0;  // format 12 group count

  // Per-glyph transient scratch (grows to the largest glyph, never unbounded).
  mutable uint8_t* glyfScratch_ = nullptr;
  mutable uint32_t glyfScratchCap_ = 0;
  uint8_t* covScratch_ = nullptr;
  uint32_t covScratchCap_ = 0;
  uint8_t* packedScratch_ = nullptr;
  uint32_t packedScratchCap_ = 0;
};

}  // namespace ttf