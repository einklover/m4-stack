#pragma once

// Built-in CrossPoint fallback shutdown wallpaper. SD-card custom wallpapers
// still take precedence in SleepActivity.

#include <GfxRenderer.h>

#include "../fontIds.h"

inline void drawCrosslinkDefaultWallpaper(GfxRenderer& renderer) {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  renderer.clearScreen();

  const int centerX = pageWidth / 2;
  const int circleY = pageHeight / 2 - 140;
  constexpr int kCircleRadius = 33;
  renderer.drawArc(kCircleRadius, centerX, circleY, -1, -1, 1, true);
  renderer.drawArc(kCircleRadius, centerX, circleY, 1, -1, 1, true);
  renderer.drawArc(kCircleRadius, centerX, circleY, 1, 1, 1, true);
  renderer.drawArc(kCircleRadius, centerX, circleY, -1, 1, 1, true);

  renderer.drawCenteredText(UI_10_FONT_ID, circleY + kCircleRadius + 47, "休息中");

  constexpr float kBrandScale = 0.7f;
  constexpr char kBrand[] = "CrossPoint";
  const int brandWidth = renderer.getTextWidth(SMALL_FONT_ID, kBrand, EpdFontFamily::REGULAR, kBrandScale);
  renderer.drawText(SMALL_FONT_ID, (pageWidth - brandWidth) / 2, pageHeight - 82, kBrand, true,
                   EpdFontFamily::REGULAR, kBrandScale);
}
