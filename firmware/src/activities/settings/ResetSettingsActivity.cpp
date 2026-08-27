#include "ResetSettingsActivity.h"

#include <GfxRenderer.h>
#include <HardwareSerial.h>

#include "CrossPointSettings.h"
#include "I18n.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4UiText.h"

void ResetSettingsActivity::taskTrampoline(void* param) {
  auto* self = static_cast<ResetSettingsActivity*>(param);
  self->displayTaskLoop();
}

void ResetSettingsActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();
  state = WARNING;
  updateRequired = true;

  xTaskCreate(&ResetSettingsActivity::taskTrampoline, "ResetSettingsTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void ResetSettingsActivity::onExit() {
  ActivityWithSubactivity::onExit();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void ResetSettingsActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void ResetSettingsActivity::render() {
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, 15, L(Str::kResetSettingsTitle), true, EpdFontFamily::BOLD);
  
  if (state == WARNING) {
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, pageHeight / 2 - 60, L(Str::kResetSettingsDesc), true);
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, pageHeight / 2 - 25, L(Str::kResetSettingsDetails), true);
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, pageHeight / 2 + 15, L(Str::kResetSettingsWarn), true,
                              EpdFontFamily::BOLD);
  
    const auto labels = mappedInput.mapLabels(L(Str::kCancel), L(Str::kConfirmReset), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == RESETTING) {
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, pageHeight / 2, L(Str::kResetting), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == SUCCESS) {
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, pageHeight / 2 - 20, L(Str::kResetSuccess), true, EpdFontFamily::BOLD);
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, pageHeight / 2 + 15, L(Str::kRebootToApply));
    
    const auto labels = mappedInput.mapLabels(L(Str::kBack), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void ResetSettingsActivity::doReset() {
  Serial.printf("[%lu] [RESET] Resetting all settings to defaults\n", millis());
  SETTINGS.resetToDefaults();
  SETTINGS.saveToFile();
  state = SUCCESS;
  updateRequired = true;
}

void ResetSettingsActivity::loop() {
  if (state == WARNING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      Serial.printf("[%lu] [RESET] User confirmed, resetting settings\n", millis());
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      state = RESETTING;
      xSemaphoreGive(renderingMutex);
      updateRequired = true;
      vTaskDelay(10 / portTICK_PERIOD_MS);

      doReset();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      Serial.printf("[%lu] [RESET] User cancelled\n", millis());
      goBack();
    }
    return;
  }

  if (state == SUCCESS) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }
}
