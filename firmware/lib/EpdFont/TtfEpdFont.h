#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <SDCardManager.h>

#include <cstddef>
#include <cstdint>

#include "EpdFont.h"
#include "TtfReader.h"

// Runtime TrueType font backend for the existing EpdFont interface.
//
// The same rasterizer can read either:
//   * an SD path (normal user/reader TTF), or
//   * a const byte array in memory-mapped flash (tiny built-in emergency TTF).
//
// Only the table directory + cmap + bounded LRU metadata stay resident. Glyph
// bitmaps are 2-bit grayscale (is2Bit=true) matching GfxRenderer's packing and
// are allocated PSRAM-first. The flash constructor deliberately defaults to a
// much smaller cache because an emergency/UI subset has very few glyphs.
//
// getGlyph()/loadGlyphBitmap() are const but mutate the LRU cache; guarded by a
// mutex so the face can be shared across render tasks.
class TtfEpdFont : public EpdFont {
 public:
  static constexpr uint16_t kDefaultRuntimeSlots = 512;
  static constexpr size_t kDefaultRuntimeBudget = 768 * 1024;
  static constexpr uint16_t kDefaultEmbeddedSlots = 96;
  static constexpr size_t kDefaultEmbeddedBudget = 96 * 1024;

  TtfEpdFont(const String& path, uint16_t sizePx, uint16_t maxSlots = kDefaultRuntimeSlots,
             size_t cacheBudget = kDefaultRuntimeBudget);
  // `data` is borrowed and must remain valid for the lifetime of this object.
  // A `static const uint8_t[]` compiled into firmware is the intended source.
  TtfEpdFont(const uint8_t* data, uint32_t dataSize, uint16_t sizePx,
             uint16_t maxSlots = kDefaultEmbeddedSlots,
             size_t cacheBudget = kDefaultEmbeddedBudget);
  ~TtfEpdFont() override;

  const EpdGlyph* getGlyph(uint32_t cp, const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const override;
  const uint8_t* loadGlyphBitmap(const EpdGlyph* glyph, uint8_t* buffer,
                                 const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const override;
  const EpdFontData* getData(const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const override { return &data_; }

  bool isRuntimeTtf() const override { return true; }

  bool valid() const { return valid_; }
  const char* lastError() const { return font_.lastError(); }
  uint16_t sizePx() const { return sizePx_; }
  uint16_t maxSlots() const { return maxSlots_; }
  size_t cacheBudget() const { return cacheBudget_; }

  // Exact cmap coverage check without the getGlyph() '?' fallback. UI code
  // uses this before replacing the built-in CJK chrome with a user TTF so a
  // partial/Latin font cannot turn system labels into question marks.
  bool hasCodepoint(uint32_t cp) const {
    uint16_t gid = 0;
    return valid_ && font_.findGlyph(cp, gid) && gid != 0;
  }

  // Free cached bitmaps (called on font-switch to bound memory).
  void clearCaches();

 private:
  struct Entry {
    uint32_t cp = 0xFFFFFFFF;  // key; 0xFFFFFFFF = empty
    uint32_t lastAccess = 0;
    EpdGlyph glyph{};
    uint8_t* bitmap = nullptr;
    uint32_t bitmapSize = 0;
  };

  // Ensure a codepoint's glyph+bitmap is cached; returns slot index or -1.
  // Caller must hold mutex_.
  int ensureGlyph(uint32_t cp) const;
  void evictSlot(int slot) const;
  bool allocateEntries();
  bool finishInit(const char* sourceLabel);

  String path_;
  uint16_t sizePx_ = 0;
  uint16_t maxSlots_ = kDefaultRuntimeSlots;
  size_t cacheBudget_ = kDefaultRuntimeBudget;
  bool valid_ = false;
  EpdFontData data_{};

  ttf::TtfStream* stream_ = nullptr;  // owned stream object; memory bytes are borrowed
  mutable ttf::TtfFont font_;

  mutable Entry* entries_ = nullptr;  // maxSlots_, PSRAM-first
  mutable uint32_t accessCounter_ = 0;
  mutable size_t cacheBytes_ = 0;
#if defined(ESP32)
  mutable SemaphoreHandle_t mutex_ = nullptr;
#endif

  static void* ttfAlloc(size_t n);
  static void ttfFree(void* p);
};