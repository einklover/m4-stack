#include "util/M4RuntimeUiFontPolicy.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

#define CHECK(cond)                                           \
  do {                                                        \
    if (!(cond)) {                                            \
      std::cerr << "runtime_ui_font_policy FAIL: " #cond "\n"; \
      std::exit(1);                                           \
    }                                                         \
  } while (0)

int main() {
  using namespace M4RuntimeUiFontPolicy;

  // Chrome is integer-N native-grid: SMALL 1x (16), UI_10/UI_12 2x (32).
  // Custom Reader fonts must never be instantiated at these sizes onto the
  // system UI IDs. Builtin faces own SMALL/UI_10/UI_12 permanently.
  CHECK(targetPxForFontId(SMALL_FONT_ID) == 16);
  CHECK(targetPxForFontId(UI_10_FONT_ID) == 32);
  CHECK(targetPxForFontId(UI_12_FONT_ID) == 32);

  // Legacy ScaledEpdFont ratio remains only as an early-boot/failure helper.
  // With a 33px Reader configuration, UI_10 fallback targets 32px.
  const float ui10Fallback = scaleForReaderPx(33, UI_10_FONT_ID);
  CHECK(std::fabs(ui10Fallback - (32.0f / 33.0f)) < 0.0001f);

  // Constraint: UI_10 at 2x is 32px, so four CJK (128px) no longer fit a 96px
  // native-app cell. Tight cells must use SMALL (16*4=64) or wrap — do not
  // shrink UI_10 back to 1x.
  const float fixedUi10Px = static_cast<float>(targetPxForFontId(UI_10_FONT_ID));
  const float fixedSmallPx = static_cast<float>(targetPxForFontId(SMALL_FONT_ID));
  CHECK(fixedUi10Px * 4.0f > 96.0f);
  CHECK(fixedSmallPx * 4.0f < 96.0f);

  // Fallback views remain shrink-only and never bitmap-upscale a smaller face.
  CHECK(scaleForReaderPx(18, UI_12_FONT_ID) == 1.0f);

  std::cout << "runtime UI chrome policy tests passed\n";
  return 0;
}
