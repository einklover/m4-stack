#pragma once

#include <algorithm>
#include <cstdlib>

// Moon+-style reader side gestures for Murphy M4 frontlight control.
//
// Only the body reader enables this policy. Menus/settings/detail pages disable
// it through Activity::onEnter(), so a vertical swipe there keeps its normal UI
// meaning. The gesture classifier is pure and host-testable; hardware PWM is
// still applied by main.cpp from SETTINGS after the activity frame.
namespace M4ReaderFrontlightGesture {

enum class Target { None, Brightness, Warmth };

struct Decision {
  Target target = Target::None;
  int deltaPercent = 0;
  explicit operator bool() const { return target != Target::None && deltaPercent != 0; }
};

inline bool& enabledFlag() {
  static bool enabled = false;
  return enabled;
}

inline void setEnabled(bool enabled) { enabledFlag() = enabled; }
inline bool enabled() { return enabledFlag(); }

inline int clampPercent(int value) { return std::max(0, std::min(100, value)); }

inline Decision decide(int sx, int sy, int ex, int ey, int width, int height) {
  Decision out;
  if (width <= 0 || height <= 0) return out;

  const int dx = ex - sx;
  const int dy = ey - sy;
  const int absDx = std::abs(dx);
  const int absDy = std::abs(dy);

  // Deliberately require a clearly vertical gesture. This keeps diagonal and
  // horizontal page-turn swipes out of the light controls.
  const int minVertical = std::max(24, height / 25);  // ~4% of screen height
  if (absDy < minVertical || absDy * 3 < absDx * 4) return out;

  // Preserve the reader's top-edge pull-down menu gesture. System back/home
  // edge gestures are filtered by MappedInputManager before calling decide().
  const int topMenuBand = height * 14 / 100;
  if (sy <= topMenuBand && dy > 0) return out;

  // Outer 22% on each side is the control strip. Use the gesture start point;
  // fingers may drift toward the center while sliding without losing control.
  const int sideWidth = std::max(56, width * 22 / 100);
  if (sx < sideWidth) {
    out.target = Target::Brightness;
  } else if (sx >= width - sideWidth) {
    out.target = Target::Warmth;
  } else {
    return Decision{};
  }

  // Full-screen vertical travel ~= 100 percentage points. Up increases,
  // down decreases. Round away from zero enough that a valid swipe always has
  // a visible effect without turning this into fixed 5% stepping.
  int delta = (-dy * 100) / std::max(1, height);
  if (delta == 0) delta = dy < 0 ? 1 : -1;
  out.deltaPercent = std::max(-100, std::min(100, delta));
  return out;
}

}  // namespace M4ReaderFrontlightGesture
