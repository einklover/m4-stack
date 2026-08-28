#include "util/M4TouchNavigation.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <atomic>

#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "util/HomeRef.h"
#include "util/M4UiText.h"

namespace M4TouchNavigation {
namespace {
std::atomic<uint8_t> gMode{static_cast<uint8_t>(Mode::None)};
std::atomic<bool> gHeaderBackVisible{false};
std::atomic<int> gChapterHeaderX{0};
std::atomic<int> gChapterHeaderY{0};

void drawHeaderBackIcon(const GfxRenderer& renderer, int x, int cy) {
  // Compact chevron only. Fengyan title text starts at ~20 px, so keep all
  // visible pixels to its left while retaining a much larger invisible hitbox.
  for (int i = 0; i <= 7; ++i) {
    renderer.drawPixel(x + i, cy - i, true);
    renderer.drawPixel(x + i, cy - i + 1, true);
    renderer.drawPixel(x + i, cy + i, true);
    renderer.drawPixel(x + i, cy + i + 1, true);
  }
}

bool inHeaderBack(int x, int y) {
  return gHeaderBackVisible.load(std::memory_order_acquire) && x >= 0 && x < kHeaderHitWidth && y >= 0 &&
         y < kHeaderHitHeight;
}
}  // namespace

void setMode(Mode value) {
  gMode.store(static_cast<uint8_t>(value), std::memory_order_release);
  if (value == Mode::None) gHeaderBackVisible.store(false, std::memory_order_release);
}

Mode mode() { return static_cast<Mode>(gMode.load(std::memory_order_acquire)); }

bool enabled() { return mode() != Mode::None; }

void activateForActivity(bool showNavigation) {
  gHeaderBackVisible.store(false, std::memory_order_release);
#if defined(CROSSPOINT_MURPHY_M4)
  setMode(showNavigation ? Mode::HeaderBack : Mode::None);
#else
  (void)showNavigation;
  setMode(Mode::None);
#endif
}

void activateForChapterSelection() {
  gHeaderBackVisible.store(false, std::memory_order_release);
  gChapterHeaderX.store(0, std::memory_order_release);
  gChapterHeaderY.store(0, std::memory_order_release);
#if defined(CROSSPOINT_MURPHY_M4)
  setMode(Mode::ChapterHeaderBack);
#else
  setMode(Mode::None);
#endif
}

bool hitBack(int x, int y, int screenWidth, int screenHeight) {
  const Mode m = mode();
  if (m == Mode::HeaderBack) return inHeaderBack(x, y);
  if (m == Mode::ChapterHeaderBack) {
    return TouchHitGeometry::chapterHeaderBackRect(gChapterHeaderX.load(std::memory_order_acquire),
                                                   gChapterHeaderY.load(std::memory_order_acquire))
        .contains(x, y);
  }
  if (m == Mode::BottomBackHome) {
    const auto layout = TouchHitGeometry::makeBottomNavigationLayout(screenWidth, screenHeight, kBottomBarHeight);
    return layout.back.contains(x, y) || inHeaderBack(x, y);
  }
  return false;
}

bool hitHome(int x, int y, int screenWidth, int screenHeight) {
  if (mode() != Mode::BottomBackHome) return false;
  return TouchHitGeometry::makeBottomNavigationLayout(screenWidth, screenHeight, kBottomBarHeight).home.contains(x, y);
}

void drawHeaderBack(const GfxRenderer& renderer, const Rect& headerRect, const char* /*title*/) {
#if defined(CROSSPOINT_MURPHY_M4)
  if (!enabled() || headerRect.width <= 0 || headerRect.height <= 0) return;
  if (mode() == Mode::ChapterHeaderBack) {
    gChapterHeaderX.store(headerRect.x, std::memory_order_release);
    gChapterHeaderY.store(headerRect.y, std::memory_order_release);
    // Keep the back affordance inside the reserved hitbox. The theme already
    // draws the chapter/book title; painting it here too caused two titles to
    // overlap on the chapter list.
    renderer.fillRect(headerRect.x, headerRect.y, kHeaderHitWidth, headerRect.height, false);
    renderer.drawRect(headerRect.x, headerRect.y, kHeaderHitWidth, headerRect.height, true);
    M4UiText::drawCenteredInBox(renderer, UI_12_FONT_ID, headerRect.x, headerRect.y, kHeaderHitWidth,
                                headerRect.height, "返回", true, EpdFontFamily::BOLD, 8);
    return;
  }
  // Visible icon stays inside the theme's existing left padding; hit area is
  // 56x56, so the control remains easy to tap without changing list geometry.
  const int cy = headerRect.y + headerRect.height / 2 - 2;
  drawHeaderBackIcon(renderer, headerRect.x + 4, cy);
  gHeaderBackVisible.store(true, std::memory_order_release);
#else
  (void)renderer;
  (void)headerRect;
#endif
}

void drawBottomBar(GfxRenderer& renderer) {
#if defined(CROSSPOINT_MURPHY_M4)
  if (!enabled()) return;
  setMode(Mode::BottomBackHome);

  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight();
  const auto layout = TouchHitGeometry::makeBottomNavigationLayout(w, h, kBottomBarHeight);
  if (!layout.valid()) return;
  const int top = layout.back.y;

  // Replace legacy hardware-only hints with a high-contrast, stable touch bar.
  // The standard layouts already reserve roughly this much footer space.
  renderer.fillRect(0, top, w, kBottomBarHeight, false);
  renderer.drawLine(0, top, w - 1, top, 1, true);
  renderer.drawLine(layout.home.x, top, layout.home.x, h - 1, 1, true);
  renderer.drawLine(layout.menu.x, top, layout.menu.x, h - 1, 1, true);

  auto drawNavItem = [&](const TouchHitGeometry::Rect& zone, const char* label, int kind) {
    const int icon = HomeRef::BottomIconSize;
    const int textW = M4UiText::systemTextWidth(renderer, UI_10_FONT_ID, label, EpdFontFamily::BOLD);
    const int gap = 8;
    const int group = icon + gap + textW;
    const int gx = zone.x + std::max(0, (zone.width - group) / 2);
    const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
    const int textY = HomeRef::BottomBaseline - lineH;
    const int textCy = textY + lineH / 2;
    const int iy = textCy - icon / 2;
    const int cx = gx + icon / 2;
    const int cy = iy + icon / 2;
    if (kind == 0) {
      const int tipX = gx + 6;
      for (int i = 0; i <= 6; ++i) {
        renderer.drawPixel(tipX + i, cy - i, true);
        renderer.drawPixel(tipX + i, cy + i, true);
      }
    } else if (kind == 1) {
      const int peakY = std::max(zone.y + 2, iy + 1);
      for (int i = 0; i <= 8; ++i) {
        renderer.drawPixel(cx - i, peakY + i, true);
        renderer.drawPixel(cx + i, peakY + i, true);
      }
      const int eaveY = peakY + 8;
      renderer.drawLine(cx - 8, eaveY, cx + 8, eaveY, true);
      renderer.drawRect(cx - 6, eaveY, 13, 8, true);
    } else {
      const int lx = gx + 4;
      const int rx = gx + icon - 5;
      for (int row = 0; row < 3; ++row) {
        renderer.drawLine(lx, iy + 5 + row * 5, rx, iy + 5 + row * 5, true);
      }
    }
    M4UiText::drawSystem(renderer, UI_10_FONT_ID, gx + icon + gap, textY, label, true, EpdFontFamily::BOLD);
  };

  drawNavItem(layout.back, "返回", 0);
  drawNavItem(layout.home, "主页", 1);
  drawNavItem(layout.menu, "菜单", 2);
#else
  (void)renderer;
#endif
}

}  // namespace M4TouchNavigation
