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

  // Chrome layout default is 中=24; SMALL stays 16. Custom Reader fonts must
  // never be instantiated onto SMALL/UI_10/UI_12.
  CHECK(targetPxForFontId(SMALL_FONT_ID) == 16);
  CHECK(targetPxForFontId(UI_10_FONT_ID) == 24);
  CHECK(targetPxForFontId(UI_12_FONT_ID) == 24);

  const float ui10Fallback = scaleForReaderPx(33, UI_10_FONT_ID);
  CHECK(std::fabs(ui10Fallback - (24.0f / 33.0f)) < 0.0001f);

  const float fixedUi10Px = static_cast<float>(targetPxForFontId(UI_10_FONT_ID));
  const float fixedSmallPx = static_cast<float>(targetPxForFontId(SMALL_FONT_ID));
  CHECK(fixedUi10Px * 4.0f <= 96.0f);
  CHECK(fixedSmallPx * 4.0f < 96.0f);

  // Fallback views remain shrink-only and never bitmap-upscale a smaller face.
  CHECK(scaleForReaderPx(18, UI_12_FONT_ID) == 1.0f);

  std::cout << "runtime UI chrome policy tests passed\n";
  return 0;
}
