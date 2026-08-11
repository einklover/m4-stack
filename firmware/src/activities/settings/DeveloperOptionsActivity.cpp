#include "DeveloperOptionsActivity.h"

#include <GfxRenderer.h>
#include <HardwareSerial.h>

#include "CrossPointSettings.h"
#include "I18n.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4UiText.h"

void DeveloperOptionsActivity::taskTrampoline(void* param) {
  static_cast<DeveloperOptionsActivity*>(param)->displayTaskLoop();
}

void DeveloperOptionsActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  renderingMutex_ = xSemaphoreCreateMutex();
  updateRequired_ = true;
  xTaskCreate(&DeveloperOptionsActivity::taskTrampoline, "DevOpts", 4096, this, 1, &displayTaskHandle_);
}

void DeveloperOptionsActivity::onExit() {
  ActivityWithSubactivity::onExit();
  xSemaphoreTake(renderingMutex_, portMAX_DELAY);
  if (displayTaskHandle_) {
    vTaskDelete(displayTaskHandle_);
    displayTaskHandle_ = nullptr;
  }
  vSemaphoreDelete(renderingMutex_);
  renderingMutex_ = nullptr;
}

void DeveloperOptionsActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired_) {
      updateRequired_ = false;
      xSemaphoreTake(renderingMutex_, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex_);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void DeveloperOptionsActivity::toggleSerialDebug() {
  const uint8_t next = SETTINGS.developerSerialDebugEnabled ? 0 : 1;
  SETTINGS.developerSerialDebugEnabled = next;
  SETTINGS.saveToFile();
  // Main loop setAuthorized() applies immediately; log without secrets.
  Serial.printf("[%lu] [DEVOPT] USB serial debugging %s (local UI)\n", millis(), next ? "ON" : "OFF");
  updateRequired_ = true;
}

void DeveloperOptionsActivity::saveAndExit() {
  SETTINGS.saveToFile();
  goBack_();
}

void DeveloperOptionsActivity::render() {
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  const auto metrics = UITheme::getInstance().getMetrics();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageW, metrics.headerHeight}, L(Str::kDeveloperOptions));

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 8;

  // Toggle row
  const bool on = SETTINGS.developerSerialDebugEnabled != 0;
  M4UiText::draw(renderer, UI_12_FONT_ID, 16, y, L(Str::kUsbSerialDebug), true, EpdFontFamily::BOLD);
  const char* stateLabel = on ? L(Str::kOn) : L(Str::kOff);
  const int stateW = M4UiText::textWidth(renderer, UI_12_FONT_ID, stateLabel);
  M4UiText::draw(renderer, UI_12_FONT_ID, pageW - 16 - stateW, y, stateLabel, true, EpdFontFamily::BOLD);
  y += metrics.listRowHeight;

  // Warning box
  y += 8;
  M4UiText::draw(renderer, UI_10_FONT_ID, 16, y, L(Str::kUsbSerialDebugWarn1), true);
  y += 28;
  M4UiText::draw(renderer, UI_10_FONT_ID, 16, y, L(Str::kUsbSerialDebugWarn2), true);
  y += 28;
  M4UiText::draw(renderer, UI_10_FONT_ID, 16, y, L(Str::kUsbSerialDebugWarn3), true);
  y += 36;
  M4UiText::draw(renderer, UI_10_FONT_ID, 16, y, L(Str::kUsbSerialDebugPersist), true);
  y += 28;
  // Plugin error log path is fixed; keep ASCII/safe CJK for the subset font.
  M4UiText::draw(renderer, UI_10_FONT_ID, 16, y, "插件错误: apps_data/.../logs/error.log", true);

  const auto labels = mappedInput.mapLabels(L(Str::kBack), L(Str::kToggle), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void DeveloperOptionsActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) {
    saveAndExit();
    return;
  }

  // Confirm toggles
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    toggleSerialDebug();
    return;
  }

  // Touch: tap toggle row (upper content area)
  if (mappedInput.hasTouch()) {
    int tx = 0, ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty)) {
      const auto metrics = UITheme::getInstance().getMetrics();
      const int rowTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
      const int rowBottom = rowTop + metrics.listRowHeight + 20;
      if (ty >= rowTop && ty < rowBottom) {
        toggleSerialDebug();
      }
    }
  }
}
