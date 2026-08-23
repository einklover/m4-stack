#pragma once

#include <Arduino.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "EpdFont.h"

// Lightweight metric/bitmap view over an existing EpdFont.
//
// Murphy M4 runtime TTF uses this for compact UI/status font IDs: there is
// still exactly one source face with one cmap, one SD stream and one glyph LRU.
// A ScaledEpdFont only borrows that face, scales the EpdGlyph metrics, and
// resamples the already-cached source bitmap into one reusable PSRAM scratch
// buffer. It never parses/rasterizes the source font itself and never owns a
// second glyph cache. The source may be a streamed TTF/CFF face or the compact
// built-in 2-bit CJK face.
class ScaledEpdFont final : public EpdFont {
 public:
  ScaledEpdFont() : EpdFont(&scaledData_) {}
  ~ScaledEpdFont() override { freeScratch(); }

  ScaledEpdFont(const ScaledEpdFont&) = delete;
  ScaledEpdFont& operator=(const ScaledEpdFont&) = delete;

  void bind(const EpdFont* source, float scale) {
    source_ = source;
    if (scale <= 0.0f) scale = 1.0f;
    scale_ = scale;
    scaledCodepoint_ = 0;

    const EpdFontData* src = source_ ? source_->getData(EpdFontStyles::REGULAR) : nullptr;
    scaledData_ = {};
    if (!src) return;
    scaledData_.bitmap = nullptr;
    scaledData_.glyph = nullptr;
    scaledData_.intervals = nullptr;
    scaledData_.intervalCount = 0;
    scaledData_.advanceY = scaledU8(src->advanceY);
    scaledData_.ascender = scaledSigned(src->ascender);
    scaledData_.descender = scaledSigned(src->descender);
    scaledData_.is2Bit = src->is2Bit;
  }

  const EpdFont* source() const { return source_; }
  float scale() const { return scale_; }

  const EpdGlyph* getGlyph(uint32_t cp,
                           const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const override {
    if (!source_) return nullptr;
    const EpdGlyph* src = source_->getGlyph(cp, style);
    if (!src) return nullptr;
    if (scale_ >= 0.999f) return src;

    scaledGlyph_ = {};
    scaledGlyph_.width = scaledDim(src->width);
    scaledGlyph_.height = scaledDim(src->height);
    scaledGlyph_.advanceX = scaledU8(src->advanceX);
    scaledGlyph_.left = static_cast<int16_t>(scaledSigned(src->left));
    scaledGlyph_.top = static_cast<int16_t>(scaledSigned(src->top));
    const uint32_t pixels = static_cast<uint32_t>(scaledGlyph_.width) * scaledGlyph_.height;
    scaledGlyph_.dataLength = scaledData_.is2Bit ? ((pixels + 3u) / 4u) : ((pixels + 7u) / 8u);
    // Keep the codepoint, not the source bitmap offset. Built-in compact faces
    // use dataOffset as a bitmap offset while runtime TTF uses it as a cache
    // key, so the codepoint is the only stable resampling identity.
    scaledCodepoint_ = cp;
    scaledGlyph_.dataOffset = 0;
    return &scaledGlyph_;
  }

  const uint8_t* loadGlyphBitmap(const EpdGlyph* glyph, uint8_t* buffer,
                                 const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const override {
    if (!source_ || !glyph) return nullptr;
    if (scale_ >= 0.999f) return source_->loadGlyphBitmap(glyph, buffer, style);

    // Re-resolve without retaining a source glyph pointer across cache
    // accesses. This works for both compact bitmap offsets and runtime TTF.
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
    for (int dy = 0; dy < dh; ++dy) {
      int sy = static_cast<int>(dy / scale_);
      if (sy >= sh) sy = sh - 1;
      for (int dx = 0; dx < dw; ++dx) {
        int sx = static_cast<int>(dx / scale_);
        if (sx >= sw) sx = sw - 1;
        const uint32_t sidx = static_cast<uint32_t>(sy) * sw + sx;
        const uint32_t didx = static_cast<uint32_t>(dy) * dw + dx;
        if (is2Bit) {
          const uint8_t val = (srcBitmap[sidx / 4u] >> ((3u - (sidx % 4u)) * 2u)) & 0x3u;
          dst[didx / 4u] |= static_cast<uint8_t>(val << ((3u - (didx % 4u)) * 2u));
        } else {
          const uint8_t val = (srcBitmap[sidx / 8u] >> (7u - (sidx % 8u))) & 0x1u;
          if (val) dst[didx / 8u] |= static_cast<uint8_t>(1u << (7u - (didx % 8u)));
        }
      }
    }
    return dst;
  }

  const EpdFontData* getData(const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const override {
    (void)style;
    return source_ ? &scaledData_ : nullptr;
  }

 private:
  uint8_t scaledDim(uint8_t v) const {
    if (v == 0) return 0;
    const int n = std::max(1, static_cast<int>(std::lround(v * scale_)));
    return static_cast<uint8_t>(std::min(255, n));
  }

  uint8_t scaledU8(uint8_t v) const {
    if (v == 0) return 0;
    const int n = std::max(1, static_cast<int>(std::lround(v * scale_)));
    return static_cast<uint8_t>(std::min(255, n));
  }

  int scaledSigned(int v) const { return static_cast<int>(std::lround(v * scale_)); }

  bool ensureScratch(size_t n) const {
    if (n <= scratchBytes_ && scratch_) return true;
    uint8_t* p = nullptr;
#if defined(ESP32) && defined(BOARD_HAS_PSRAM)
    if (psramFound()) p = static_cast<uint8_t*>(ps_malloc(n));
#endif
    if (!p) p = static_cast<uint8_t*>(malloc(n));
    if (!p) return false;
    free(scratch_);
    scratch_ = p;
    scratchBytes_ = n;
    return true;
  }

  void freeScratch() {
    free(scratch_);
    scratch_ = nullptr;
    scratchBytes_ = 0;
  }

  const EpdFont* source_ = nullptr;  // borrowed; owned by FontManager/builtin table
  float scale_ = 1.0f;
  EpdFontData scaledData_{};
  mutable EpdGlyph scaledGlyph_{};
  mutable uint32_t scaledCodepoint_ = 0;
  mutable uint8_t* scratch_ = nullptr;
  mutable size_t scratchBytes_ = 0;
};
