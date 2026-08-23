#include "util/M4RuntimeUiFontPolicy.h"

#include <cassert>
#include <cmath>
#include <iostream>

int main() {
  using namespace M4RuntimeUiFontPolicy;

  // Historical chrome pixel targets remain documented for layout math, but
  // custom Reader fonts must never be instantiated at these sizes onto the
  // system UI IDs. Builtin faces own SMALL/UI_10/UI_12 permanently.
  assert(targetPxForFontId(SMALL_FONT_ID) == 18);
  assert(targetPxForFontId(UI_10_FONT_ID) == 22);
  assert(targetPxForFontId(UI_12_FONT_ID) == 26);

  // Legacy ScaledEpdFont ratio remains only as an early-boot/failure helper.
  // With a 33px Reader configuration, UI_10 fallback targets 22px.
  const float ui10Fallback = scaleForReaderPx(33, UI_10_FONT_ID);
  assert(std::fabs(ui10Fallback - (22.0f / 33.0f)) < 0.0001f);

  // Builtin Native providers reuse the system UI_10 face. Four approximately
  // square CJK advances therefore fit a 96px cell.
  const float fixedUi10Px = static_cast<float>(targetPxForFontId(UI_10_FONT_ID));
  assert(fixedUi10Px * 4.0f < 96.0f);

  // Fallback views remain shrink-only and never bitmap-upscale a smaller face.
  assert(scaleForReaderPx(18, UI_12_FONT_ID) == 1.0f);

  std::cout << "runtime UI chrome policy tests passed\n";
  return 0;
}
