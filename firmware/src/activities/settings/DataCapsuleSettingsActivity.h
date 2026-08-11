#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>

#include "activities/ActivityWithSubactivity.h"

/**
 * Settings Activity for Data Capsule (数据胶囊) WebDAV.
 * WebDAV URL is configurable via dcWebdavUrl setting.
 * Default: https://data.cstcloud.cn/dav
 * User configures username, password, and optionally the WebDAV URL.
 */
class DataCapsuleSettingsActivity final : public ActivityWithSubactivity {
 public:
  explicit DataCapsuleSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                   const std::function<void()>& onBack)
      : ActivityWithSubactivity("DataCapsuleSettings", renderer, mappedInput), onBack(onBack) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;

  int selectedIndex = 0;
  const std::function<void()> onBack;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render();
  void handleSelection();
};
