#include "EpubReaderPercentSelectionActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>

#include <algorithm>
#include <string>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4ReaderMenuLayout.h"
#include "util/M4UiText.h"

namespace {
constexpr int kSmallStep = 1;
constexpr int kLargeStep = 10;
constexpr int kStepDeltas[M4ReaderMenuLayout::kProgressStepCount] = {-10, -1, 1, 10};
constexpr const char* kStepLabels[M4ReaderMenuLayout::kProgressStepCount] = {"-10%", "-1%", "+1%", "+10%"};
}  // namespace

void EpubReaderPercentSelectionActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();
  firstPaint = true;
  updateRequired = true;
  xTaskCreate(&EpubReaderPercentSelectionActivity::taskTrampoline, "EpubPercentSlider", 4096, this, 1,
              &displayTaskHandle);
}

void EpubReaderPercentSelectionActivity::onExit() {
  ActivityWithSubactivity::onExit();
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void EpubReaderPercentSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<EpubReaderPercentSelectionActivity*>(param);
  self->displayTaskLoop();
}

void EpubReaderPercentSelectionActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired && !subActivity) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      renderScreen();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void EpubReaderPercentSelectionActivity::adjustPercent(const int delta) {
  const int next = std::max(0, std::min(100, percent + delta));
  if (next == percent) return;
  percent = next;
  updateRequired = true;
}

void EpubReaderPercentSelectionActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) {
    onCancel();
    return;
  }

  // Touch intentionally reacts only to completed taps. There is no drag-to-seek,
  // which prevents a stream of e-ink refreshes while a finger moves on the panel.
  if (mappedInput.hasTouch()) {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty)) {
      const auto metrics = UITheme::getInstance().getMetrics();
      const int pageWidth = renderer.getScreenWidth();
      const auto orientation = renderer.getOrientation();
      const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
      const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
      const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
      const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
      const int contentX = isLandscapeCw ? hintGutterWidth : 0;
      const int contentWidth = pageWidth - hintGutterWidth;
      const int hintGutterHeight = isPortraitInverted ? 50 : 0;
      const int contentTop = hintGutterHeight + metrics.headerHeight + metrics.verticalSpacing;
      const auto layout = M4ReaderMenuLayout::makeProgressPanelLayout(contentX, contentWidth, contentTop);

      const int step = layout.stepFromPoint(tx, ty);
      if (step >= 0) {
        adjustPercent(kStepDeltas[step]);
        return;
      }

      const int tappedPercent = layout.percentFromPoint(tx, ty);
      if (tappedPercent >= 0 && tappedPercent != percent) {
        percent = tappedPercent;
        updateRequired = true;
      }
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    onSelect(percent);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    adjustPercent(-kSmallStep);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    adjustPercent(kSmallStep);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    adjustPercent(kLargeStep);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    adjustPercent(-kLargeStep);
    return;
  }
}

void EpubReaderPercentSelectionActivity::renderScreen() {
  renderer.clearScreen();

  const auto metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int contentTop = hintGutterHeight + metrics.headerHeight + metrics.verticalSpacing;
  const auto layout = M4ReaderMenuLayout::makeProgressPanelLayout(contentX, contentWidth, contentTop);

  GUI.drawHeader(renderer, Rect{contentX, hintGutterHeight, contentWidth, metrics.headerHeight}, "阅读进度");

  const std::string percentText = std::to_string(percent) + "%";
  M4UiText::drawCenteredInBox(renderer, UI_12_FONT_ID, layout.value.x, layout.value.y,
                              layout.value.width, layout.value.height, percentText.c_str(),
                              true, EpdFontFamily::BOLD, 8);

  // Reading progress should read as one calm continuum, not as an instrument
  // scale. The large percentage carries the precise value; the bar only shows
  // position and remains a generous one-shot touch target through trackHit.
  renderer.drawRect(layout.track.x, layout.track.y, layout.track.width, layout.track.height, true);
  const int innerWidth = std::max(0, layout.track.width - 4);
  const int fillWidth = innerWidth * percent / 100;
  if (fillWidth > 0) {
    renderer.fillRect(layout.track.x + 2, layout.track.y + 2, fillWidth,
                      std::max(1, layout.track.height - 4), true);
  }

  const int labelY = layout.track.y + layout.track.height + 8;
  M4UiText::draw(renderer, UI_10_FONT_ID, layout.track.x, labelY, "0%", true, EpdFontFamily::REGULAR);
  const char* endLabel = "100%";
  const int endWidth = M4UiText::textWidth(renderer, UI_10_FONT_ID, endLabel);
  M4UiText::draw(renderer, UI_10_FONT_ID, layout.track.x + layout.track.width - endWidth,
                 labelY, endLabel, true, EpdFontFamily::REGULAR);

  for (int i = 0; i < M4ReaderMenuLayout::kProgressStepCount; ++i) {
    const auto r = layout.stepRect(i);
    renderer.drawRect(r.x, r.y, r.width, r.height, true);
    M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, r.x, r.y, r.width, r.height,
                                kStepLabels[i], true, EpdFontFamily::BOLD, 8);
  }

  const auto labels = mappedInput.mapLabels("« 返回", "跳转", "-1%", "+1%");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (firstPaint) {
    firstPaint = false;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}
