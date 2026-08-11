#include "util/M4TouchNavigation.h"

#include <GfxRenderer.h>

#include <atomic>

#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "util/M4UiText.h"

namespace M4TouchNavigation {
namespace {
std::atomic<uint8_t> gMode{static_cast<uint8_t>(Mode::None)};
std::atomic<bool> gHeaderBackVisible{false};

void drawBackIcon(const GfxRenderer& renderer, int cx, int cy) {
  // GfxRenderer::drawLine intentionally supports only horizontal/vertical
  // lines. Draw the chevron explicitly so this stays cheap and deterministic.
  for (int i = 0; i <= 9; ++i) {
    renderer.drawPixel(cx - i, cy - i, true);
    renderer.drawPixel(cx - i, cy - i + 1, true);
    renderer.drawPixel(cx - i, cy + i, true);
    renderer.drawPixel(cx - i, cy + i + 1, true);
  }
  renderer.fillRect(cx - 8, cy, 23, 2, true);
}

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

void drawHomeIcon(const GfxRenderer& renderer, int cx, int cy) {
  // Roof (two 45-degree strokes) + rectangular body. No font glyph is needed
  // for the icon, so navigation remains recognizable during font failures.
  for (int i = 0; i <= 9; ++i) {
    renderer.drawPixel(cx - i, cy - 2 + i, true);
    renderer.drawPixel(cx + i, cy - 2 + i, true);
    renderer.drawPixel(cx - i, cy - 1 + i, true);
    renderer.drawPixel(cx + i, cy - 1 + i, true);
  }
  renderer.drawRect(cx - 8, cy + 7, 17, 13, true);
  renderer.fillRect(cx - 2, cy + 13, 5, 7, true);
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

bool hitBack(int x, int y, int screenWidth, int screenHeight) {
  const Mode m = mode();
  if (m == Mode::HeaderBack) return inHeaderBack(x, y);
  if (m == Mode::BottomBackHome) {
    const int top = screenHeight - kBottomBarHeight;
    const bool bottomBack = y >= top && y < screenHeight && x >= 0 && x < screenWidth / 2;
    return bottomBack || inHeaderBack(x, y);
  }
  return false;
}

bool hitHome(int x, int y, int screenWidth, int screenHeight) {
  if (mode() != Mode::BottomBackHome) return false;
  const int top = screenHeight - kBottomBarHeight;
  return y >= top && y < screenHeight && x >= screenWidth / 2 && x < screenWidth;
}

void drawHeaderBack(const GfxRenderer& renderer, const Rect& headerRect) {
#if defined(CROSSPOINT_MURPHY_M4)
  if (!enabled() || headerRect.width <= 0 || headerRect.height <= 0) return;
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
  const int top = h - kBottomBarHeight;
  if (top < 0) return;

  // Replace legacy hardware-only hints with a high-contrast, stable touch bar.
  // The standard layouts already reserve roughly this much footer space.
  renderer.fillRect(0, top, w, kBottomBarHeight, false);
  renderer.drawLine(0, top, w - 1, top, true);
  renderer.drawLine(w / 2, top + 7, w / 2, h - 7, true);

  const int iconY = top + 18;
  drawBackIcon(renderer, w / 4 - 30, iconY);
  drawHomeIcon(renderer, 3 * w / 4 - 30, iconY - 7);

  M4UiText::draw(renderer, UI_10_FONT_ID, w / 4 - 8, top + 12, "返回", true);
  M4UiText::draw(renderer, UI_10_FONT_ID, 3 * w / 4 - 8, top + 12, "主页", true);
#else
  (void)renderer;
#endif
}

}  // namespace M4TouchNavigation
