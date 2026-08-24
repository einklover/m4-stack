// Host-only TU that emits EpdFont's vtable without Arduino/Utf8.
// Firmware still links firmware/lib/EpdFont/EpdFont.cpp. Do not add this
// file to PlatformIO or to host targets that already compile EpdFont.cpp.
#include "EpdFont.h"

const EpdGlyph* EpdFont::getGlyph(const uint32_t cp, const EpdFontStyles::Style style) const {
  const EpdFontData* fontData = getData(style);
  if (!fontData) return nullptr;

  const EpdUnicodeInterval* intervals = fontData->intervals;
  const int count = fontData->intervalCount;
  if (count == 0) return nullptr;

  int left = 0;
  int right = count - 1;
  while (left <= right) {
    const int mid = left + (right - left) / 2;
    const EpdUnicodeInterval* interval = &intervals[mid];
    if (cp < interval->first) {
      right = mid - 1;
    } else if (cp > interval->last) {
      left = mid + 1;
    } else if (fontData->glyph) {
      return &fontData->glyph[interval->offset + (cp - interval->first)];
    } else {
      return nullptr;
    }
  }
  return nullptr;
}

int EpdFont::glyphAdvanceX(const uint32_t cp, const EpdFontStyles::Style style) const {
  const EpdGlyph* glyph = getGlyph(cp, style);
  if (!glyph) glyph = getGlyph('?', style);
  return glyph ? glyph->advanceX : 0;
}

const uint8_t* EpdFont::loadGlyphBitmap(const EpdGlyph* glyph, uint8_t* buffer,
                                        const EpdFontStyles::Style style) const {
  (void)buffer;
  const EpdFontData* fontData = getData(style);
  if (!fontData || !fontData->bitmap || !glyph) return nullptr;
  return fontData->bitmap + glyph->dataOffset;
}
