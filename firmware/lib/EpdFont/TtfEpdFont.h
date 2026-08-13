#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <SDCardManager.h>
#include <cstddef>
#include <cstdint>
#include "EpdFont.h"
#include "TtfReader.h"
#include "CffReader.h"

// Runtime sfnt font backend for the existing EpdFont interface. Only the active
// outline backend allocates resident cmap/raster scratch; glyph cache bitmaps
// remain PSRAM-first and shared by glyf/CFF1/CFF2 paths.
class TtfEpdFont : public EpdFont {
 public:
  static constexpr uint16_t kDefaultRuntimeSlots = 512;
  static constexpr size_t kDefaultRuntimeBudget = 768 * 1024;
  static constexpr uint16_t kDefaultEmbeddedSlots = 96;
  static constexpr size_t kDefaultEmbeddedBudget = 96 * 1024;

  TtfEpdFont(const String& path, uint16_t sizePx, uint16_t maxSlots = kDefaultRuntimeSlots,
             size_t cacheBudget = kDefaultRuntimeBudget);
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
  const char* lastError() const;
  uint16_t sizePx() const { return sizePx_; }
  uint16_t maxSlots() const { return maxSlots_; }
  size_t cacheBudget() const { return cacheBudget_; }
  bool hasCodepoint(uint32_t cp) const;
  void clearCaches();

 private:
  enum class Backend : uint8_t { Glyf, Cff1, Cff2 };
  struct Entry {
    uint32_t cp = 0xFFFFFFFF;
    uint32_t lastAccess = 0;
    EpdGlyph glyph{};
    uint8_t* bitmap = nullptr;
    uint32_t bitmapSize = 0;
  };

  bool usesCffBackend() const { return backend_ != Backend::Glyf; }
  int ensureGlyph(uint32_t cp) const;
  void evictSlot(int slot) const;
  bool allocateEntries();
  bool finishInit(const char* sourceLabel);
  bool backendFindGlyph(uint32_t cp, uint16_t& gid) const;
  bool backendRasterize(uint16_t gid, ttf::GlyphBitmap& out) const;
  bool backendPixelBox(uint16_t gid, int& x0, int& y0, int& x1, int& y1) const;
  uint16_t backendUnitsPerEm() const;
  int32_t backendBBoxYMax() const;
  void backendVMetrics(int32_t& asc, int32_t& desc, int32_t& gap) const;
  const char* backendError() const;
  const char* backendName() const;
  void backendClearScratch();

  String path_;
  String runtimeError_;
  uint16_t sizePx_ = 0;
  uint16_t maxSlots_ = kDefaultRuntimeSlots;
  size_t cacheBudget_ = kDefaultRuntimeBudget;
  bool valid_ = false;
  Backend backend_ = Backend::Glyf;
  // Absolute sfnt directory offset in the original stream. Zero for standalone
  // fonts; non-zero for glyf/CFF1/CFF2 faces inside TTC/OTC collections.
  uint32_t faceOffset_ = 0;
  EpdFontData data_{};

  ttf::TtfStream* stream_ = nullptr;
  mutable ttf::TtfFont font_;
  mutable ttf::CffFont cffFont_;

  mutable Entry* entries_ = nullptr;
  mutable uint32_t accessCounter_ = 0;
  mutable size_t cacheBytes_ = 0;
#if defined(ESP32)
  mutable SemaphoreHandle_t mutex_ = nullptr;
#endif

  static void* ttfAlloc(size_t n);
  static void ttfFree(void* p);
};
