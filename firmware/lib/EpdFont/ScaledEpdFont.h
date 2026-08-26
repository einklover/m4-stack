#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(ESP32) && defined(BOARD_HAS_PSRAM)
#include <Arduino.h>  // psramFound / ps_malloc
#endif

#include "EpdFont.h"

// Lightweight metric/bitmap view over an existing EpdFont.
//
// Murphy M4 uses this for:
//   * system *reader* NOTOSANS_16 — builtin native-grid snaps to integer N
//     via bindInteger; canonical 16px SD epdfont keeps an arbitrary float
//     dest-sample scale
//   * system *chrome* SMALL/UI_10/UI_12 — bindInteger at a fixed chrome N,
//     never readerPixelSize, never a reader TTF
//
// There is still exactly one source face with one cmap, one SD stream and one
// glyph LRU. A ScaledEpdFont only borrows that face, scales the EpdGlyph
// metrics, and resamples the already-cached source bitmap into one reusable
// PSRAM scratch buffer. Runtime TTF/OTF reader faces do not use this scaler;
// they rasterize at the selected pixel size.
//
// Native-grid 1-bit MUST use integer Kronecker replication (bindInteger):
// dest(sx*N+ix, sy*N+iy) = src(sx,sy) for ix,iy in 0..N-1. A 1px source
// stroke becomes exactly N dest pixels everywhere. The old coverage0/coverage1
// arbitrary-ratio blit is gone from this path — it produced 29x31-class
// rasters with mixed 1/2/3 dest pixels per source pixel. Canonical 2-bit
// epdfonts still dest-sample at an arbitrary float scale.
class ScaledEpdFont final : public EpdFont {
 public:
  ScaledEpdFont() : EpdFont(&scaledData_) {}
  ~ScaledEpdFont() override { freeScratch(); }

  ScaledEpdFont(const ScaledEpdFont&) = delete;
  ScaledEpdFont& operator=(const ScaledEpdFont&) = delete;

  // Native 1-bit integer pixel replication. n>=1. Metrics and bitmap scale
  // by exact integer multiply; there is no lround(px/16) and no coverage map.
  void bindInteger(const EpdFont* source, int n) {
    if (n < 1) n = 1;
    source_ = source;
    integerScale_ = n;
    scale_ = static_cast<float>(n);
    replicateSourcePixels_ = true;
    scaledCodepoint_ = 0;
    fillScaledData();
  }

  // Canonical / non-native arbitrary scale. 2-bit faces always dest-sample.
  // If a 1-bit caller still passes replicateSourcePixels=true, snap to nearest
  // integer N so native-grid cannot re-enter coverage mapping. Production
  // native-grid must call bindInteger with M4FontPolicy::nativeGridIntegerScale
  // (lround of an arbitrary ratio is not the reader-size policy).
  void bind(const EpdFont* source, float scale, bool replicateSourcePixels = false) {
    source_ = source;
    if (scale <= 0.0f) scale = 1.0f;
    const EpdFontData* src = source_ ? source_->getData(EpdFontStyles::REGULAR) : nullptr;
    if (src && !src->is2Bit && replicateSourcePixels) {
      int n = static_cast<int>(std::lround(scale));
      if (n < 1) n = 1;
      bindInteger(source, n);
      return;
    }
    integerScale_ = 0;
    scale_ = scale;
    replicateSourcePixels_ = false;
    scaledCodepoint_ = 0;
    fillScaledData();
    if (src && src->is2Bit) replicateSourcePixels_ = false;
  }

  const EpdFont* source() const { return source_; }
  float scale() const { return scale_; }
  int integerScale() const { return integerScale_; }
  bool isUnityScale() const {
    if (integerScale_ > 0) return integerScale_ == 1;
    return scale_ >= 0.999f && scale_ <= 1.001f;
  }
  bool replicatesSourcePixels() const { return replicateSourcePixels_; }

  const EpdGlyph* getGlyph(uint32_t cp,
                           const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const override {
    if (!source_) return nullptr;
    const EpdGlyph* src = source_->getGlyph(cp, style);
    if (!src) return nullptr;
    if (isUnityScale()) return src;

    scaledGlyph_ = {};
    if (integerScale_ > 1) {
      scaledGlyph_.width = mulU8(src->width, integerScale_);
      scaledGlyph_.height = mulU8(src->height, integerScale_);
      scaledGlyph_.advanceX = mulU8(src->advanceX, integerScale_);
      scaledGlyph_.left = static_cast<int16_t>(src->left * integerScale_);
      scaledGlyph_.top = static_cast<int16_t>(src->top * integerScale_);
    } else {
      scaledGlyph_.width = scaledDim(src->width);
      scaledGlyph_.height = scaledDim(src->height);
      scaledGlyph_.advanceX = scaledU8(src->advanceX);
      scaledGlyph_.left = static_cast<int16_t>(scaledSigned(src->left));
      scaledGlyph_.top = static_cast<int16_t>(scaledSigned(src->top));
    }
    const uint32_t pixels = static_cast<uint32_t>(scaledGlyph_.width) * scaledGlyph_.height;
    scaledGlyph_.dataLength = scaledData_.is2Bit ? ((pixels + 3u) / 4u) : ((pixels + 7u) / 8u);
    // Keep the codepoint, not the source bitmap offset. Built-in bitmap faces
    // use dataOffset as a bitmap offset while runtime TTF uses it as a cache
    // key, so the codepoint is the only stable resampling identity.
    scaledCodepoint_ = cp;
    scaledGlyph_.dataOffset = 0;
    return &scaledGlyph_;
  }

  int glyphAdvanceX(uint32_t cp,
                    const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const override {
    const EpdGlyph* glyph = getGlyph(cp, style);
    if (!glyph) glyph = getGlyph('?', style);
    return glyph ? glyph->advanceX : 0;
  }

  const uint8_t* loadGlyphBitmap(const EpdGlyph* glyph, uint8_t* buffer,
                                 const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const override {
    if (!source_ || !glyph) return nullptr;
    if (isUnityScale()) return source_->loadGlyphBitmap(glyph, buffer, style);

    // Re-resolve without retaining a source glyph pointer across cache
    // accesses. This works for both builtin bitmap offsets and runtime TTF.
    const EpdGlyph* srcGlyph = source_->getGlyph(scaledCodepoint_, style);
    if (!srcGlyph) return nullptr;
    const uint8_t* srcBitmap = source_->loadGlyphBitmap(srcGlyph, nullptr, style);
    if (!srcBitmap || glyph->dataLength == 0) return nullptr;

    uint8_t* dst = buffer;
    if (!dst) {
      if (!ensureScratch(glyph->dataLength)) return nullptr;
      dst = scratch_;
    }
    std::memset(dst, 0, glyph->dataLength);

    const int sw = srcGlyph->width;
    const int sh = srcGlyph->height;
    const int dw = glyph->width;
    const int dh = glyph->height;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return dst;

    const EpdFontData* srcData = source_->getData(style);
    const bool is2Bit = srcData && srcData->is2Bit;
    if (!is2Bit && integerScale_ > 1) {
      blitInteger1Bit(srcBitmap, dst, sw, sh, integerScale_, dw, dh);
    } else {
      blitDestSample(srcBitmap, dst, sw, sh, dw, dh, is2Bit);
    }
    return dst;
  }

  const EpdFontData* getData(const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const override {
    (void)style;
    return source_ ? &scaledData_ : nullptr;
  }

 private:
  static int clampInt(int n, int lo, int hi) {
    if (n < lo) return lo;
    if (n > hi) return hi;
    return n;
  }

  static uint8_t mulU8(uint8_t v, int n) {
    if (v == 0) return 0;
    return static_cast<uint8_t>(clampInt(static_cast<int>(v) * n, 1, 255));
  }

  void fillScaledData() {
    const EpdFontData* src = source_ ? source_->getData(EpdFontStyles::REGULAR) : nullptr;
    scaledData_ = {};
    if (!src) return;
    scaledData_.bitmap = nullptr;
    scaledData_.glyph = nullptr;
    scaledData_.intervals = nullptr;
    scaledData_.intervalCount = 0;
    if (integerScale_ > 0) {
      scaledData_.advanceY = mulU8(src->advanceY, integerScale_);
      scaledData_.ascender = src->ascender * integerScale_;
      scaledData_.descender = src->descender * integerScale_;
    } else {
      scaledData_.advanceY = scaledU8(src->advanceY);
      scaledData_.ascender = scaledSigned(src->ascender);
      scaledData_.descender = scaledSigned(src->descender);
    }
    scaledData_.is2Bit = src->is2Bit;
  }

  uint8_t scaledDim(uint8_t v) const {
    if (v == 0) return 0;
    const int n = clampInt(static_cast<int>(std::lround(v * scale_)), 1, 255);
    return static_cast<uint8_t>(n);
  }

  uint8_t scaledU8(uint8_t v) const {
    if (v == 0) return 0;
    const int n = clampInt(static_cast<int>(std::lround(v * scale_)), 1, 255);
    return static_cast<uint8_t>(n);
  }

  int scaledSigned(int v) const { return static_cast<int>(std::lround(v * scale_)); }

  static void setBit1(uint8_t* dst, int w, int x, int y) {
    const uint32_t idx = static_cast<uint32_t>(y) * static_cast<uint32_t>(w) + static_cast<uint32_t>(x);
    dst[idx / 8u] |= static_cast<uint8_t>(1u << (7u - (idx % 8u)));
  }

  static bool getBit1(const uint8_t* src, int w, int x, int y) {
    const uint32_t idx = static_cast<uint32_t>(y) * static_cast<uint32_t>(w) + static_cast<uint32_t>(x);
    return ((src[idx / 8u] >> (7u - (idx % 8u))) & 1u) != 0;
  }

  // Exact nearest-neighbor / Kronecker replication. Dest size is sw*N x sh*N
  // (stored bitmap; CJK's extra logical column is advance, not stored pixels).
  static void blitInteger1Bit(const uint8_t* src, uint8_t* dst, int sw, int sh, int n, int dw, int dh) {
    if (n < 1) n = 1;
    for (int sy = 0; sy < sh; ++sy) {
      for (int iy = 0; iy < n; ++iy) {
        const int dy = sy * n + iy;
        if (dy < 0 || dy >= dh) continue;
        for (int sx = 0; sx < sw; ++sx) {
          if (!getBit1(src, sw, sx, sy)) continue;
          for (int ix = 0; ix < n; ++ix) {
            const int dx = sx * n + ix;
            if (dx < 0 || dx >= dw) continue;
            setBit1(dst, dw, dx, dy);
          }
        }
      }
    }
  }

  static void blitDestSample(const uint8_t* src, uint8_t* dst, int sw, int sh, int dw, int dh,
                             bool is2Bit) {
    for (int dy = 0; dy < dh; ++dy) {
      int sy = dy * sh / dh;
      if (sy >= sh) sy = sh - 1;
      for (int dx = 0; dx < dw; ++dx) {
        int sx = dx * sw / dw;
        if (sx >= sw) sx = sw - 1;
        const uint32_t sidx = static_cast<uint32_t>(sy) * sw + sx;
        const uint32_t didx = static_cast<uint32_t>(dy) * dw + dx;
        if (is2Bit) {
          const uint8_t val = (src[sidx / 4u] >> ((3u - (sidx % 4u)) * 2u)) & 0x3u;
          dst[didx / 4u] |= static_cast<uint8_t>(val << ((3u - (didx % 4u)) * 2u));
        } else if (getBit1(src, sw, sx, sy)) {
          dst[didx / 8u] |= static_cast<uint8_t>(1u << (7u - (didx % 8u)));
        }
      }
    }
  }

  bool ensureScratch(size_t n) const {
    if (n <= scratchBytes_ && scratch_) return true;
    uint8_t* p = nullptr;
#if defined(ESP32) && defined(BOARD_HAS_PSRAM)
    if (psramFound()) p = static_cast<uint8_t*>(ps_malloc(n));
#endif
    if (!p) p = static_cast<uint8_t*>(std::malloc(n));
    if (!p) return false;
    std::free(scratch_);
    scratch_ = p;
    scratchBytes_ = n;
    return true;
  }

  void freeScratch() {
    std::free(scratch_);
    scratch_ = nullptr;
    scratchBytes_ = 0;
  }

  const EpdFont* source_ = nullptr;  // borrowed; owned by FontManager/builtin table
  float scale_ = 1.0f;
  int integerScale_ = 0;  // >0 = Kronecker N; 0 = dest-sample float path
  bool replicateSourcePixels_ = false;
  EpdFontData scaledData_{};
  mutable EpdGlyph scaledGlyph_{};
  mutable uint32_t scaledCodepoint_ = 0;
  mutable uint8_t* scratch_ = nullptr;
  mutable size_t scratchBytes_ = 0;
};
