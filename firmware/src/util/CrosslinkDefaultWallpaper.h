#pragma once

// Built-in CrossLink/CrossPoint fallback wallpaper. Keep boot and shutdown
// paths on the exact same image and placement; SD-card custom wallpapers still
// take precedence in their respective activities.

#include <GfxRenderer.h>

#include "images/bg.h"

inline void drawCrosslinkDefaultWallpaper(GfxRenderer& renderer) {
  const int pageWidth = renderer.getScreenWidth();
  renderer.clearScreen();
  // Asset is stored as the original 800x450 landscape bitmap and displayed
  // centered in the 450x800 portrait frame.
  renderer.drawImage(BgIcon, (pageWidth - 450) / 2, 0, 800, 450);
}
