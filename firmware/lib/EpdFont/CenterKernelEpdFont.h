#pragma once

// Reader-body CJK renderer consuming 16x16 absolute occupancy + 2-bit geometry class.
//
// M4CK blob: 48-byte header magic M4CK; u16 version,flags,glyph_count,leaf_count @4;
// u8 grid 16x16 @12,13; u32 page_dir,leaves,bitmaps,classes @16; bitmap_bytes=32.
// Occupancy 32 bytes MSB-first per row (bit7 is col0). Class 2 bits per rank:
// (bytes[i>>2] >> ((i&3)*2)) &3. One instance has one pixelSize; reader and
// chrome use separate instances of the same M4CK blob. Latin/punct fallback
// CJK is U+3400–U+9FFF occupancy. Latin/punct is stamped from the native-grid
// 15/16 occupancy with the same class-1 kernel as CJK at this pixelSize.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "../../src/util/CenterKernelFont.h"
#include "EpdFont.h"
#include "EpdFontData.h"

class CenterKernelEpdFont final : public EpdFont {
 public:
  CenterKernelEpdFont() : EpdFont(&data_) {}
  CenterKernelEpdFont(const uint8_t* blob, size_t size, int pixelSize = CenterKernelFont::kDefaultPx)
      : EpdFont(&data_) {
    bind(blob, size);
    setPixelSize(pixelSize);
  }
  ~CenterKernelEpdFont() override { freeScratch(); }

  CenterKernelEpdFont(const CenterKernelEpdFont&) = delete;
  CenterKernelEpdFont& operator=(const CenterKernelEpdFont&) = delete;

  bool bind(const uint8_t* blob, size_t size) {
    freeScratch();
    blob_ = nullptr;
    size_ = 0;
    valid_ = false;
    glyphCount_ = 0;
    leafCount_ = 0;
    pageDir_ = nullptr;
    leaves_ = nullptr;
    bitmapsOff_ = 0;
    classesOff_ = 0;
    bitmapBytes_ = 32;
    classBytes_ = 0;
    data_ = {};

    if (!blob || size < kHeaderBytes) return false;
    if (rd32(blob) != kMagic) return false;
    const uint16_t glyphCount = rd16(blob + 8);
    const uint16_t leafCount = rd16(blob + 10);
    const uint8_t gridW = blob[12];
    const uint8_t gridH = blob[13];
    if (gridW != 16 || gridH != 16) return false;

    const uint32_t pageDirOff = rd32(blob + 16);
    const uint32_t leavesOff = rd32(blob + 20);
    const uint32_t bitmapsOff = rd32(blob + 24);
    const uint32_t classesOff = rd32(blob + 28);
    const uint16_t bitmapBytes = rd16(blob + 32);
    // class bytes at 34
    const uint16_t classBytes = rd16(blob + 34);
    if (bitmapBytes != 32) return false;
    if (pageDirOff != kHeaderBytes) return false;
    if (pageDirOff + 512u != leavesOff) return false;
    // leaves *34 + bitmaps...
    if (leavesOff + static_cast<uint32_t>(leafCount) * kLeafBytes != bitmapsOff) return false;
    if (bitmapsOff + static_cast<uint32_t>(glyphCount) * 32u != classesOff) return false;
    const uint32_t classEnd = classesOff + static_cast<uint32_t>(classBytes);
    if (classEnd > size) return false;
    // additionally check total size matches header
    const uint32_t expected = kHeaderBytes + 512u + static_cast<uint32_t>(leafCount)*kLeafBytes + static_cast<uint32_t>(glyphCount)*32u + static_cast<uint32_t>(classBytes);
    if (expected != size) {
      // allow larger blob (extra padding) but not smaller
      if (size < expected) return false;
    }

    blob_ = blob;
    size_ = size;
    glyphCount_ = glyphCount;
    leafCount_ = leafCount;
    pageDir_ = blob + pageDirOff;
    leaves_ = blob + leavesOff;
    bitmapsOff_ = bitmapsOff;
    classesOff_ = classesOff;
    bitmapBytes_ = bitmapBytes;
    classBytes_ = classBytes;

    data_.bitmap = blob;
    data_.glyph = nullptr;
    data_.intervals = nullptr;
    data_.intervalCount = 0;
    // advanceY and ascender track current pixelSize, filled by setPixelSize
    updateMetrics();
    valid_ = true;
    return true;
  }

  bool valid() const { return valid_; }
  uint16_t glyphCount() const { return glyphCount_; }
  uint16_t leafCount() const { return leafCount_; }

  void setPixelSize(int n) {
    if (n < 1) n = 1;
    if (n > 255) n = 255;
    pixelSize_ = n;
    if (valid_) updateMetrics();
  }
  int pixelSize() const { return pixelSize_; }

  // Fallback for non-CJK (Latin/punct/digits) and when blob not bound.
  // Corpus is CJK U+3400-U+9FFF only; missing glyphs delegate to native-grid.
  void setFallback(const EpdFont* fb) { fallback_ = fb; }
  const EpdFont* fallback() const { return fallback_; }

  // For host tests / loader convenience.
  void bindReaderBody(int pixelSize) { setPixelSize(pixelSize); }

  const EpdGlyph* getGlyph(uint32_t cp, const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const override {
    (void)style;
    // If CenterKernel not bound, delegate to native-grid fallback so reader not blank.
    if (!valid_) {
      if (fallback_) return fallback_->getGlyph(cp, style);
      return nullptr;
    }
    int32_t rank = rankedIndex(cp);
    if (rank < 0) {
      return latinGlyph(cp, style);
    }
    int cls = glyphClass(rank);
    if (cls < 0 || cls > 3) cls = 1;
    EpdGlyph& g = nextScratch(static_cast<uint32_t>(rank));
    const int N = pixelSize_;
    g.width = static_cast<uint8_t>(N);
    g.height = static_cast<uint8_t>(N);
    int adv = advanceForCp(cp, static_cast<uint32_t>(rank), cls);
    if (adv < 1) adv = 1;
    if (adv > 255) adv = 255;
    g.advanceX = static_cast<uint8_t>(adv);
    g.left = 0;
    g.top = static_cast<int16_t>(N);
    const uint32_t pixels = static_cast<uint32_t>(N) * static_cast<uint32_t>(N);
    g.dataLength = (pixels + 7u) / 8u;
    // Store rank for bitmap reconstruction; not a blob offset.
    g.dataOffset = static_cast<uint32_t>(rank);
    return &g;
  }

  int glyphAdvanceX(uint32_t cp, const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const override {
    // Delegate to CenterKernel if present, otherwise fallback.
    if (valid_) {
      int32_t rank = rankedIndex(cp);
      if (rank >= 0) {
        int cls = glyphClass(rank);
        if (cls < 0 || cls > 3) cls = 1;
        return advanceForCp(cp, static_cast<uint32_t>(rank), cls);
      }
    }
    if (fallback_) {
      const int src = fallback_->glyphAdvanceX(cp, style);
      const int scaled = (src * pixelSize_ + CenterKernelFont::kCellPx / 2) / CenterKernelFont::kCellPx;
      return scaled < 1 ? 1 : scaled;
    }
    const EpdGlyph* g = getGlyph(cp, style);
    if (!g) {
      g = getGlyph('?', style);
      if (!g && fallback_) return fallback_->glyphAdvanceX('?', style);
      if (!g) return CenterKernelFont::advancePx(pixelSize_, 1);
    }
    return g->advanceX;
  }

  const uint8_t* loadGlyphBitmap(const EpdGlyph* glyph, uint8_t* buffer,
                                 const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const override {
    (void)style;
    if (!glyph) return nullptr;
    // If glyph is not one of our scratch glyphs, it must be from fallback.
    bool isScratch = false;
    int32_t rank = -1;
    for (int i = 0; i < 4; ++i) {
      if (&scratch_[i] == glyph) {
        isScratch = true;
        rank = static_cast<int32_t>(scratchRank_[i]);
        break;
      }
    }
    if (!isScratch) {
      if (fallback_) return fallback_->loadGlyphBitmap(glyph, buffer, style);
      if (!valid_) return nullptr;
      if (glyph->dataOffset < glyphCount_) rank = static_cast<int32_t>(glyph->dataOffset);
      else return nullptr;
    }
    if ((static_cast<uint32_t>(rank) & kLatinSentinel) != 0) {
      return stampLatin(static_cast<uint32_t>(rank) & ~kLatinSentinel, glyph, buffer, style);
    }
    if (!valid_) {
      if (fallback_) return fallback_->loadGlyphBitmap(glyph, buffer, style);
      return nullptr;
    }
    if (rank < 0 || static_cast<uint32_t>(rank) >= glyphCount_) {
      if (fallback_) return fallback_->loadGlyphBitmap(glyph, buffer, style);
      return nullptr;
    }
    const uint8_t* occBmp = blob_ + bitmapsOff_ + static_cast<uint32_t>(rank) * 32u;
    if (occBmp + 32 > blob_ + size_) return nullptr;
    int cls = glyphClass(rank);
    if (cls < 0 || cls > 3) cls = 1;
    const int N = pixelSize_;
    const int Kx = CenterKernelFont::kernelX(N, cls);
    const int Ky = CenterKernelFont::kernelY(N, cls);
    const uint32_t dstLen = glyph->dataLength;
    if (dstLen == 0) return nullptr;
    uint8_t* dst = buffer;
    if (!dst) {
      if (!ensureScratch(dstLen)) return nullptr;
      dst = scratchBitmap_;
    }
    std::memset(dst, 0, dstLen);
    // OR-composite kernels at absolute centers.
    for (int row = 0; row < 16; ++row) {
      for (int col = 0; col < 16; ++col) {
        const int idx = row * 16 + col;
        const int on = (occBmp[idx / 8] >> (7 - (idx % 8))) & 1;
        if (!on) continue;
        const int cx = CenterKernelFont::centerX(N, cls, col);
        const int cy = CenterKernelFont::centerY(N, row);
        const int x0 = CenterKernelFont::origin0(cx, Kx);
        const int y0 = CenterKernelFont::origin0(cy, Ky);
        for (int ky = 0; ky < Ky; ++ky) {
          const int y = y0 + ky;
          if (y < 0 || y >= N) continue;
          for (int kx = 0; kx < Kx; ++kx) {
            const int x = x0 + kx;
            if (x < 0 || x >= N) continue;
            const uint32_t p = static_cast<uint32_t>(y) * static_cast<uint32_t>(N) + static_cast<uint32_t>(x);
            dst[p / 8] |= static_cast<uint8_t>(0x80 >> (p % 8));
          }
        }
      }
    }
    return dst;
  }

  const EpdFontData* getData(const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const override {
    (void)style;
    if (valid_) return &data_;
    if (fallback_) return fallback_->getData(style);
    return valid_ ? &data_ : nullptr;
  }

 private:
  static constexpr uint32_t kMagic = 0x4B43344Du; // 'M4CK' LE
  static constexpr uint16_t kVersion = 1;
  static constexpr uint16_t kHeaderBytes = 48;
  static constexpr uint16_t kLeafBytes = 34;
  static constexpr uint32_t kLatinSentinel = 0x80000000u;

  const EpdGlyph* latinGlyph(uint32_t cp, const EpdFontStyles::Style style) const {
    if (!fallback_) return nullptr;
    const EpdGlyph* src = fallback_->getGlyph(cp, style);
    if (!src) return nullptr;
    const int N = pixelSize_;
    EpdGlyph& g = nextScratch(kLatinSentinel | (cp & ~kLatinSentinel));
    g.width = static_cast<uint8_t>(N);
    g.height = static_cast<uint8_t>(N);
    const int srcAdv = fallback_->glyphAdvanceX(cp, style);
    int adv = (srcAdv * N + CenterKernelFont::kCellPx / 2) / CenterKernelFont::kCellPx;
    if (adv < 1) adv = 1;
    if (adv > 255) adv = 255;
    g.advanceX = static_cast<uint8_t>(adv);
    g.left = 0;
    g.top = static_cast<int16_t>(N);
    const uint32_t pixels = static_cast<uint32_t>(N) * static_cast<uint32_t>(N);
    g.dataLength = (pixels + 7u) / 8u;
    g.dataOffset = kLatinSentinel | (cp & ~kLatinSentinel);
    return &g;
  }

  const uint8_t* stampOcc(const uint8_t occ[32], int cls, const EpdGlyph* glyph, uint8_t* buffer) const {
    const int N = pixelSize_;
    const int Kx = CenterKernelFont::kernelX(N, cls);
    const int Ky = CenterKernelFont::kernelY(N, cls);
    const uint32_t dstLen = glyph->dataLength;
    if (dstLen == 0) return nullptr;
    uint8_t* dst = buffer;
    if (!dst) {
      if (!ensureScratch(dstLen)) return nullptr;
      dst = scratchBitmap_;
    }
    std::memset(dst, 0, dstLen);
    for (int row = 0; row < 16; ++row) {
      for (int col = 0; col < 16; ++col) {
        const int idx = row * 16 + col;
        const int on = (occ[idx / 8] >> (7 - (idx % 8))) & 1;
        if (!on) continue;
        const int cx = CenterKernelFont::centerX(N, cls, col);
        const int cy = CenterKernelFont::centerY(N, row);
        const int x0 = CenterKernelFont::origin0(cx, Kx);
        const int y0 = CenterKernelFont::origin0(cy, Ky);
        for (int ky = 0; ky < Ky; ++ky) {
          const int y = y0 + ky;
          if (y < 0 || y >= N) continue;
          for (int kx = 0; kx < Kx; ++kx) {
            const int x = x0 + kx;
            if (x < 0 || x >= N) continue;
            const uint32_t p = static_cast<uint32_t>(y) * static_cast<uint32_t>(N) + static_cast<uint32_t>(x);
            dst[p / 8] |= static_cast<uint8_t>(0x80 >> (p % 8));
          }
        }
      }
    }
    return dst;
  }

  const uint8_t* stampLatin(uint32_t cp, const EpdGlyph* glyph, uint8_t* buffer,
                            const EpdFontStyles::Style style) const {
    if (!fallback_ || !glyph) return nullptr;
    const EpdGlyph* src = fallback_->getGlyph(cp, style);
    if (!src) return nullptr;
    uint8_t nativeBuf[32];
    const uint8_t* srcBmp = fallback_->loadGlyphBitmap(src, nativeBuf, style);
    if (!srcBmp) return nullptr;
    uint8_t occ[32];
    std::memset(occ, 0, sizeof(occ));
    const int w = src->width;
    const int h = src->height;
    for (int row = 0; row < 16 && row < h; ++row) {
      for (int col = 0; col < 16 && col < w; ++col) {
        const uint32_t sidx = static_cast<uint32_t>(row) * static_cast<uint32_t>(w) + static_cast<uint32_t>(col);
        if (((srcBmp[sidx / 8u] >> (7u - (sidx % 8u))) & 1u) == 0) continue;
        const int didx = row * 16 + col;
        occ[didx / 8] |= static_cast<uint8_t>(0x80 >> (didx % 8));
      }
    }
    return stampOcc(occ, 1, glyph, buffer);
  }

  static uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8)); }
  static uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
  }

  void freeScratch() const {
    if (scratchBitmap_) {
      std::free(scratchBitmap_);
      scratchBitmap_ = nullptr;
      scratchBitmapSize_ = 0;
    }
  }
  bool ensureScratch(size_t need) const {
    if (scratchBitmap_ && scratchBitmapSize_ >= need) return true;
    std::free(scratchBitmap_);
    scratchBitmap_ = static_cast<uint8_t*>(std::malloc(need));
    scratchBitmapSize_ = scratchBitmap_ ? need : 0;
    return scratchBitmap_ != nullptr;
  }

  EpdGlyph& nextScratch(uint32_t rank) const {
    scratchI_ = static_cast<uint8_t>((scratchI_ + 1u) & 3u);
    scratch_[scratchI_] = {};
    scratchRank_[scratchI_] = rank;
    return scratch_[scratchI_];
  }

  void updateMetrics() {
    data_.advanceY = static_cast<uint8_t>(pixelSize_);
    data_.ascender = static_cast<int8_t>(pixelSize_);
    data_.descender = 0;
    data_.is2Bit = false;
  }

  int32_t rankedIndex(uint32_t cp) const {
    if (cp > 0xFFFFu) return -1;
    const uint16_t u = static_cast<uint16_t>(cp);
    const uint8_t page = static_cast<uint8_t>(u >> 8);
    const uint8_t bit = static_cast<uint8_t>(u & 0xFFu);
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
    return static_cast<int32_t>(rank);
  }

  static bool fullwidthCp(uint32_t cp) {
    if (cp >= 0x3400u && cp <= 0x9FFFu) return true;
    if (cp >= 0x3000u && cp <= 0x303Fu) return true;
    if (cp >= 0xFF01u && cp <= 0xFF60u) return true;
    if (cp >= 0xFFE0u && cp <= 0xFFE6u) return true;
    return false;
  }

  int occupancyInkWidth(uint32_t rank) const {
    if (!blob_ || rank >= glyphCount_) return 0;
    const uint8_t* occ = blob_ + bitmapsOff_ + rank * 32u;
    int lo = 16;
    int hi = -1;
    for (int row = 0; row < 16; ++row) {
      for (int col = 0; col < 16; ++col) {
        const int idx = row * 16 + col;
        if (((occ[idx / 8] >> (7 - (idx % 8))) & 1) == 0) continue;
        if (col < lo) lo = col;
        if (col > hi) hi = col;
      }
    }
    if (hi < 0) return 0;
    return hi - lo + 1;
  }

  int advanceForCp(uint32_t cp, uint32_t rank, int cls) const {
    const int N = pixelSize_;
    if (cp == ' ' || cp == 0x00A0u) {
      int adv = (4 * N + CenterKernelFont::kCellPx / 2) / CenterKernelFont::kCellPx;
      return adv < 1 ? 1 : adv;
    }
    if (fullwidthCp(cp)) return CenterKernelFont::advancePx(N, cls);
    const int inkW = occupancyInkWidth(rank);
    int lsb = 1;
    int rsb = 1;
    switch (cp) {
      case '(':
      case '[':
      case '{':
      case '<':
      case 0x2018u:
      case 0x201Cu:
        lsb = 2;
        rsb = 1;
        break;
      case ')':
      case ']':
      case '}':
      case '>':
      case 0x2019u:
      case 0x201Du:
        lsb = 1;
        rsb = 2;
        break;
      default:
        break;
    }
    const int cells = (inkW > 0) ? (lsb + inkW + rsb) : 4;
    int adv = (cells * N + CenterKernelFont::kCellPx / 2) / CenterKernelFont::kCellPx;
    return adv < 1 ? 1 : adv;
  }

  int glyphClass(int32_t rank) const {
    if (rank < 0 || static_cast<uint32_t>(rank) >= glyphCount_) return 1;
    if (!blob_ || classesOff_ >= size_) return 1;
    const uint32_t byteIdx = static_cast<uint32_t>(rank) >> 2;
    const uint8_t shift = static_cast<uint8_t>((rank & 3) * 2);
    if (classesOff_ + byteIdx >= size_) return 1;
    return (blob_[classesOff_ + byteIdx] >> shift) & 3;
  }

  const uint8_t* blob_ = nullptr;
  size_t size_ = 0;
  bool valid_ = false;
  uint16_t glyphCount_ = 0;
  uint16_t leafCount_ = 0;
  const uint8_t* pageDir_ = nullptr;
  const uint8_t* leaves_ = nullptr;
  uint32_t bitmapsOff_ = 0;
  uint32_t classesOff_ = 0;
  uint16_t bitmapBytes_ = 32;
  uint16_t classBytes_ = 0;
  int pixelSize_ = CenterKernelFont::kDefaultPx;
  EpdFontData data_{};
  const EpdFont* fallback_ = nullptr;
  mutable EpdGlyph scratch_[4]{};
  mutable uint32_t scratchRank_[4]{};
  mutable uint8_t scratchI_ = 0;
  mutable uint8_t* scratchBitmap_ = nullptr;
  mutable size_t scratchBitmapSize_ = 0;
};
