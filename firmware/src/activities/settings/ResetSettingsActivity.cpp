#include "ResetSettingsActivity.h"

#include <GfxRenderer.h>
#include <HardwareSerial.h>

#include "CrossPointSettings.h"
#include "I18n.h"
#include "MappedInputManager.h"
#include "activities/settings/M4SettingsConfirm.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4ListTouchPolicy.h"
#include "util/M4UiText.h"

namespace {

M4ListTouchPolicy::DialogTwoButtonLayout warningDialogLayout(const GfxRenderer& renderer) {
  return M4ListTouchPolicy::makeCenteredTwoButtons(renderer.getScreenWidth(), renderer.getScreenHeight() - 190,
                                                   144, 64, 24, 2);
}

void drawWarningButton(const GfxRenderer& renderer, const M4ListTouchPolicy::DialogTwoButtonLayout& layout,
                       int index, const char* label) {
  const auto r = layout.buttonRect(index);
  renderer.fillRoundedRect(r.x, r.y, r.width, r.height, 12, index == 1 ? Color::Black : Color::LightGray);
  M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, r.x, r.y, r.width, r.height, label, index == 0,
                              EpdFontFamily::BOLD, 8);
}

}  // namespace

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
    const auto dialog = warningDialogLayout(renderer);
    drawWarningButton(renderer, dialog, 0, L(Str::kCancel));
    drawWarningButton(renderer, dialog, 1, L(Str::kConfirmReset));
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
    int tx = 0, ty = 0;
    if (mappedInput.hasTouch() && mappedInput.wasScreenTapped(tx, ty)) {
      int hit = -1;
      if (M4ListTouchPolicy::dialogButtonFromPoint(warningDialogLayout(renderer), tx, ty, hit)) {
        if (hit == 0) {
          Serial.printf("[%lu] [RESET] User cancelled by touch\n", millis());
          goBack();
        } else {
          Serial.printf("[%lu] [RESET] User confirmed by touch\n", millis());
          xSemaphoreTake(renderingMutex, portMAX_DELAY);
          state = RESETTING;
          xSemaphoreGive(renderingMutex);
          updateRequired = true;
          vTaskDelay(10 / portTICK_PERIOD_MS);
          doReset();
        }
        return;
      }
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Power) &&
        m4SettingsDangerAccepts(M4ConfirmButton::Power, true)) {
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) &&
        m4SettingsDangerAccepts(M4ConfirmButton::Confirm, true)) {
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
    int tx = 0, ty = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        (mappedInput.hasTouch() && mappedInput.wasScreenTapped(tx, ty))) {
      goBack();
    }
    return;
  }
}
