// Zero-copy sfnt-face entry for TtfFont. The legacy init(TtfStream&) remains in
// TtfReader.cpp; this overload differs only in where the sfnt directory lives.
// TableRecord offsets are absolute from the beginning of a TTC/OTC file, so all
// existing loca/glyf/cmap/hmtx reads can continue to use the original stream.

#include "TtfReader.h"

#include <cstdint>

namespace ttf {
namespace {

constexpr uint32_t kTagHead = 0x68656164u;
constexpr uint32_t kTagMaxp = 0x6d617870u;
constexpr uint32_t kTagLoca = 0x6c6f6361u;
constexpr uint32_t kTagCmap = 0x636d6170u;
constexpr uint32_t kTagHhea = 0x68686561u;
constexpr uint32_t kTagHmtx = 0x686d7478u;
constexpr uint32_t kTagGlyf = 0x676c7966u;
constexpr uint32_t kTagKern = 0x6b65726eu;
constexpr uint32_t kTagGvar = 0x67766172u;
constexpr uint32_t kTagFvar = 0x66766172u;

uint16_t rd16Face(const uint8_t* p) {
  return static_cast<uint16_t>((uint16_t(p[0]) << 8) | p[1]);
}
int16_t rds16Face(const uint8_t* p) {
  return static_cast<int16_t>(rd16Face(p));
}
uint32_t rd32Face(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

}  // namespace

bool TtfFont::init(TtfStream& s, uint32_t faceOffset) {
  ready_ = false;
  lastError_ = "not initialized";
  s_ = &s;
  fileSize_ = s.size();

  // Reset face-specific metadata. High-water glyph scratch is deliberately
  // retained across re-init; initCmap() replaces/frees the old cmap only after
  // the new one has been read successfully.
  head_ = Table{};
  maxp_ = Table{};
  loca_ = Table{};
  cmap_ = Table{};
  hhea_ = Table{};
  hmtx_ = Table{};
  glyf_ = Table{};
  kern_ = Table{};
  gvar_ = Table{};
  fvar_ = Table{};
  unitsPerEm_ = 0;
  longLoca_ = false;
  numGlyphs_ = 0;
  ascender_ = 0;
  descender_ = 0;
  lineGap_ = 0;
  bboxYMax_ = 0;
  numHMetrics_ = 0;
  cmapLen_ = 0;
  cmapIs12_ = false;
  nGroups_ = 0;

  if (faceOffset > fileSize_ || fileSize_ - faceOffset < 12u) {
    lastError_ = "sfnt face header out of range";
    return false;
  }

  uint8_t hdr[12];
  if (!readAt(faceOffset, hdr, sizeof(hdr))) {
    lastError_ = "failed to read sfnt face header";
    return false;
  }
  const uint32_t sfnt = rd32Face(hdr);
  if (sfnt != 0x00010000u && sfnt != 0x74727565u && sfnt != 0x4f54544fu) {
    lastError_ = "not a TrueType/OpenType glyf face";
    return false;
  }

  const uint16_t numTables = rd16Face(hdr + 4);
  if (numTables == 0 || numTables > 128) {
    lastError_ = "invalid table count";
    return false;
  }
  const uint64_t dirEnd = uint64_t(faceOffset) + 12u + uint64_t(numTables) * 16u;
  if (dirEnd > fileSize_) {
    lastError_ = "table directory out of range";
    return false;
  }

  Table* slots[10] = {
      &head_, &maxp_, &loca_, &cmap_, &hhea_,
      &hmtx_, &glyf_, &kern_, &gvar_, &fvar_};
  const uint32_t keys[10] = {
      kTagHead, kTagMaxp, kTagLoca, kTagCmap, kTagHhea,
      kTagHmtx, kTagGlyf, kTagKern, kTagGvar, kTagFvar};
  bool got[10] = {};

  for (uint16_t i = 0; i < numTables; ++i) {
    uint8_t rec[16];
    const uint32_t recOff = faceOffset + 12u + uint32_t(i) * 16u;
    if (!readAt(recOff, rec, sizeof(rec))) {
      lastError_ = "failed to read table directory";
      return false;
    }
    const uint32_t key = rd32Face(rec);
    const uint32_t off = rd32Face(rec + 8);
    const uint32_t len = rd32Face(rec + 12);

    // In TTC/OTC, offsets are relative to the beginning of the entire
    // collection, not the face directory. Validate once here and preserve the
    // absolute value so every existing table reader remains zero-copy.
    if (off > fileSize_ || len > fileSize_ - off) {
      lastError_ = "font table out of file range";
      return false;
    }
    for (int k = 0; k < 10; ++k) {
      if (key == keys[k]) {
        slots[k]->off = off;
        slots[k]->len = len;
        slots[k]->present = true;
        got[k] = true;
        break;
      }
    }
  }

  if (!(got[0] && got[1] && got[2] && got[3] && got[4] && got[5] && got[6])) {
    lastError_ = "missing required glyf tables (head/maxp/loca/cmap/hhea/hmtx/glyf)";
    return false;
  }

  if (head_.len < 54) {
    lastError_ = "head table too small";
    return false;
  }
  uint8_t head[54];
  if (!readAt(head_.off, head, sizeof(head))) {
    lastError_ = "failed to read head table";
    return false;
  }
  unitsPerEm_ = rd16Face(head + 18);
  bboxYMax_ = rds16Face(head + 42);
  longLoca_ = rds16Face(head + 50) != 0;
  if (unitsPerEm_ == 0 || unitsPerEm_ > 16384) {
    lastError_ = "invalid unitsPerEm";
    return false;
  }

  if (maxp_.len < 6) {
    lastError_ = "maxp table too small";
    return false;
  }
  uint8_t maxp[6];
  if (!readAt(maxp_.off, maxp, sizeof(maxp))) {
    lastError_ = "failed to read maxp table";
    return false;
  }
  numGlyphs_ = rd16Face(maxp + 4);
  if (numGlyphs_ <= 0) {
    lastError_ = "no glyphs";
    return false;
  }

  if (hhea_.len < 36) {
    lastError_ = "hhea table too small";
    return false;
  }
  uint8_t hhea[36];
  if (!readAt(hhea_.off, hhea, sizeof(hhea))) {
    lastError_ = "failed to read hhea table";
    return false;
  }
  ascender_ = rds16Face(hhea + 4);
  descender_ = rds16Face(hhea + 6);
  lineGap_ = rds16Face(hhea + 8);
  numHMetrics_ = rd16Face(hhea + 34);
  if (numHMetrics_ <= 0 || numHMetrics_ > numGlyphs_) {
    lastError_ = "invalid horizontal metrics count";
    return false;
  }

  const uint32_t locaEntry = longLoca_ ? 4u : 2u;
  const uint64_t requiredLoca = uint64_t(locaEntry) * (uint64_t(numGlyphs_) + 1u);
  if (loca_.len < requiredLoca) {
    lastError_ = "loca table too small";
    return false;
  }

  if (!initCmap()) return false;

  ready_ = true;
  lastError_ = "ok";
  return true;
}

}  // namespace ttf
