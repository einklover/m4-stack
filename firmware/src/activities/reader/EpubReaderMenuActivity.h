#pragma once
#include <Epub.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cstring>
#include <functional>
#include <string>
#include <vector>

#ifdef CROSSPOINT_X3
#include "TiltPageTurnSettingsActivity.h"
#endif
#include "../ActivityWithSubactivity.h"
#include "CrossPointSettings.h"
#include "LanguageMapper.h"

class EpubReaderMenuActivity final : public ActivityWithSubactivity {
 public:
  enum class MenuLayer : uint8_t { QUICK = 0, STYLE = 1, MORE = 2 };

  enum class MenuAction {
    SELECT_CHAPTER,
    ADD_BOOKMARK,
    BOOKMARK_MANAGER,
    GO_TO_PERCENT,
    ROTATE_SCREEN,
    AUTO_PAGE_TURN,
    PAGE_TURN_MODE,
    PAGE_TURN_INTERVAL,
    GO_HOME,
    SYNC,
    SYNCY,
    DELETE_CACHE,
    READER_SETTINGS,
    TOGGLE_ANTI_ALIAS,
    TOGGLE_FONT,
    SELECT_EXTERNAL_FONT,
    TOGGLE_GLOBAL_NEXT_PAGE,
    TOGGLE_DARK_MODE,
    BLUETOOTH_SETTINGS,
    LONG_PRESS_CONFIRM_MAPPING,
#ifdef CROSSPOINT_X3
    TILT_PAGE_TURN,
    TILT_PAGE_TURN_SETTINGS,
#endif
  };

  explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                  const int currentPage, const int totalPages, const int bookProgressPercent,
                                  const uint8_t currentOrientation, const std::function<void(uint8_t)>& onBack,
                                  const std::function<void(MenuAction)>& onAction,
                                  MenuLayer initialLayer = MenuLayer::QUICK)
      : ActivityWithSubactivity("EpubReaderMenu", renderer, mappedInput),
        menuLayer_(initialLayer),
        title(title),
        pendingOrientation(currentOrientation),
        currentPage(currentPage),
        totalPages(totalPages),
        bookProgressPercent(bookProgressPercent),
        onBack(onBack),
        onAction(onAction),
        initialLayer_(initialLayer) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

  std::string debugUiJson() override {
    const char* layer = menuLayer_ == MenuLayer::QUICK ? "quick" :
                        (menuLayer_ == MenuLayer::STYLE ? "style" : "more");
    bool hasSync = false;
    for (const auto& item : dataMenuItems) {
      if (item.action == MenuAction::SYNC || item.action == MenuAction::SYNCY) {
        hasSync = true;
        break;
      }
    }
    std::string out = "{\"layer\":\"";
    out += layer;
    out += "\",\"items\":" + std::to_string(activeMenuItems().size());
    out += ",\"has_sync\":";
    out += hasSync ? "true" : "false";
    out += ",\"overlay\":";
    out += menuLayer_ == MenuLayer::MORE ? "false" : "true";
    if (menuLayer_ == MenuLayer::MORE) {
      out += ",\"more_section\":\"";
      out += moreSectionKey();
      out += "\"";
    }
    if (menuLayer_ == MenuLayer::STYLE) {
      out += ",\"quick_fonts\":" + std::to_string(quickFontFamilies_.size() + 1);
    }
    out += ",\"subactivity\":\"";
    if (subActivity) out += subActivity->getName();
    out += "\"";
    if (subActivity) {
      std::string child = subActivity->debugUiJson();
      if (child.empty()) child = "{}";
      out += ",\"child\":" + child;
    }
    out += "}";
    return out;
  }

 private:
  enum class MoreSection : uint8_t { ROOT = 0, TYPOGRAPHY, TURNING, DISPLAY, CONTROL, DATA };

  enum class InternalAction : uint8_t {
    NONE = 0,
    OPEN_STYLE,
    OPEN_MORE,
    OPEN_MORE_TYPOGRAPHY,
    OPEN_MORE_TURNING,
    OPEN_MORE_DISPLAY,
    OPEN_MORE_CONTROL,
    OPEN_MORE_DATA,
    FONT_DECREASE,
    FONT_INCREASE,
    LAYOUT_COMPACT,
    LAYOUT_STANDARD,
    LAYOUT_RELAXED,
  };

  struct MenuItem {
    MenuAction action;
    std::string label;
    InternalAction internalAction = InternalAction::NONE;
  };

  const std::vector<MenuItem> quickMenuItems = {
      {MenuAction::SELECT_CHAPTER, "目录"},
      {MenuAction::GO_TO_PERCENT, "进度"},
      {MenuAction::READER_SETTINGS, "字体", InternalAction::OPEN_STYLE},
      {MenuAction::GO_HOME, "更多", InternalAction::OPEN_MORE},
  };

  const std::vector<MenuItem> styleMenuItems = {
      {MenuAction::READER_SETTINGS, "A-", InternalAction::FONT_DECREASE},
      {MenuAction::READER_SETTINGS, "A+", InternalAction::FONT_INCREASE},
      {MenuAction::READER_SETTINGS, "紧凑", InternalAction::LAYOUT_COMPACT},
      {MenuAction::READER_SETTINGS, "标准", InternalAction::LAYOUT_STANDARD},
      {MenuAction::READER_SETTINGS, "宽松", InternalAction::LAYOUT_RELAXED},
  };

  const std::vector<MenuItem> moreRootItems = {
      {MenuAction::READER_SETTINGS, "排版与字体", InternalAction::OPEN_MORE_TYPOGRAPHY},
      {MenuAction::AUTO_PAGE_TURN, "翻页与自动", InternalAction::OPEN_MORE_TURNING},
      // Keep bookmark management one tap away; it is content, not a device setting.
      {MenuAction::BOOKMARK_MANAGER, "书签管理"},
      {MenuAction::TOGGLE_ANTI_ALIAS, "显示", InternalAction::OPEN_MORE_DISPLAY},
      {MenuAction::TOGGLE_GLOBAL_NEXT_PAGE, "操作控制", InternalAction::OPEN_MORE_CONTROL},
      {MenuAction::DELETE_CACHE, "数据与缓存", InternalAction::OPEN_MORE_DATA},
  };

  const std::vector<MenuItem> typographyMenuItems = {
      {MenuAction::SELECT_EXTERNAL_FONT, "全部字体"},
      {MenuAction::READER_SETTINGS, "排版与页面"},
  };

  const std::vector<MenuItem> turningMenuItems = {
      {MenuAction::ROTATE_SCREEN, "阅读方向"},
      {MenuAction::AUTO_PAGE_TURN, "自动翻页"},
      {MenuAction::PAGE_TURN_INTERVAL, "自动间隔"},
      {MenuAction::PAGE_TURN_MODE, "自动方式"},
  };

  const std::vector<MenuItem> displayMenuItems = {
      {MenuAction::TOGGLE_ANTI_ALIAS, "抗锯齿"},
      {MenuAction::TOGGLE_DARK_MODE, "暗黑模式"},
  };

  const std::vector<MenuItem> controlMenuItems = {
      {MenuAction::TOGGLE_GLOBAL_NEXT_PAGE, "全局下一页"},
      {MenuAction::LONG_PRESS_CONFIRM_MAPPING, "长按确认键"},
#ifdef CROSSPOINT_X3
      {MenuAction::TILT_PAGE_TURN, "晃动翻页"},
      {MenuAction::TILT_PAGE_TURN_SETTINGS, "晃动设置"},
#endif
  };

  std::vector<MenuItem> dataMenuItems = {
      {MenuAction::SYNC, "KOReader 同步"},
      {MenuAction::SYNCY, "开源阅读同步"},
      {MenuAction::DELETE_CACHE, "清理当前书缓存"},
  };

  MenuLayer menuLayer_ = MenuLayer::QUICK;
  MoreSection moreSection_ = MoreSection::ROOT;
  int selectedIndex = 0;
  bool updateRequired = false;
  bool readerStyleDirty_ = false;
  bool readerFontDirty_ = false;
  bool forceHalfRefresh_ = false;
  std::vector<std::string> quickFontFamilies_;
  std::string pendingPopup_;
  bool firstPaint_ = true;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  std::string title = "Reader Menu";
  uint8_t pendingOrientation = 0;
  const std::vector<const char*> orientationLabels = {getChineseName("Portrait"), getChineseName("Landscape CW"),
                                                       "按钮在上面", getChineseName("Landscape CCW")};
  const std::vector<const char*> autoPageTurnLabels = {"关闭", "开启"};
  const std::vector<const char*> antiAliasLabels = {"关闭", "开启"};
  const std::vector<const char*> darkModeLabels = {"关闭", "开启"};
  const std::vector<const char*> fontLabels = {"系统字体", "外置字体"};
  const std::vector<const char*> globalNextPageLabels = {"关闭", "开启"};
  const std::vector<const char*> pageTurnModeLabels = {"全屏翻页", "半屏翻页"};
#ifdef CROSSPOINT_X3
  const std::vector<const char*> tiltPageTurnLabels = {"关闭", "开启"};
#endif
  const std::vector<const char*> longPressConfirmLabels = {
      "切换全局下一页", "打开蓝牙", "切换自动翻页", "切换抗锯齿", "切换暗黑模式",
#ifdef CROSSPOINT_X3
      "切换晃动翻页",
#endif
      "无",
  };
  int currentPage = 0;
  int totalPages = 0;
  int bookProgressPercent = 0;

  const std::vector<MenuItem>& activeMenuItems() const {
    if (menuLayer_ == MenuLayer::STYLE) return styleMenuItems;
    if (menuLayer_ == MenuLayer::QUICK) return quickMenuItems;
    switch (moreSection_) {
      case MoreSection::TYPOGRAPHY: return typographyMenuItems;
      case MoreSection::TURNING: return turningMenuItems;
      case MoreSection::DISPLAY: return displayMenuItems;
      case MoreSection::CONTROL: return controlMenuItems;
      case MoreSection::DATA: return dataMenuItems;
      case MoreSection::ROOT:
      default: return moreRootItems;
    }
  }

  const char* getExternalFontName() const {
    if (SETTINGS.fontFamily == CrossPointSettings::FONT_CUSTOM && strlen(SETTINGS.customFontFamily) > 0) {
      return SETTINGS.customFontFamily;
    }
    return "";
  }

  const std::function<void(uint8_t)> onBack;
  const std::function<void(MenuAction)> onAction;
  MenuLayer initialLayer_ = MenuLayer::QUICK;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();
  void applyInternalAction(InternalAction action);
  std::string currentFontSizeLabel() const;
  std::string styleValueFor(InternalAction action) const;
  void notifyParentStyleChanged();
  void closeToReader();
  void prepareQuickFontFamilies();
  void applyQuickFontChoice(int slot);
  void showMoreRoot();
  const char* moreSectionKey() const;
  const char* moreSectionTitle() const;
};
