#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "MappedInputManager.h"
#include "activities/ActivityWithSubactivity.h"

/**
 * 晃动翻页设置页面
 * 提供倾斜角度、回正角度、持续时间、冷却时间、左右倾斜功能、大幅度倾斜功能等设置
 */
class TiltPageTurnSettingsActivity final : public ActivityWithSubactivity {
 public:
  explicit TiltPageTurnSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        const std::function<void()>& onGoBack)
      : ActivityWithSubactivity("TiltPageTurnSettings", renderer, mappedInput), onGoBack(onGoBack) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  enum class SettingItemType { NUMBER, ENUM };

  struct TiltSettingItem {
    const char* label;
    SettingItemType type;
    // For NUMBER type: value range
    int minValue = 0;
    int maxValue = 100;
    int smallStep = 1;
    int largeStep = 5;
    std::string unit;
    // For ENUM type: option labels
    std::vector<const char*> enumLabels;
  };

  int selectedIndex = 0;
  bool updateRequired = false;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  const std::function<void()> onGoBack;

  // Get the list of setting items
  std::vector<TiltSettingItem> getSettingItems() const;
  // Get the current value for a setting item by index
  int getCurrentValue(int index) const;
  // Get display string for a setting item value
  std::string getValueDisplay(int index) const;
  // Toggle/cycle enum values on confirm
  void toggleCurrentSetting();
  // Open number selector for numeric settings
  void openNumberSelector(int index);

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();
};
