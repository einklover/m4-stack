#pragma once

#include <Arduino.h>
#include <SDCardManager.h>

#include <vector>

#include "EpdFont.h"

struct BitmapCacheEntry {
  uint32_t codePoint = 0;
  uint32_t lastAccess = 0;
  uint8_t* data = nullptr;
  uint16_t size = 0;
};

struct GlyphStructCacheEntry {
  uint32_t codePoint = 0xFFFFFFFF;  // Invalid initial value
  uint32_t lastAccess = 0;
  EpdGlyph glyph;
};

class CustomEpdFont : public EpdFont {
 public:
  CustomEpdFont(const String& filePath, const EpdFontData* data, uint32_t offsetIntervals, uint32_t offsetGlyphs,
                uint32_t offsetBitmaps, int version = 0);
  ~CustomEpdFont() override;

  const EpdGlyph* getGlyph(uint32_t cp, const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const override;
  const uint8_t* loadGlyphBitmap(const EpdGlyph* glyph, uint8_t* buffer,
                                 const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const override;

 private:
  String filePath;  // SD 卡路径
  mutable FsFile fontFile;  // SD 卡文件
  uint32_t offsetIntervals;
  uint32_t offsetGlyphs;
  uint32_t offsetBitmaps;

  // Bitmap Cache (Pixel data)
  static constexpr size_t BITMAP_CACHE_CAPACITY = 10;
  mutable BitmapCacheEntry bitmapCache[BITMAP_CACHE_CAPACITY];

  // Glyph Struct Cache (Metadata)
  static constexpr size_t GLYPH_CACHE_CAPACITY = 200;
  mutable GlyphStructCacheEntry glyphCache[GLYPH_CACHE_CAPACITY];

  mutable uint32_t currentAccessCount = 0;
  int version = 0;
  
  // 缓存统计（用于调试）
  mutable uint32_t glyphCacheHits = 0;
  mutable uint32_t glyphCacheMisses = 0;
  mutable uint32_t bitmapCacheHits = 0;
  mutable uint32_t bitmapCacheMisses = 0;

  void clearCache() const;
};
