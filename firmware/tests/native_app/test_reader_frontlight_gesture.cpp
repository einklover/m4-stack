#include "util/M4ReaderFrontlightGesture.h"

#include <cassert>
#include <iostream>

int main() {
  using namespace M4ReaderFrontlightGesture;

  setEnabled(false);
  assert(!enabled());
  setEnabled(true);
  assert(enabled());

  // Portrait M4: left side upward swipe raises brightness by travel ratio.
  auto d = decide(50, 600, 55, 200, 480, 800);
  assert(d.target == Target::Brightness);
  assert(d.deltaPercent == 50);

  // Right side downward swipe lowers warmth.
  d = decide(450, 200, 445, 600, 480, 800);
  assert(d.target == Target::Warmth);
  assert(d.deltaPercent == -50);

  // Center vertical swipes remain available to normal reader/UI behavior.
  d = decide(240, 600, 240, 200, 480, 800);
  assert(!d);

  // Horizontal/diagonal page-turn gesture must never adjust the light.
  d = decide(40, 400, 320, 360, 480, 800);
  assert(!d);

  // Preserve the existing top-edge pull-down reader menu gesture.
  d = decide(50, 40, 52, 180, 480, 800);
  assert(!d);

  // Tiny motion is not a light gesture even when it begins in a side strip.
  d = decide(40, 400, 41, 420, 480, 800);
  assert(!d);

  assert(clampPercent(-7) == 0);
  assert(clampPercent(43) == 43);
  assert(clampPercent(107) == 100);

  std::cout << "reader frontlight gesture tests passed\n";
  return 0;
}
