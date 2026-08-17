#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>

#include "MappedInputManager.h"
#include "activities/ActivityWithSubactivity.h"

class EpubReaderPercentSelectionActivity final : public ActivityWithSubactivity {
 public:
  // Touch-friendly percent selector for jumping within a book.
  explicit EpubReaderPercentSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                              const int initialPercent, const std::function<void(int)>& onSelect,
                                              const std::function<void()>& onCancel)
      : ActivityWithSubactivity("EpubReaderPercentSelection", renderer, mappedInput),
        percent(initialPercent),
        onSelect(onSelect),
        onCancel(onCancel) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  int percent = 0;
  bool updateRequired = false;
  bool firstPaint = true;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;

  const std::function<void(int)> onSelect;
  const std::function<void()> onCancel;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();
  void adjustPercent(int delta);
};
