#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>

#include "activities/ActivityWithSubactivity.h"

// Murphy M4 device-only: Developer Options page with USB serial debugging toggle.
// Not exposed on the Wi-Fi web settings API.
class DeveloperOptionsActivity final : public ActivityWithSubactivity {
 public:
  explicit DeveloperOptionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                    const std::function<void()>& goBack)
      : ActivityWithSubactivity("DeveloperOptions", renderer, mappedInput), goBack_(goBack) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  TaskHandle_t displayTaskHandle_ = nullptr;
  SemaphoreHandle_t renderingMutex_ = nullptr;
  bool updateRequired_ = false;
  const std::function<void()> goBack_;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render();
  void toggleSerialDebug();
  void saveAndExit();
};
