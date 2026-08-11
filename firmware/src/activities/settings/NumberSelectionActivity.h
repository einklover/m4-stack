#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>

#include "MappedInputManager.h"
#include "activities/ActivityWithSubactivity.h"

/**
 * 通用数字选择器Activity
 * 支持整数范围选择，带滑块UI
 * 用于：刷新频率、行间距、字间距、边距等设置
 */
class NumberSelectionActivity final : public ActivityWithSubactivity {
 public:
  struct Config {
    std::string title;        // 标题文本
    std::string unit;         // 单位文本（如"页"、"秒"）
    int minValue;             // 最小值
    int maxValue;             // 最大值
    int smallStep = 1;        // 小步长（左右键）
    int largeStep = 5;        // 大步长（上下键）
    bool isSigned = false;    // 是否为有符号数（显示负数）
    // 可选的自定义显示格式化函数，若设置则替代默认的数字显示逻辑
    std::function<std::string(int)> displayFormatter;
  };

  explicit NumberSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                   const Config& config, int initialValue,
                                   const std::function<void(int)>& onSelect,
                                   const std::function<void()>& onCancel)
      : ActivityWithSubactivity("NumberSelection", renderer, mappedInput),
        config(config),
        value(initialValue),
        onSelect(onSelect),
        onCancel(onCancel) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  Config config;
  int value;
  bool updateRequired = false;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;

  const std::function<void(int)> onSelect;
  const std::function<void()> onCancel;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();
  void adjustValue(int delta);
};
