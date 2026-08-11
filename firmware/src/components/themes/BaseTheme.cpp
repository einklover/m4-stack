#include "BaseTheme.h"

#include <GfxRenderer.h>

#include <cstdint>
#include <string>

#include "fontIds.h"
#include "util/M4UiText.h"

// All UI drawing methods are fully overridden by LyraTheme.
// Only the following three methods remain here as default implementations
// used via virtual dispatch from LyraTheme (which does not override them).

void BaseTheme::drawProgressBar(const GfxRenderer& renderer, Rect rect, const size_t current,
                                const size_t total) const {
  if (total == 0) {
    return;
  }

  // Use 64-bit arithmetic to avoid overflow for large files
  const int percent = static_cast<int>((static_cast<uint64_t>(current) * 100) / total);

  // Draw outline
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);

  // Draw filled portion
  const int fillWidth = (rect.width - 4) * percent / 100;
  if (fillWidth > 0) {
    renderer.fillRect(rect.x + 2, rect.y + 2, fillWidth, rect.height - 4);
  }

  // Draw percentage text centered below bar
  const std::string percentText = std::to_string(percent) + "%";
  M4UiText::drawCentered(renderer, UI_10_FONT_ID, rect.y + rect.height + 15, percentText.c_str());
}

void BaseTheme::fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress) const {
  constexpr int barHeight = 4;
  const int barWidth = layout.width - 30;  // twice the margin in drawPopup to match text width
  const int barX = layout.x + (layout.width - barWidth) / 2;
  const int barY = layout.y + layout.height - 10;

  int fillWidth = barWidth * progress / 100;

  renderer.fillRect(barX, barY, fillWidth, barHeight, true);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void BaseTheme::drawReadingProgressBar(const GfxRenderer& renderer, const size_t bookProgress) const {
  int vieweableMarginTop, vieweableMarginRight, vieweableMarginBottom, vieweableMarginLeft;
  renderer.getOrientedViewableTRBL(&vieweableMarginTop, &vieweableMarginRight, &vieweableMarginBottom,
                                   &vieweableMarginLeft);

  const int progressBarMaxWidth = renderer.getScreenWidth() - vieweableMarginLeft - vieweableMarginRight;
  const int progressBarY =
      renderer.getScreenHeight() - vieweableMarginBottom - BaseMetrics::values.bookProgressBarHeight;
  const int barWidth = progressBarMaxWidth * bookProgress / 100;
  renderer.fillRect(vieweableMarginLeft, progressBarY, barWidth, BaseMetrics::values.bookProgressBarHeight, true);
}
