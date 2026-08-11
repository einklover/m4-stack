#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>

#include "MappedInputManager.h"
#include "activities/ActivityWithSubactivity.h"

class AutoPageTurnIntervalActivity final : public ActivityWithSubactivity {
 public:
  // Slider-style interval selector for auto page turn (1-60 seconds).
  explicit AutoPageTurnIntervalActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        const int initialInterval, const std::function<void(int)>& onSelect,
                                        const std::function<void()>& onCancel)
      : ActivityWithSubactivity("AutoPageTurnInterval", renderer, mappedInput),
        interval(initialInterval),
        onSelect(onSelect),
        onCancel(onCancel) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  // Current interval value (1-60 seconds).
  int interval = 10;
  // Render dirty flag for the task loop.
  bool updateRequired = false;
  // FreeRTOS task and mutex for rendering.
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;

  // Callback invoked when the user confirms an interval.
  const std::function<void(int)> onSelect;
  // Callback invoked when the user cancels the selector.
  const std::function<void()> onCancel;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  // Render the selector UI.
  void renderScreen();
  // Change the current interval by a delta and clamp within bounds.
  void adjustInterval(int delta);
};
