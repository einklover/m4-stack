#pragma once

// Pure, host-testable UI text face policy (no GfxRenderer / Arduino).
// Runtime drawing lives in M4UiText.h which wraps GfxRenderer using this policy.

#include "fontIds.h"

namespace M4UiText {

struct Face {
  int fontId = UI_12_FONT_ID;       // face used for glyph raster
  float scale = 1.0f;               // shrink reader face to layout metrics (never >1)
  int layoutFontId = UI_12_FONT_ID; // original UI metrics face (geometry)
};

// Mirror GfxRenderer::scaleFontToMatch with pure ascenders (host tests).
inline float scaleToMatchAscenders(int srcAsc, int tgtAsc) {
  if (srcAsc <= 0 || tgtAsc <= 0) return 1.0f;
  float s = static_cast<float>(tgtAsc) / static_cast<float>(srcAsc);
  if (s > 1.0f) s = 1.0f;
  if (s < 0.55f) s = 0.55f;
  if (s > 0.97f) return 1.0f;
  return s;
}

// Pure face pick: prefer reader when available; else layout UI face at 1.0.
// scale is left 1.0 when metrics are unknown (caller fills via GfxRenderer).
inline Face resolveFace(int layoutFontId, int readerFontId, bool readerAvailable) {
  Face f;
  f.layoutFontId = (layoutFontId == 0) ? UI_12_FONT_ID : layoutFontId;
  const bool readerOk = readerAvailable && readerFontId != 0 && readerFontId != -1;
  if (readerOk) {
    f.fontId = readerFontId;
    f.scale = 1.0f;
  } else {
    f.fontId = f.layoutFontId;
    f.scale = 1.0f;
  }
  return f;
}

// Semantic size (Lua gui.drawText 10/12/16) → layout metrics face.
// Large (16) targets body metrics; small/body match UI chrome.
inline int layoutFontForSemantic(int semantic) {
  if (semantic == 10) return UI_10_FONT_ID;
  if (semantic == 16) return NOTOSANS_16_FONT_ID;
  return UI_12_FONT_ID;
}

}  // namespace M4UiText
