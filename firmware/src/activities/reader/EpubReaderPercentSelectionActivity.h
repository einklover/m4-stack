#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>

#include "MappedInputManager.h"
#include "activities/ActivityWithSubactivity.h"

class EpubReaderPercentSelectionActivity final : public ActivityWithSubactivity {
 public:
  // Touch-friendly percent selector for jumping within a book.
  // onToolbar: persistent reader bar while the sheet is open.
  // index 0=目录 1=进度 2=字体 3=更多. Progress (1) is ignored by the caller.
  explicit EpubReaderPercentSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                              const int initialPercent, const std::function<void(int)>& onSelect,
                                              const std::function<void()>& onCancel,
                                              const std::function<void(int)>& onToolbar = {})
      : ActivityWithSubactivity("EpubReaderPercentSelection", renderer, mappedInput),
        percent(initialPercent),
        onSelect(onSelect),
        onCancel(onCancel),
        onToolbar(onToolbar) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

  std::string debugUiJson() override {
    std::string out = "{\"overlay\":true,\"sheet\":\"progress\",\"percent\":";
    out += std::to_string(percent);
    out += "}";
    return out;
  }

 private:
  int percent = 0;
  bool updateRequired = false;
  bool firstPaint = true;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;

  const std::function<void(int)> onSelect;
  const std::function<void()> onCancel;
  const std::function<void(int)> onToolbar;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();
  void adjustPercent(int delta);
};
