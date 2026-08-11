#include "NumberSelectionActivity.h"

#include <GfxRenderer.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "I18n.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4UiText.h"

void NumberSelectionActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();
  updateRequired = true;
  xTaskCreate(&NumberSelectionActivity::taskTrampoline, "NumberSelection", 4096, this, 1,
              &displayTaskHandle);
}

void NumberSelectionActivity::onExit() {
  ActivityWithSubactivity::onExit();
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void NumberSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<NumberSelectionActivity*>(param);
  self->displayTaskLoop();
}

void NumberSelectionActivity::displayTaskLoop() {
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

void NumberSelectionActivity::adjustValue(const int delta) {
  value += delta;
  if (value < config.minValue) {
    value = config.minValue;
  } else if (value > config.maxValue) {
    value = config.maxValue;
  }
  updateRequired = true;
}

void NumberSelectionActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) {
    onCancel();
    return;
  }

  // Touch controls: tap the slider to set a value, or use the explicit
  // footer buttons. Horizontal swipes are also treated as back here because
  // this screen has no horizontal paging action of its own.
  if (mappedInput.hasTouch()) {
    int touchX = 0;
    int touchY = 0;
    if (mappedInput.wasScreenTapped(touchX, touchY)) {
      const int screenWidth = renderer.getScreenWidth();
      const int screenHeight = renderer.getScreenHeight();
      constexpr int barWidth = 360;
      constexpr int barHeight = 16;
      const int barX = (screenWidth - barWidth) / 2;
      const int barY = 140;
      const int buttonY = screenHeight - 64;
      const int buttonH = 50;

      if (touchY >= buttonY && touchY < buttonY + buttonH) {
        if (touchX < screenWidth / 2) {
          onCancel();
        } else {
          onSelect(value);
        }
        return;
      }

      if (touchX >= barX - 18 && touchX <= barX + barWidth + 18 &&
          touchY >= barY - 24 && touchY <= barY + barHeight + 24) {
        const int clampedX = std::max(barX, std::min(barX + barWidth, touchX));
        const int range = config.maxValue - config.minValue;
        value = range > 0 ? config.minValue + (clampedX - barX) * range / barWidth : config.minValue;
        updateRequired = true;
        return;
      }
    }

    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Left ||
        swipe == MappedInputManager::SwipeDir::Right) {
      onCancel();
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    onSelect(value);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    adjustValue(-config.smallStep);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    adjustValue(config.smallStep);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    adjustValue(config.largeStep);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    adjustValue(-config.largeStep);
    return;
  }
}

void NumberSelectionActivity::renderScreen() {
  renderer.clearScreen();

  // 标题
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, 15, config.title.c_str(), true, EpdFontFamily::BOLD);

  // 数值显示
  std::string valueText;
  if (config.displayFormatter) {
    valueText = config.displayFormatter(value);
  } else if (config.isSigned && value >= 0) {
    valueText = "+" + std::to_string(value);
  } else {
    valueText = std::to_string(value);
  }
  if (!config.displayFormatter && !config.unit.empty()) {
    valueText += " " + config.unit;
  }
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, 90, valueText.c_str(), true, EpdFontFamily::BOLD);

  // 滑块轨道
  const int screenWidth = renderer.getScreenWidth();
  constexpr int barWidth = 360;
  constexpr int barHeight = 16;
  const int barX = (screenWidth - barWidth) / 2;
  const int barY = 140;

  renderer.drawRect(barX, barY, barWidth, barHeight);

  // 填充滑块
  const int range = config.maxValue - config.minValue;
  const int fillPercent = range > 0 ? (value - config.minValue) * 100 / range : 0;
  const int fillWidth = (barWidth - 4) * fillPercent / 100;
  if (fillWidth > 0) {
    renderer.fillRect(barX + 2, barY + 2, fillWidth, barHeight - 4);
  }

  // 滑块指示器
  const int knobX = barX + 2 + fillWidth - 2;
  renderer.fillRect(knobX, barY - 4, 4, barHeight + 8, true);

  // 操作提示
  std::string hintText = std::string(L(Str::kLeft)) + "/" + L(Str::kRight) + ": \302\261" + std::to_string(config.smallStep);
  if (config.largeStep != config.smallStep) {
    hintText += "  " + std::string(L(Str::kUp)) + "/" + L(Str::kDown) + ": \302\261" + std::to_string(config.largeStep);
  }
  renderer.drawCenteredText(SMALL_FONT_ID, barY + 30, hintText.c_str(), true);

  if (mappedInput.hasTouch()) {
    // Touch devices need visible actions; the physical-button hint bar is not
    // an actionable return control on this screen.
    const int buttonY = renderer.getScreenHeight() - 64;
    const int buttonH = 50;
    const int margin = 18;
    const int gap = 12;
    const int buttonW = (screenWidth - margin * 2 - gap) / 2;
    const int backX = margin;
    const int confirmX = backX + buttonW + gap;
    renderer.drawRect(backX, buttonY, buttonW, buttonH);
    renderer.drawRect(confirmX, buttonY, buttonW, buttonH);
    M4UiText::drawCenteredInBox(renderer, UI_12_FONT_ID, backX, buttonY, buttonW, buttonH,
                                L(Str::kBack), true);
    M4UiText::drawCenteredInBox(renderer, UI_12_FONT_ID, confirmX, buttonY, buttonW, buttonH,
                                L(Str::kConfirmShort), true);
  } else {
    const auto labels = mappedInput.mapLabels(L(Str::kBack), L(Str::kConfirmShort), "-", "+");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
