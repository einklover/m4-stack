#include "ClearCacheActivity.h"

#include <GfxRenderer.h>
#include <HardwareSerial.h>
#include <SDCardManager.h>

#include "I18n.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4UiText.h"

void ClearCacheActivity::taskTrampoline(void* param) {
  auto* self = static_cast<ClearCacheActivity*>(param);
  self->displayTaskLoop();
}

void ClearCacheActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();
  state = WARNING;
  updateRequired = true;

  xTaskCreate(&ClearCacheActivity::taskTrampoline, "ClearCacheActivityTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void ClearCacheActivity::onExit() {
  ActivityWithSubactivity::onExit();

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void ClearCacheActivity::displayTaskLoop() {
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

void ClearCacheActivity::render() {
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, 15, L(Str::kClearCacheTitle), true, EpdFontFamily::BOLD);
  
  if (state == WARNING) {
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, pageHeight / 2 - 60, L(Str::kClearCacheDesc1), true);
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, pageHeight / 2 - 30, L(Str::kClearCacheDesc2), true,
                              EpdFontFamily::BOLD);
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, pageHeight / 2 + 10, L(Str::kClearCacheDesc3), true);
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, pageHeight / 2 + 30, L(Str::kClearCacheDesc4), true);
  
    const auto labels = mappedInput.mapLabels(L(Str::kCancel), L(Str::kClear), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == CLEARING) {
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, pageHeight / 2, L(Str::kClearingCache), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == SUCCESS) {
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, pageHeight / 2 - 20, L(Str::kCacheCleared), true, EpdFontFamily::BOLD);
    String resultText = String(clearedCount) + L(Str::kItemsCleared);
    if (failedCount > 0) {
      resultText += ", " + String(failedCount) + L(Str::kItemsClearFailed);
    }
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, pageHeight / 2 + 10, resultText.c_str());
    
    const auto labels = mappedInput.mapLabels(L(Str::kBack), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == FAILED) {
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, pageHeight / 2 - 20, L(Str::kCacheClearFailed), true, EpdFontFamily::BOLD);
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, pageHeight / 2 + 10, L(Str::kCheckSerialOutput));
    
    const auto labels = mappedInput.mapLabels(L(Str::kBack), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void ClearCacheActivity::clearCache() {
  Serial.printf("[%lu] [CLEAR_CACHE] Clearing cache...\n", millis());

  // Open .crosspoint directory
  auto root = SdMan.open("/.crosspoint");
  if (!root || !root.isDirectory()) {
    Serial.printf("[%lu] [CLEAR_CACHE] Failed to open cache directory\n", millis());
    if (root) root.close();
    state = FAILED;
    updateRequired = true;
    return;
  }

  clearedCount = 0;
  failedCount = 0;
  char name[128];

  // Iterate through all entries in the directory
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    String itemName(name);

    // Only delete directories starting with epub_, xtc_, or txt_
    if (file.isDirectory() && (itemName.startsWith("epub_") || itemName.startsWith("xtc_") || itemName.startsWith("txt_"))) {
      String fullPath = "/.crosspoint/" + itemName;
      Serial.printf("[%lu] [CLEAR_CACHE] Removing cache: %s\n", millis(), fullPath.c_str());

      file.close();  // Close before attempting to delete

      if (SdMan.removeDir(fullPath.c_str())) {
        clearedCount++;
      } else {
        Serial.printf("[%lu] [CLEAR_CACHE] Failed to remove: %s\n", millis(), fullPath.c_str());
        failedCount++;
      }
    } else {
      file.close();
    }
  }
  root.close();

  Serial.printf("[%lu] [CLEAR_CACHE] Cache cleared: %d removed, %d failed\n", millis(), clearedCount, failedCount);

  state = SUCCESS;
  updateRequired = true;
}

void ClearCacheActivity::loop() {
  if (state == WARNING) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      Serial.printf("[%lu] [CLEAR_CACHE] User confirmed, starting cache clear\n", millis());
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      state = CLEARING;
      xSemaphoreGive(renderingMutex);
      updateRequired = true;
      vTaskDelay(10 / portTICK_PERIOD_MS);

      clearCache();
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      Serial.printf("[%lu] [CLEAR_CACHE] User cancelled\n", millis());
      goBack();
    }
    return;
  }

  if (state == SUCCESS || state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }
}
