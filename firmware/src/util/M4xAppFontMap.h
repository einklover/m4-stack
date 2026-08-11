#pragma once

// Map M4x plugin semantic font sizes (10 / 12 / 16) onto system content faces.
// Plugins must not ship or gate on their own font files; the host supplies
// glyphs via GfxRenderer + EpdFontLoader, using SETTINGS.getReaderFontId()
// as the primary content face (with safe builtin NOTOSANS fallback).

#include "fontIds.h"

namespace M4xAppFontMap {

// Semantic sizes used by Lua gui.drawText / textWidth / lineHeight.
constexpr int kSemanticSmall = 10;
constexpr int kSemanticBody = 12;
constexpr int kSemanticLarge = 16;

// Default when SETTINGS / renderer path is unavailable (host unit tests, early boot).
constexpr int kSafeFallbackFontId = NOTOSANS_16_FONT_ID;

// Pure mapping: readerFontId is SETTINGS.getReaderFontId() (or a test stand-in).
// - 10  → slightly smaller content face (NOTOSANS_12) when reader is the default
//         NOTOSANS ladder; otherwise follow readerFontId so custom CJK still works.
// - 12/16/0 → readerFontId
// - other non-zero → pass through (advanced plugins / real renderer IDs)
// - invalid readerFontId (0 / -1) → kSafeFallbackFontId
inline int mapSemanticToSystem(int semantic, int readerFontId) {
  const int reader =
      (readerFontId == 0 || readerFontId == -1) ? kSafeFallbackFontId : readerFontId;

  if (semantic == kSemanticSmall) {
    // Labels / status: prefer compact content face when reader is a standard
    // NOTOSANS size; custom / unknown reader IDs keep full CJK coverage.
    if (reader == NOTOSANS_12_FONT_ID || reader == NOTOSANS_14_FONT_ID ||
        reader == NOTOSANS_16_FONT_ID || reader == NOTOSANS_18_FONT_ID) {
      return NOTOSANS_12_FONT_ID;
    }
    return reader;
  }
  if (semantic == kSemanticBody || semantic == kSemanticLarge || semantic == 0) {
    return reader;
  }
  // Pass-through real IDs (custom hash IDs, explicit NOTOSANS_*, etc.).
  return semantic;
}

}  // namespace M4xAppFontMap
