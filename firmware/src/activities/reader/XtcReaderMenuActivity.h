#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "../ActivityWithSubactivity.h"
#include "LanguageMapper.h"

class XtcReaderMenuActivity final : public ActivityWithSubactivity {
 public:
  enum class MenuAction { AUTO_PAGE_TURN, PAGE_TURN_MODE, PAGE_TURN_INTERVAL, GO_TO_PERCENT, ROTATE_SCREEN, ANTI_ALIAS, DELETE_CACHE, GO_HOME };

  explicit XtcReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                  const std::string& title,
                                  const int currentPage, const int totalPages,
                                  const int bookProgressPercent,
                                  const uint8_t currentOrientation,
                                  const bool currentAntiAlias,
                                  const std::function<void(uint8_t, bool)>& onBack,
                                  const std::function<void(MenuAction)>& onAction)
      : ActivityWithSubactivity("XtcReaderMenu", renderer, mappedInput),
        title(title),
        pendingOrientation(currentOrientation),
        pendingAntiAlias(currentAntiAlias),
        currentPage(currentPage),
        totalPages(totalPages),
        bookProgressPercent(bookProgressPercent),
        onBack(onBack),
        onAction(onAction) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  struct MenuItem {
    MenuAction action;
    std::string label;
  };

  // 菜单项：自动翻页、翻页方式、翻页间隔、进度跳转、阅读方向、抗锯齿、清理缓存、返回主页
  const std::vector<MenuItem> menuItems = {
      {MenuAction::AUTO_PAGE_TURN, "1) 自动翻页"},
      {MenuAction::PAGE_TURN_INTERVAL, "2) 翻页间隔"},
      {MenuAction::GO_TO_PERCENT,  "3) 进度跳转"},
      {MenuAction::ROTATE_SCREEN,  "4) 阅读方向"},
      {MenuAction::ANTI_ALIAS,     "5) 抗锯齿"},
      {MenuAction::DELETE_CACHE,   "6) 清理缓存"},
      {MenuAction::GO_HOME,        "7) 返回主页"},
  };

  int selectedIndex = 0;
  bool updateRequired = false;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;

  std::string title;
  uint8_t pendingOrientation = 0;
  bool pendingAntiAlias = false;

  const std::vector<const char*> orientationLabels = {
      getChineseName("Portrait"),
      getChineseName("Landscape CW"),
      "按钮在上面",
      getChineseName("Landscape CCW"),
  };

  const std::vector<const char*> autoPageTurnLabels = {"关闭", "开启"};
  const std::vector<const char*> pageTurnModeLabels = {"全屏翻页", "半屏卷帘"};

  int currentPage = 0;
  int totalPages = 0;
  int bookProgressPercent = 0;

  const std::function<void(uint8_t, bool)> onBack;
  const std::function<void(MenuAction)> onAction;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();
};
