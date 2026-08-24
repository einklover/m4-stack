#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "EpdFont.h"

// ROM-backed 15x16 1-bit native-grid face. Unicode lookup is a ranked two-level
// BMP bitset (O(1) page + popcount); 32 outliers live in a compact sidecar.
// There is no per-glyph EpdGlyph table in flash — getGlyph synthesizes a
// scratch record. Bitmap bits stay in the embedded blob (no large RAM copy).
class NativeGridEpdFont final : public EpdFont {
 public:
  NativeGridEpdFont() : EpdFont(&data_) {}
  NativeGridEpdFont(const uint8_t* blob, size_t size) : EpdFont(&data_) { bind(blob, size); }

  NativeGridEpdFont(const NativeGridEpdFont&) = delete;
  NativeGridEpdFont& operator=(const NativeGridEpdFont&) = delete;

  bool bind(const uint8_t* blob, size_t size) {
    blob_ = nullptr;
    size_ = 0;
    valid_ = false;
    data_ = {};
    if (!blob || size < kHeaderBytes) return false;
    if (rd32(blob + 0) != kMagic) return false;
    if (rd16(blob + 4) != kVersion) return false;

    const uint16_t glyphCount = rd16(blob + 8);
    const uint16_t outlierCount = rd16(blob + 10);
    const uint16_t leafCount = rd16(blob + 12);
    const uint8_t gridW = blob[14];
    const uint8_t gridH = blob[15];
    const uint32_t pageDirOff = rd32(blob + 20);
    const uint32_t leavesOff = rd32(blob + 24);
    const uint32_t bitmapsOff = rd32(blob + 28);
    const uint32_t outlierCpsOff = rd32(blob + 32);
    const uint32_t outlierBmpsOff = rd32(blob + 36);
    const uint16_t bitmapBytes = rd16(blob + 40);
    const uint16_t outlierBmpBytes = rd16(blob + 42);

    if (gridW != 15 || gridH != 16 || bitmapBytes != 30 || outlierBmpBytes != 32) return false;
    if (pageDirOff != kHeaderBytes) return false;
    if (leavesOff != pageDirOff + 256u * 2u) return false;
    if (bitmapsOff != leavesOff + static_cast<uint32_t>(leafCount) * kLeafBytes) return false;
    if (outlierCpsOff != bitmapsOff + static_cast<uint32_t>(glyphCount) * 30u) return false;
    if (outlierBmpsOff != outlierCpsOff + static_cast<uint32_t>(outlierCount) * 2u) return false;
    const uint32_t end = outlierBmpsOff + static_cast<uint32_t>(outlierCount) * 32u;
    if (end > size) return false;

    blob_ = blob;
    size_ = size;
    glyphCount_ = glyphCount;
    outlierCount_ = outlierCount;
    leafCount_ = leafCount;
    pageDir_ = blob + pageDirOff;
    leaves_ = blob + leavesOff;
    bitmapsOff_ = bitmapsOff;
    outlierCpsOff_ = outlierCpsOff;
    outlierBmpsOff_ = outlierBmpsOff;

    data_.bitmap = blob;
    data_.glyph = nullptr;
    data_.intervals = nullptr;
    data_.intervalCount = 0;
    data_.advanceY = blob[16];
    data_.ascender = static_cast<int8_t>(blob[17]);
    data_.descender = static_cast<int8_t>(blob[18]);
    data_.is2Bit = false;
    valid_ = true;
    return true;
  }

  bool valid() const { return valid_; }
  uint16_t glyphCount() const { return glyphCount_; }
  uint16_t outlierCount() const { return outlierCount_; }

  const EpdGlyph* getGlyph(uint32_t cp,
                           const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const override {
    (void)style;
    if (!valid_) return nullptr;

    int32_t rank = -1;
    bool outlier = false;
    if (cp <= 0xFFFFu) {
      rank = rankedIndex(static_cast<uint16_t>(cp));
    }
    if (rank < 0) {
      rank = outlierIndex(cp);
      if (rank < 0) return nullptr;
      outlier = true;
    }

    EpdGlyph& g = nextScratch();
    if (outlier) {
      g.width = 16;
      g.height = 16;
      g.advanceX = 16;
      g.left = 0;
      g.top = 16;
      g.dataLength = 32;
      g.dataOffset = outlierBmpsOff_ + static_cast<uint32_t>(rank) * 32u;
    } else {
      g.width = 15;
      g.height = 16;
      g.advanceX = 15;
      g.left = 0;
      g.top = 16;
      g.dataLength = 30;
      g.dataOffset = bitmapsOff_ + static_cast<uint32_t>(rank) * 30u;
    }
    return &g;
  }

  int glyphAdvanceX(uint32_t cp,
                    const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const override {
    const EpdGlyph* glyph = getGlyph(cp, style);
    if (!glyph) glyph = getGlyph('?', style);
    return glyph ? glyph->advanceX : 0;
  }

  const uint8_t* loadGlyphBitmap(const EpdGlyph* glyph, uint8_t* buffer,
                                 const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const override {
    (void)buffer;
    (void)style;
    if (!valid_ || !glyph) return nullptr;
    const uint32_t end = glyph->dataOffset + glyph->dataLength;
    if (end < glyph->dataOffset || end > size_) return nullptr;
    return blob_ + glyph->dataOffset;
  }

  const EpdFontData* getData(const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const override {
    (void)style;
    return valid_ ? &data_ : nullptr;
  }

 private:
  static constexpr uint32_t kMagic = 0x474E344Du;  // 'M4NG' LE
  static constexpr uint16_t kVersion = 1;
  static constexpr uint16_t kHeaderBytes = 48;
  static constexpr uint16_t kLeafBytes = 34;

  static uint16_t rd16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
  }
  static uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
  }

  EpdGlyph& nextScratch() const {
    scratchI_ = static_cast<uint8_t>((scratchI_ + 1u) & 3u);
    scratch_[scratchI_] = {};
    return scratch_[scratchI_];
  }

  int32_t rankedIndex(uint16_t cp) const {
    const uint8_t page = static_cast<uint8_t>(cp >> 8);
    const uint8_t bit = static_cast<uint8_t>(cp & 0xFFu);
    const uint16_t leafIdx = rd16(pageDir_ + static_cast<size_t>(page) * 2u);
    if (leafIdx == 0xFFFFu || leafIdx >= leafCount_) return -1;
    const uint8_t* leaf = leaves_ + static_cast<size_t>(leafIdx) * kLeafBytes;
    const uint8_t* occ = leaf + 2;
    if (((occ[bit >> 3] >> (bit & 7u)) & 1u) == 0) return -1;
    uint16_t rank = rd16(leaf);
    const uint8_t fullBytes = static_cast<uint8_t>(bit >> 3);
    for (uint8_t i = 0; i < fullBytes; ++i) {
      rank = static_cast<uint16_t>(rank + __builtin_popcount(occ[i]));
    }
    const uint8_t rem = static_cast<uint8_t>(bit & 7u);
    if (rem) {
      rank = static_cast<uint16_t>(rank + __builtin_popcount(static_cast<uint8_t>(occ[fullBytes] & ((1u << rem) - 1u))));
    }
    if (rank >= glyphCount_) return -1;
    return rank;
  }

  int32_t outlierIndex(uint32_t cp) const {
    if (cp > 0xFFFFu || outlierCount_ == 0) return -1;
    const uint8_t* cps = blob_ + outlierCpsOff_;
    int lo = 0;
    int hi = static_cast<int>(outlierCount_) - 1;
    while (lo <= hi) {
      const int mid = lo + (hi - lo) / 2;
      const uint16_t v = rd16(cps + static_cast<size_t>(mid) * 2u);
      if (cp == v) return mid;
      if (cp < v) hi = mid - 1;
      else lo = mid + 1;
    }
    return -1;
  }

  const uint8_t* blob_ = nullptr;
  size_t size_ = 0;
  bool valid_ = false;
  uint16_t glyphCount_ = 0;
  uint16_t outlierCount_ = 0;
  uint16_t leafCount_ = 0;
  const uint8_t* pageDir_ = nullptr;
  const uint8_t* leaves_ = nullptr;
  uint32_t bitmapsOff_ = 0;
  uint32_t outlierCpsOff_ = 0;
  uint32_t outlierBmpsOff_ = 0;
  EpdFontData data_{};
  mutable EpdGlyph scratch_[4]{};
  mutable uint8_t scratchI_ = 0;
};
