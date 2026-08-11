#include "util/M4RuntimeUiFontPolicy.h"

#include <cassert>
#include <cmath>
#include <iostream>

int main() {
  using namespace M4RuntimeUiFontPolicy;

  // Issue #20: runtime TTF chrome is quantized to three real raster sizes
  // shared by system UI and Native plugins. These are not Reader-derived sizes.
  assert(targetPxForFontId(SMALL_FONT_ID) == 18);
  assert(targetPxForFontId(UI_10_FONT_ID) == 22);
  assert(targetPxForFontId(UI_12_FONT_ID) == 26);

  // The old ScaledEpdFont ratio remains only as an early-boot/failure fallback.
  // With the issue #18 Reader configuration (33px), UI_10 fallback targets 22px.
  const float ui10Fallback = scaleForReaderPx(33, UI_10_FONT_ID);
  assert(std::fabs(ui10Fallback - (22.0f / 33.0f)) < 0.0001f);

  // Built-in Native providers now reuse the system UI_10 face directly at
  // 22px. Four approximately square CJK advances therefore fit a 96px cell.
  const float fixedUi10Px = static_cast<float>(targetPxForFontId(UI_10_FONT_ID));
  assert(fixedUi10Px * 4.0f < 96.0f);

  // Fallback views remain shrink-only and never bitmap-upscale a smaller face.
  assert(scaleForReaderPx(18, UI_12_FONT_ID) == 1.0f);

  std::cout << "runtime UI fixed-raster policy tests passed\n";
  return 0;
}
