#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <vector>

#include "../ActivityWithSubactivity.h"
#include "../settings/SettingsActivity.h"

// In-reader settings page: shows only the "Reader" category from SettingsLists.
class EpubReaderSettingsActivity final : public ActivityWithSubactivity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;
  mutable bool firstPaint_ = true;  // HALF after reader AA residual
  // A tap/release that opened this activity must not be reinterpreted as the
  // first list touch after a busy reader page-turn/menu handoff. Buttons are
  // intentionally handled independently in loop(). Two frames covers the
  // opener edge plus one residual held-touch sample after animation.
  uint8_t touchHandoffFrames_ = 0;
  int selectedIndex = 0;
  std::vector<SettingInfo> settings;
  const std::function<void()> onGoBack;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;
  void toggleCurrentSetting();

 public:
  explicit EpubReaderSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                      const std::function<void()>& onGoBack);
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
