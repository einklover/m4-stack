#include "MappedInputManager.h"

#include <Arduino.h>
#include <GfxRenderer.h>

#include <algorithm>
#include <cstdlib>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "util/M4FooterTouchPolicy.h"
#include "util/M4ReaderFrontlightGesture.h"
#include "util/M4TouchNavigation.h"
#include "util/TouchHitGeometry.h"

namespace {
using ButtonIndex = uint8_t;

struct SideLayoutMap {
  ButtonIndex pageBack;
  ButtonIndex pageForward;
};

// Order matches CrossPointSettings::SIDE_BUTTON_LAYOUT.
constexpr SideLayoutMap kSideLayouts[] = {
    {HalGPIO::BTN_UP, HalGPIO::BTN_DOWN},
    {HalGPIO::BTN_DOWN, HalGPIO::BTN_UP},
};

constexpr float TOP_EDGE_MENU_GESTURE_FRAC_Y = 0.14f;
constexpr unsigned long TOUCH_DOWN_SELECT_DELAY_MS = 90;
}  // namespace

MappedInputManager::MappedInputManager(HalGPIO& gpioRef) : gpio(gpioRef), renderer(nullptr) {}

void MappedInputManager::beginFrame() {
  tapCacheValid = false;
  tapCacheHas = false;
  swipeCacheValid = false;
  swipeCacheHas = false;
  syntheticBack = false;
  touchHeldOverrideValid = false;
#if defined(CROSSPOINT_MURPHY_M4)
  // Consume one-frame synthetic events exactly once per beginFrame().
  synthKind_ = SynthKind::None;

  // Moon+-style reader side controls. Decode once into the normal swipe cache so
  // the reader can still inspect the same event later. Only true body-reader
  // activities enable the policy; menus/details/settings disable it on enter.
  if (M4ReaderFrontlightGesture::enabled() && hasTouch()) {
    int sx = 0, sy = 0, ex = 0, ey = 0;
    if (decodeSwipe(sx, sy, ex, ey)) {
      const int w = renderer->getScreenWidth();
      const int h = renderer->getScreenHeight();
      const bool systemGesture = TouchHitGeometry::isSystemBackSwipe(sx, sy, ex, ey, w, h) ||
                                 TouchHitGeometry::isSystemHomeSwipe(sx, sy, ex, ey, w, h);
      if (!systemGesture) {
        const auto decision = M4ReaderFrontlightGesture::decide(sx, sy, ex, ey, w, h);
        if (decision) {
          uint8_t* value = decision.target == M4ReaderFrontlightGesture::Target::Brightness
                               ? &SETTINGS.frontlightBrightness
                               : &SETTINGS.frontlightWarmth;
          const int next = M4ReaderFrontlightGesture::clampPercent(static_cast<int>(*value) + decision.deltaPercent);
          if (next != static_cast<int>(*value)) {
            *value = static_cast<uint8_t>(next);
            // The main loop applies SETTINGS to FrontlightManager after the
            // activity frame, so changing light does not redraw the EPD body.
            SETTINGS.saveToFile();
            Serial.printf("[%lu] [M4-LIGHT] Reader side swipe %s=%u%% delta=%d\n", millis(),
                          decision.target == M4ReaderFrontlightGesture::Target::Brightness ? "brightness" : "warmth",
                          static_cast<unsigned>(*value), decision.deltaPercent);
          }
        }
      }
    }
  }
#endif
}

void MappedInputManager::pulseSyntheticBack() { syntheticBack = true; }

#if defined(CROSSPOINT_MURPHY_M4)
bool MappedInputManager::hasPendingSyntheticInput() const { return synthKind_ != SynthKind::None; }

bool MappedInputManager::injectSyntheticTap(int x, int y, bool& busy) {
  busy = false;
  if (!renderer) {
    return false;
  }
  const int w = renderer->getScreenWidth();
  const int h = renderer->getScreenHeight();
  if (x < 0 || y < 0 || x >= w || y >= h) {
    return false;
  }
  const unsigned long now = millis();
  // Do not treat synthLastInjectMs_==0 as a recent inject (first command after boot).
  const bool rateBusy = synthEverInjected_ && (now - synthLastInjectMs_) < kSynthMinIntervalMs;
  if (synthKind_ != SynthKind::None || rateBusy) {
    busy = true;
    return false;
  }
  synthKind_ = SynthKind::Tap;
  synthTapX_ = x;
  synthTapY_ = y;
  synthLastInjectMs_ = now;
  synthEverInjected_ = true;
  return true;
}

bool MappedInputManager::injectSyntheticKey(Button button, bool& busy) {
  busy = false;
  const unsigned long now = millis();
  const bool rateBusy = synthEverInjected_ && (now - synthLastInjectMs_) < kSynthMinIntervalMs;
  if (synthKind_ != SynthKind::None || rateBusy) {
    busy = true;
    return false;
  }
  synthKind_ = SynthKind::Key;
  synthKey_ = button;
  synthLastInjectMs_ = now;
  synthEverInjected_ = true;
  return true;
}
#endif

bool MappedInputManager::mapButton(const Button button, bool (HalGPIO::*fn)(uint8_t) const) const {
  const auto sideLayout = static_cast<CrossPointSettings::SIDE_BUTTON_LAYOUT>(SETTINGS.sideButtonLayout);
  const auto& side = kSideLayouts[sideLayout];

  switch (button) {
    case Button::Back:
      return (gpio.*fn)(SETTINGS.frontButtonBack);
    case Button::Confirm:
      return (gpio.*fn)(SETTINGS.frontButtonConfirm);
    case Button::Left:
      return (gpio.*fn)(SETTINGS.frontButtonLeft);
    case Button::Right:
      return (gpio.*fn)(SETTINGS.frontButtonRight);
    case Button::Up:
      return (gpio.*fn)(HalGPIO::BTN_UP);
    case Button::Down:
      return (gpio.*fn)(HalGPIO::BTN_DOWN);
    case Button::Power:
      return (gpio.*fn)(HalGPIO::BTN_POWER);
    case Button::PageBack:
      return (gpio.*fn)(side.pageBack);
    case Button::PageForward:
      return (gpio.*fn)(side.pageForward);
  }

  return false;
}

bool MappedInputManager::wasPressed(const Button button) const {
  if (button == Button::Back && syntheticBack) return true;
#if defined(CROSSPOINT_MURPHY_M4)
  if (synthKind_ == SynthKind::Key && synthKey_ == button) return true;
#endif
  return mapButton(button, &HalGPIO::wasPressed);
}

bool MappedInputManager::wasReleased(const Button button) const {
  // Pulse both pressed+released so activities using either edge work in one frame.
  if (button == Button::Back && syntheticBack) return true;
#if defined(CROSSPOINT_MURPHY_M4)
  if (synthKind_ == SynthKind::Key && synthKey_ == button) return true;

  // Fullscreen provider/login pages draw an activity-owned four-slot footer.
  // When that activity opts in, convert a tap on the painted physical slot back
  // into the same logical button event used by hardware keys. This keeps button
  // remapping correct and avoids per-screen duplicate hit geometry.
  M4FooterTouchPolicy::LogicalButton logical = M4FooterTouchPolicy::Back;
  uint8_t mappedHw = 0xFF;
  bool eligible = true;
  switch (button) {
    case Button::Back:
      logical = M4FooterTouchPolicy::Back;
      mappedHw = SETTINGS.frontButtonBack;
      break;
    case Button::Confirm:
      logical = M4FooterTouchPolicy::Confirm;
      mappedHw = SETTINGS.frontButtonConfirm;
      break;
    case Button::Left:
      logical = M4FooterTouchPolicy::Left;
      mappedHw = SETTINGS.frontButtonLeft;
      break;
    case Button::Right:
      logical = M4FooterTouchPolicy::Right;
      mappedHw = SETTINGS.frontButtonRight;
      break;
    default:
      eligible = false;
      break;
  }
  if (eligible && M4FooterTouchPolicy::enabled(logical) && renderer) {
    int tx = 0;
    int ty = 0;
    if (wasScreenTapped(tx, ty)) {
      const auto& metrics = UITheme::getInstance().getMetrics();
      const int w = renderer->getScreenWidth();
      const int h = renderer->getScreenHeight();
      if (w > 0 && ty >= h - metrics.buttonHintsHeight && ty < h) {
        const int slot = std::min(3, std::max(0, tx * 4 / w));
        const uint8_t physical[4] = {HalGPIO::BTN_BACK, HalGPIO::BTN_CONFIRM,
                                     HalGPIO::BTN_LEFT, HalGPIO::BTN_RIGHT};
        if (physical[slot] == mappedHw) return true;
      }
    }
  }
#endif
  return mapButton(button, &HalGPIO::wasReleased);
}

bool MappedInputManager::isPressed(const Button button) const { return mapButton(button, &HalGPIO::isPressed); }

bool MappedInputManager::wasAnyPressed() const {
  return gpio.wasAnyPressed() || gpio.wasTouchActivity();
}

bool MappedInputManager::wasAnyReleased() const { return gpio.wasAnyReleased(); }

unsigned long MappedInputManager::getHeldTime() const { return gpio.getHeldTime(); }

MappedInputManager::Labels MappedInputManager::mapLabels(const char* back, const char* confirm, const char* previous,
                                                         const char* next) const {
  auto labelForHardware = [&](uint8_t hw) -> const char* {
    if (hw == SETTINGS.frontButtonBack) return back;
    if (hw == SETTINGS.frontButtonConfirm) return confirm;
    if (hw == SETTINGS.frontButtonLeft) return previous;
    if (hw == SETTINGS.frontButtonRight) return next;
    return "";
  };

  return {labelForHardware(HalGPIO::BTN_BACK), labelForHardware(HalGPIO::BTN_CONFIRM),
          labelForHardware(HalGPIO::BTN_LEFT), labelForHardware(HalGPIO::BTN_RIGHT)};
}

int MappedInputManager::getPressedFrontButton() const {
  if (gpio.wasPressed(HalGPIO::BTN_BACK)) return HalGPIO::BTN_BACK;
  if (gpio.wasPressed(HalGPIO::BTN_CONFIRM)) return HalGPIO::BTN_CONFIRM;
  if (gpio.wasPressed(HalGPIO::BTN_LEFT)) return HalGPIO::BTN_LEFT;
  if (gpio.wasPressed(HalGPIO::BTN_RIGHT)) return HalGPIO::BTN_RIGHT;
  return -1;
}

bool MappedInputManager::hasTouch() const {
#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
  // Plugin-debug QEMU skips FT6x36 I2C polling. Activities gate on hasTouch()
  // before wasScreenTapped(); keep true so m4adb synthetic taps still reach UI.
  return renderer != nullptr;
#else
  return gpio.hasTouch() && renderer != nullptr;
#endif
}

bool MappedInputManager::wasScreenTapped(int& x, int& y) const {
  if (!tapCacheValid) {
    tapCacheValid = true;
    tapCacheHas = false;
#if defined(CROSSPOINT_MURPHY_M4)
    if (synthKind_ == SynthKind::Tap) {
      tapX = synthTapX_;
      tapY = synthTapY_;
      tapCacheHas = true;
    } else
#endif
    if (hasTouch()) {
      float nx = 0.0f;
      float ny = 0.0f;
      if (gpio.wasTouchTap(nx, ny)) {
        renderer->tapToLogical(nx, ny, tapX, tapY);
        tapCacheHas = true;
        rememberTouchHeldTime();
      }
    }
  }
  if (!tapCacheHas) return false;
  x = tapX;
  y = tapY;
  return true;
}

bool MappedInputManager::wasScreenTouchDown(int& x, int& y) const {
  if (!hasTouch()) return false;
  float nx = 0.0f;
  float ny = 0.0f;
  unsigned long heldMs = 0;
  if (!gpio.isTouchTapCandidate(nx, ny, heldMs)) return false;
  if (heldMs < TOUCH_DOWN_SELECT_DELAY_MS) return false;
  renderer->tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::isScreenTouchHeld(int& x, int& y) const {
  if (!hasTouch()) return false;
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.isTouchHeldAt(nx, ny)) return false;
  renderer->tapToLogical(nx, ny, x, y);
  return true;
}

unsigned long MappedInputManager::lastScreenTouchHeldMs() const {
  if (!hasTouch()) return 0;
  if (touchHeldOverrideValid && millis() - touchHeldOverrideAt <= 250) return touchHeldOverrideMs;
  touchHeldOverrideValid = false;
  return gpio.lastTouchHeldMs();
}

void MappedInputManager::rememberTouchHeldTime() const {
  touchHeldOverrideValid = true;
  touchHeldOverrideMs = gpio.lastTouchHeldMs();
  touchHeldOverrideAt = millis();
}

bool MappedInputManager::wasTapInRect(const int x, const int y, const int width, const int height) const {
  int tx = 0;
  int ty = 0;
  return wasScreenTapped(tx, ty) && tx >= x && tx < x + width && ty >= y && ty < y + height;
}

bool MappedInputManager::listItemFromPoint(const int x, const int y, int& index, const int itemCount,
                                           const int selectedIndex, const int listTop, const int listHeight,
                                           const bool hasSubtitle) const {
  (void)x;
  if (itemCount <= 0 || !renderer) return false;
  if (y < listTop || y >= listTop + listHeight) return false;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int rowStep = hasSubtitle ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight;
  if (rowStep <= 0) return false;

  const int pageItems = std::max(1, listHeight / rowStep);
  const int pageStart = std::max(0, selectedIndex / pageItems) * pageItems;
  const int row = (y - listTop) / rowStep;
  const int tapped = pageStart + row;
  if (row < 0 || row >= pageItems || tapped >= itemCount) return false;
  index = tapped;
  return true;
}

bool MappedInputManager::wasListItemTapped(int& index, const int itemCount, const int selectedIndex, const int listTop,
                                           const int listHeight, const bool hasSubtitle) const {
  int tx = 0;
  int ty = 0;
  return wasScreenTapped(tx, ty) &&
         listItemFromPoint(tx, ty, index, itemCount, selectedIndex, listTop, listHeight, hasSubtitle);
}

bool MappedInputManager::wasListItemTouchedDown(int& index, const int itemCount, const int selectedIndex,
                                                const int listTop, const int listHeight, const bool hasSubtitle) const {
  int tx = 0;
  int ty = 0;
  return wasScreenTouchDown(tx, ty) &&
         listItemFromPoint(tx, ty, index, itemCount, selectedIndex, listTop, listHeight, hasSubtitle);
}

MappedInputManager::RowTouch MappedInputManager::rowTouch(int& row, const int top, const int rowStep,
                                                          const int rowCount, const int xStart, const int xEnd,
                                                          const int rowHeight) const {
  if (rowStep <= 0 || rowCount <= 0) return RowTouch::None;
  const auto hit = [&](const int x, const int y) {
    if (x < xStart || x >= xEnd || y < top) return false;
    const int r = (y - top) / rowStep;
    if (r >= rowCount) return false;
    if (rowHeight > 0 && (y - top) % rowStep >= rowHeight) return false;
    row = r;
    return true;
  };
  int x = 0;
  int y = 0;
  if (wasScreenTouchDown(x, y) && hit(x, y)) return RowTouch::Down;
  if (wasScreenTapped(x, y) && hit(x, y)) return RowTouch::Tap;
  return RowTouch::None;
}

bool MappedInputManager::decodeSwipe(int& sx, int& sy, int& ex, int& ey) const {
  if (!hasTouch()) return false;
  if (!swipeCacheValid) {
    swipeCacheValid = true;
    float nxs = 0.0f, nys = 0.0f, nxe = 0.0f, nye = 0.0f;
    swipeCacheHas = gpio.wasSwipe(nxs, nys, nxe, nye);
    if (swipeCacheHas) {
      renderer->tapToLogical(nxs, nys, swipeSx, swipeSy);
      renderer->tapToLogical(nxe, nye, swipeEx, swipeEy);
    }
  }
  if (!swipeCacheHas) return false;
  sx = swipeSx;
  sy = swipeSy;
  ex = swipeEx;
  ey = swipeEy;
  return true;
}

MappedInputManager::SwipeDir MappedInputManager::wasSwipe() const {
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return SwipeDir::None;
  const int w = renderer->getScreenWidth();
  const int h = renderer->getScreenHeight();
  if (TouchHitGeometry::isSystemBackSwipe(sx, sy, ex, ey, w, h)) return SwipeDir::None;
  if (TouchHitGeometry::isSystemHomeSwipe(sx, sy, ex, ey, w, h)) return SwipeDir::None;
  const int dx = ex - sx;
  const int dy = ey - sy;
  if (std::abs(dx) >= std::abs(dy)) return dx < 0 ? SwipeDir::Left : SwipeDir::Right;
  return dy < 0 ? SwipeDir::Up : SwipeDir::Down;
}

bool MappedInputManager::wasBackGesture() const {
  if (!hasTouch()) return false;

#if defined(CROSSPOINT_MURPHY_M4)
  if (M4TouchNavigation::enabled()) {
    int tx = 0, ty = 0;
    if (wasScreenTapped(tx, ty) &&
        M4TouchNavigation::hitBack(tx, ty, renderer->getScreenWidth(), renderer->getScreenHeight())) {
      rememberTouchHeldTime();
      return true;
    }
  }
#endif

  int sx = 0, sy = 0, ex = 0, ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return false;
  const bool hit = TouchHitGeometry::isSystemBackSwipe(sx, sy, ex, ey, renderer->getScreenWidth(),
                                                       renderer->getScreenHeight());
  if (hit) rememberTouchHeldTime();
  return hit;
}

bool MappedInputManager::wasBackSwipeGesture() const {
  if (!hasTouch()) return false;
  int sx = 0, sy = 0, ex = 0, ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return false;
  return TouchHitGeometry::isSystemBackSwipe(sx, sy, ex, ey, renderer->getScreenWidth(),
                                             renderer->getScreenHeight());
}

int MappedInputManager::backSwipeAnimationDirection() const {
  if (!hasTouch()) return -1;
  int sx = 0, sy = 0, ex = 0, ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey) ||
      !TouchHitGeometry::isSystemBackSwipe(sx, sy, ex, ey, renderer->getScreenWidth(),
                                           renderer->getScreenHeight())) {
    return -1;
  }
  const int dx = ex - sx;
  return dx > 0 ? 1 : 0;
}

bool MappedInputManager::wasHomeGesture() const {
  if (!hasTouch()) return false;

#if defined(CROSSPOINT_MURPHY_M4)
  if (M4TouchNavigation::mode() == M4TouchNavigation::Mode::BottomBackHome) {
    int tx = 0, ty = 0;
    if (wasScreenTapped(tx, ty) &&
        M4TouchNavigation::hitHome(tx, ty, renderer->getScreenWidth(), renderer->getScreenHeight())) {
      rememberTouchHeldTime();
      return true;
    }
  }
#endif

  int sx = 0, sy = 0, ex = 0, ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return false;
  const bool hit = TouchHitGeometry::isSystemHomeSwipe(sx, sy, ex, ey, renderer->getScreenWidth(),
                                                       renderer->getScreenHeight());
  if (hit) rememberTouchHeldTime();
  return hit;
}

bool MappedInputManager::wasHomeSwipeGesture() const {
  if (!hasTouch()) return false;
  int sx = 0, sy = 0, ex = 0, ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return false;
  return TouchHitGeometry::isSystemHomeSwipe(sx, sy, ex, ey, renderer->getScreenWidth(),
                                              renderer->getScreenHeight());
}

bool MappedInputManager::wasMenuGesture() const {
  if (!hasTouch()) return false;
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return false;
  const int w = renderer->getScreenWidth();
  const int h = renderer->getScreenHeight();
  if (TouchHitGeometry::isSystemBackSwipe(sx, sy, ex, ey, w, h)) return false;
  if (TouchHitGeometry::isSystemHomeSwipe(sx, sy, ex, ey, w, h)) return false;
  return sy <= renderer->getScreenHeight() * TOP_EDGE_MENU_GESTURE_FRAC_Y && ey > sy &&
         std::abs(ey - sy) > std::abs(ex - sx);
}

bool MappedInputManager::wasTouchActivity() const { return gpio.wasTouchActivity(); }
