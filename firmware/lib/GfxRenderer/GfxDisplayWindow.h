#pragma once

// Logical UI rect -> panel-native AABB for SSD1677 displayWindow.
// Panel x/w must be 8 px aligned (byte columns). y/h are not snapped.

struct GfxPanelRect { int x = 0; int y = 0; int w = 0; int h = 0; bool valid = false; };
constexpr int kGfxOrientPortrait = 0;
constexpr int kGfxOrientLandscapeCW = 1;
constexpr int kGfxOrientPortraitInverted = 2;
constexpr int kGfxOrientLandscapeCCW = 3;

inline void gfxRotateLogicalToPanel(int orientation, int x, int y, int panelW, int panelH,
                                    int* phyX, int* phyY) {
  switch (orientation) {
    case kGfxOrientPortrait: *phyX = y; *phyY = panelH - 1 - x; break;
    case kGfxOrientLandscapeCW: *phyX = panelW - 1 - x; *phyY = panelH - 1 - y; break;
    case kGfxOrientPortraitInverted: *phyX = panelW - 1 - y; *phyY = x; break;
    case kGfxOrientLandscapeCCW: default: *phyX = x; *phyY = y; break;
  }
}

inline GfxPanelRect gfxLogicalWindowToPanel(int orientation, int lx, int ly, int lw, int lh,
                                             int panelW, int panelH) {
  GfxPanelRect out{};
  if (lw <= 0 || lh <= 0 || panelW <= 0 || panelH <= 0) return out;
  const int cx[4] = {lx, lx + lw - 1, lx, lx + lw - 1};
  const int cy[4] = {ly, ly, ly + lh - 1, ly + lh - 1};
  int minX = 0, maxX = 0, minY = 0, maxY = 0;
  for (int i = 0; i < 4; ++i) {
    int px = 0, py = 0;
    gfxRotateLogicalToPanel(orientation, cx[i], cy[i], panelW, panelH, &px, &py);
    if (i == 0) minX = maxX = px, minY = maxY = py;
    else { if (px < minX) minX = px; if (px > maxX) maxX = px;
           if (py < minY) minY = py; if (py > maxY) maxY = py; }
  }
  if (minX < 0) minX = 0; if (minY < 0) minY = 0;
  if (maxX >= panelW) maxX = panelW - 1; if (maxY >= panelH) maxY = panelH - 1;
  if (minX > maxX || minY > maxY) return out;
  int x = minX, y = minY, w = maxX - minX + 1, h = maxY - minY + 1;
  int left = x & ~7, right = (x + w + 7) & ~7;
  if (left < 0) left = 0; if (right > panelW) right = panelW; right &= ~7;
  if (right <= left) return out;
  out = {left, y, right - left, h, true};
  return out;
}
