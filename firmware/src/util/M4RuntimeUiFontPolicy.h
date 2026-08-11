#pragma once

#include <algorithm>

#include "../fontIds.h"

// Fixed physical-pixel policy for runtime-TTF chrome.
//
// UI text is deliberately quantized to a tiny set of real raster sizes instead
// of shrinking the Reader bitmap. This keeps small CJK strokes crisp and lets
// system UI + Native plugins reuse exactly the same three faces/cache pools.
//
// The legacy generated epdfont names are not pixel sizes (`m4_ui_cjk_13` is
// 13pt @ 150 DPI), so never derive runtime-TTF chrome metrics from them.
namespace M4RuntimeUiFontPolicy {

constexpr int kSmallBasePx = 18;
constexpr int kUi10BasePx = 22;
constexpr int kUi12BasePx = 26;

inline int targetPxForFontId(int fontId) {
  if (fontId == SMALL_FONT_ID) return kSmallBasePx;
  if (fontId == UI_10_FONT_ID) return kUi10BasePx;
  if (fontId == UI_12_FONT_ID) return kUi12BasePx;
  return kUi12BasePx;
}

// Kept as a fallback for old ScaledEpdFont mappings during early boot. Normal
// runtime-TTF UI rendering now installs native-size fixed faces and therefore
// renders at scale=1.0.
inline float scaleForReaderPx(int readerPx, int fontId) {
  if (readerPx <= 0) return 1.0f;
  float scale = static_cast<float>(targetPxForFontId(fontId)) / static_cast<float>(readerPx);
  if (scale > 1.0f) scale = 1.0f;
  if (scale < 0.25f) scale = 0.25f;
  return scale;
}

inline float effectiveScale(int readerPx, int fontId, int activityScalePercent) {
  const int pct = std::max(60, std::min(125, activityScalePercent));
  return scaleForReaderPx(readerPx, fontId) * static_cast<float>(pct) / 100.0f;
}

}  // namespace M4RuntimeUiFontPolicy
