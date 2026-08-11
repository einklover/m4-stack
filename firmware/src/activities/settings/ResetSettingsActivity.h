#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>

#include "activities/ActivityWithSubactivity.h"

class ResetSettingsActivity final : public ActivityWithSubactivity {
 public:
  explicit ResetSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 const std::function<void()>& goBack)
      : ActivityWithSubactivity("ResetSettings", renderer, mappedInput), goBack(goBack) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  enum State { WARNING, RESETTING, SUCCESS };

  State state = WARNING;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;
  const std::function<void()> goBack;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render();
  void doReset();
};
