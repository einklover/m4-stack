#include "OtaUpdateActivity.h"

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "I18n.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4UiText.h"

void SdOtaUpdateActivity::taskTrampoline(void* param) {
  static_cast<SdOtaUpdateActivity*>(param)->displayTaskLoop();
}

void SdOtaUpdateActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();
  xTaskCreate(&SdOtaUpdateActivity::taskTrampoline, "SdOtaTask",
              4096, this, 1, &displayTaskHandle);

  // Immediately check SD card
  updateRequired = true;
  vTaskDelay(10 / portTICK_PERIOD_MS);

  auto res = updater.checkSdCard();
  if (res == SdOtaUpdater::NO_UPDATE) {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    state = NO_FIRMWARE;
    xSemaphoreGive(renderingMutex);
    updateRequired = true;
  } else if (res != SdOtaUpdater::OK) {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    state = FAILED;
    xSemaphoreGive(renderingMutex);
    updateRequired = true;
  } else {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    state = WAITING_CONFIRMATION;
    progressTotal = updater.getFirmwareSize();
    xSemaphoreGive(renderingMutex);
    updateRequired = true;
  }
}

void SdOtaUpdateActivity::onExit() {
  ActivityWithSubactivity::onExit();
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void SdOtaUpdateActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

void SdOtaUpdateActivity::render() {
  if (subActivity) return;

  const auto pageWidth = renderer.getScreenWidth();
  renderer.clearScreen();
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, 15, L(Str::kSdCardUpdateTitle), true, EpdFontFamily::BOLD);

  if (state == CHECKING_SD) {
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 300, L(Str::kCheckingSdCard), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == NO_FIRMWARE) {
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 280, L(Str::kNoUpdateFileFound), true, EpdFontFamily::BOLD);
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 320, L(Str::kPutFirmwareIn));
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 345, L(Str::kSdCardUpdateDir));
    renderer.displayBuffer();
    return;
  }

  if (state == WAITING_CONFIRMATION) {
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 240, L(Str::kUpdateFileFound), true, EpdFontFamily::BOLD);
    std::string sizeStr = std::string(L(Str::kFileSize)) + std::to_string(updater.getFirmwareSize() / 1024) + " KB";
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 280, sizeStr.c_str());
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 320, L(Str::kDoNotPowerOff));

    const auto labels = mappedInput.mapLabels(L(Str::kCancel), L(Str::kStartFlashing), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == FLASHING) {
    float pct = (progressTotal > 0)
                    ? static_cast<float>(progressDone) / static_cast<float>(progressTotal)
                    : 0.0f;
    unsigned int pctInt = static_cast<unsigned int>(pct * 100);
    if (pctInt == lastPercentage) return;
    lastPercentage = pctInt;

    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 280, L(Str::kPreparingUpdate), true, EpdFontFamily::BOLD);
    renderer.drawRect(20, 350, pageWidth - 40, 50);
    renderer.fillRect(24, 354, static_cast<int>(pct * static_cast<float>(pageWidth - 44)), 42);
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 420, (std::to_string(pctInt) + "%").c_str());
    renderer.displayBuffer();
    return;
  }

  if (state == FAILED) {
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 300, L(Str::kUpdateFailed), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == FINISHED) {
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 300, L(Str::kAutoUpdateAfterReboot), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    state = SHUTTING_DOWN;
    return;
  }
}

void SdOtaUpdateActivity::loop() {
  if (subActivity) {
    pumpSubActivityFrame();
    return;
  }

  if (state == WAITING_CONFIRMATION) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      Serial.printf("[%lu] [SdOTA] User confirmed, flashing...\n", millis());

      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      state = FLASHING;
      progressDone = 0;
      lastPercentage = UNINITIALIZED_PERCENTAGE;
      xSemaphoreGive(renderingMutex);
      updateRequired = true;
      vTaskDelay(10 / portTICK_PERIOD_MS);

      auto res = updater.flashUpdaterAndReboot([this](size_t done, size_t total) {
        progressDone = done;
        progressTotal = total;
        updateRequired = true;
      });

      if (res != SdOtaUpdater::OK) {
        Serial.printf("[%lu] [SdOTA] Flash failed: %d\n", millis(), res);
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        state = FAILED;
        xSemaphoreGive(renderingMutex);
        updateRequired = true;
        return;
      }

      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      state = FINISHED;
      xSemaphoreGive(renderingMutex);
      updateRequired = true;
      // Give render task time to display the message
      vTaskDelay(2000 / portTICK_PERIOD_MS);
      ESP.restart();
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }

  if (state == FAILED || state == NO_FIRMWARE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }

  if (state == SHUTTING_DOWN) {
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    ESP.restart();
  }
}
