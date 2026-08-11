#include "AutoPageTurnIntervalActivity.h"

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4UiText.h"

namespace {
// Fine/coarse slider step sizes for interval adjustments.
constexpr int kSmallStep = 1;
constexpr int kLargeStep = 5;
constexpr int kMinInterval = 1;
constexpr int kMaxInterval = 60;
}  // namespace

void AutoPageTurnIntervalActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  // Set up rendering task and mark first frame dirty.
  renderingMutex = xSemaphoreCreateMutex();
  updateRequired = true;
  xTaskCreate(&AutoPageTurnIntervalActivity::taskTrampoline, "AutoPageInterval", 4096, this, 1,
              &displayTaskHandle);
}

void AutoPageTurnIntervalActivity::onExit() {
  ActivityWithSubactivity::onExit();
  // Ensure the render task is stopped before freeing the mutex.
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void AutoPageTurnIntervalActivity::taskTrampoline(void* param) {
  auto* self = static_cast<AutoPageTurnIntervalActivity*>(param);
  self->displayTaskLoop();
}

void AutoPageTurnIntervalActivity::displayTaskLoop() {
  while (true) {
    // Render only when the view is dirty and no subactivity is running.
    if (updateRequired && !subActivity) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      renderScreen();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void AutoPageTurnIntervalActivity::adjustInterval(const int delta) {
  // Apply delta and clamp within 1-60.
  interval += delta;
  if (interval < kMinInterval) {
    interval = kMinInterval;
  } else if (interval > kMaxInterval) {
    interval = kMaxInterval;
  }
  updateRequired = true;
}

void AutoPageTurnIntervalActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  // Back cancels, confirm selects, arrows adjust the interval.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) {
    onCancel();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    onSelect(interval);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    adjustInterval(-kSmallStep);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    adjustInterval(kSmallStep);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    adjustInterval(kLargeStep);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    adjustInterval(-kLargeStep);
    return;
  }
}

void AutoPageTurnIntervalActivity::renderScreen() {
  renderer.clearScreen();

  // Title
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, 15, "翻页间隔", true, EpdFontFamily::BOLD);

  // Numeric interval value
  const std::string intervalText = std::to_string(interval) + " 秒";
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, 90, intervalText.c_str(), true, EpdFontFamily::BOLD);

  // Draw slider track.
  const int screenWidth = renderer.getScreenWidth();
  constexpr int barWidth = 360;
  constexpr int barHeight = 16;
  const int barX = (screenWidth - barWidth) / 2;
  const int barY = 140;

  renderer.drawRect(barX, barY, barWidth, barHeight);

  // Fill slider based on interval (1-60 mapped to 0-100%).
  const int fillPercent = (interval - kMinInterval) * 100 / (kMaxInterval - kMinInterval);
  const int fillWidth = (barWidth - 4) * fillPercent / 100;
  if (fillWidth > 0) {
    renderer.fillRect(barX + 2, barY + 2, fillWidth, barHeight - 4);
  }

  // Draw a simple knob centered at the current position.
  const int knobX = barX + 2 + fillWidth - 2;
  renderer.fillRect(knobX, barY - 4, 4, barHeight + 8, true);

  // Hint text for step sizes.
  renderer.drawCenteredText(SMALL_FONT_ID, barY + 30, "左/右: ±1秒  上/下: ±5秒", true);

  // Button hints follow the current front button layout.
  const auto labels = mappedInput.mapLabels("« 返回", "确定", "-", "+");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
