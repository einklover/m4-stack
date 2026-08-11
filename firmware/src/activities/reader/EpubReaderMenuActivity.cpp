#include "EpubReaderMenuActivity.h"

#include <EpdFontLoader.h>
#include <GfxRenderer.h>

#include "AutoPageTurnIntervalActivity.h"
#include "CrossPointSettings.h"
#include "I18n.h"
#include "MappedInputManager.h"
#include "activities/settings/SimpleBluetoothActivity.h"
#include "activities/settings/FontSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4UiText.h"
#include "util/M4ListTouchPolicy.h"
#include "util/TouchHitGeometry.h"
#include <BluetoothHIDManager.h>
#include <algorithm>

void EpubReaderMenuActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();
  firstPaint_ = true;
  updateRequired = true;

  xTaskCreate(&EpubReaderMenuActivity::taskTrampoline, "EpubMenuTask", 4096, this, 1, &displayTaskHandle);
}

void EpubReaderMenuActivity::onExit() {
  ActivityWithSubactivity::onExit();
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void EpubReaderMenuActivity::taskTrampoline(void* param) {
  auto* self = static_cast<EpubReaderMenuActivity*>(param);
  self->displayTaskLoop();
}

void EpubReaderMenuActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired && !subActivity) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      std::string popup = std::move(pendingPopup_);
      renderScreen();
      if (!popup.empty()) GUI.drawPopup(renderer, popup.c_str());
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void EpubReaderMenuActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  // Touch: list hit-test + swipe page matches renderScreen() list rect.
  if (mappedInput.hasTouch()) {
    if (mappedInput.wasBackGesture()) {
      onBack(pendingOrientation);
      return;
    }
    auto metrics = UITheme::getInstance().getMetrics();
    const int pageHeight = renderer.getScreenHeight();
    const int listTop = metrics.headerHeight + metrics.verticalSpacing;
    const int listHeight = pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
    const int totalItems = static_cast<int>(menuItems.size());
    const int pageItems = std::max(1, listHeight / metrics.listRowHeight);

    M4ListTouchPolicy::Event te{};
    const auto sw = mappedInput.wasSwipe();
    if (sw == MappedInputManager::SwipeDir::Up) te.swipe = M4ListTouchPolicy::Swipe::Up;
    else if (sw == MappedInputManager::SwipeDir::Down) te.swipe = M4ListTouchPolicy::Swipe::Down;
    else if (sw == MappedInputManager::SwipeDir::Left) te.swipe = M4ListTouchPolicy::Swipe::Left;
    else if (sw == MappedInputManager::SwipeDir::Right) te.swipe = M4ListTouchPolicy::Swipe::Right;
    int dx = 0, dy = 0, tx = 0, ty = 0;
    te = M4ListTouchPolicy::mergeFrame(false, te.swipe, mappedInput.wasScreenTouchDown(dx, dy), dx, dy,
                                       mappedInput.wasScreenTapped(tx, ty), tx, ty);
    M4ListTouchPolicy::ListLayout layout;
    layout.listTop = listTop;
    layout.listHeight = listHeight;
    layout.rowStep = metrics.listRowHeight;
    layout.itemCount = totalItems;
    layout.selectedIndex = static_cast<int>(selectedIndex);
    int hit = -1;
    const auto act = M4ListTouchPolicy::resolveList(te, layout, hit);
    if (act == M4ListTouchPolicy::Action::PageDown || act == M4ListTouchPolicy::Action::PageUp) {
      if (totalItems > 0) {
        selectedIndex = static_cast<size_t>(M4ListTouchPolicy::applyPage(
            static_cast<int>(selectedIndex), totalItems, pageItems,
            act == M4ListTouchPolicy::Action::PageDown));
        updateRequired = true;
      }
      return;
    }
    if (act == M4ListTouchPolicy::Action::Select && hit >= 0) {
      if (static_cast<int>(selectedIndex) != hit) {
        selectedIndex = static_cast<size_t>(hit);
        updateRequired = true;
      }
      return;
    }
    if (act == M4ListTouchPolicy::Action::Activate && hit >= 0) {
      selectedIndex = static_cast<size_t>(hit);
      goto activate_menu_item;
    }
  }

  // Use local variables for items we need to check after potential deletion
  if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
      mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    selectedIndex = (selectedIndex + menuItems.size() - 1) % menuItems.size();
    updateRequired = true;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
             mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    selectedIndex = (selectedIndex + 1) % menuItems.size();
    updateRequired = true;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
  activate_menu_item:
    const auto selectedAction = menuItems[selectedIndex].action;
    if (selectedAction == MenuAction::ROTATE_SCREEN) {
      // Cycle orientation preview locally; actual rotation happens on menu exit.
      pendingOrientation = (pendingOrientation + 1) % orientationLabels.size();
      updateRequired = true;
      return;
    }

    if (selectedAction == MenuAction::AUTO_PAGE_TURN) {
      // Toggle auto page turn on/off and save to settings
      SETTINGS.autoPageTurnEnabled = SETTINGS.autoPageTurnEnabled ? 0 : 1;
      SETTINGS.saveToFile();
      updateRequired = true;
      return;
    }

    if (selectedAction == MenuAction::TOGGLE_ANTI_ALIAS) {
      // Toggle anti-aliasing on/off and save to settings
      SETTINGS.textAntiAliasing = SETTINGS.textAntiAliasing ? 0 : 1;
      SETTINGS.saveToFile();
      updateRequired = true;
      return;
    }

    if (selectedAction == MenuAction::TOGGLE_DARK_MODE) {
      // Toggle dark mode on/off and save to settings
      SETTINGS.epubDarkMode = SETTINGS.epubDarkMode ? 0 : 1;
      SETTINGS.saveToFile();
      updateRequired = true;
      return;
    }

    if (selectedAction == MenuAction::TOGGLE_FONT) {
      // 切换系统字体/外置字体
      if (SETTINGS.fontFamily == CrossPointSettings::SYSTEM_FONT) {
        SETTINGS.fontFamily = CrossPointSettings::FONT_CUSTOM;
      } else {
        SETTINGS.fontFamily = CrossPointSettings::SYSTEM_FONT;
      }
      SETTINGS.saveToFile();
      updateRequired = true;
      return;
    }

    if (selectedAction == MenuAction::SELECT_EXTERNAL_FONT) {
      // 打开外置字体选择界面
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      enterNewActivity(new FontSelectionActivity(renderer, mappedInput, [this](bool loaded) {
        exitActivity();
        if (!loaded) {
          xSemaphoreTake(renderingMutex, portMAX_DELAY);
          pendingPopup_ = L(Str::kFontLoadFailed);
          xSemaphoreGive(renderingMutex);
        }
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
      return;
    }

    if (selectedAction == MenuAction::TOGGLE_GLOBAL_NEXT_PAGE) {
      // Toggle global next page mode on/off and save to settings
      SETTINGS.globalNextPageModeEnabled = SETTINGS.globalNextPageModeEnabled ? 0 : 1;
      SETTINGS.saveToFile();
      updateRequired = true;
      return;
    }

    if (selectedAction == MenuAction::PAGE_TURN_MODE) {
      // Toggle page turn mode (full screen / rolling half) and save to settings
      SETTINGS.autoPageTurnMode = SETTINGS.autoPageTurnMode ? 0 : 1;
      SETTINGS.saveToFile();
      updateRequired = true;
      return;
    }

    if (selectedAction == MenuAction::PAGE_TURN_INTERVAL) {
      // Open interval selector subactivity
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      enterNewActivity(new AutoPageTurnIntervalActivity(
          renderer, mappedInput, SETTINGS.autoPageTurnInterval,
          [this](const int interval) {
            // On select: save interval and return
            SETTINGS.autoPageTurnInterval = interval;
            SETTINGS.saveToFile();
            exitActivity();
            updateRequired = true;
          },
          [this]() {
            // On cancel: just return
            exitActivity();
            updateRequired = true;
          }));
      xSemaphoreGive(renderingMutex);
      return;
    }

#ifdef CROSSPOINT_X3
    if (selectedAction == MenuAction::TILT_PAGE_TURN) {
      // Toggle tilt page turn on/off and save to settings
      SETTINGS.tiltPageTurnEnabled = SETTINGS.tiltPageTurnEnabled ? 0 : 1;
      SETTINGS.saveToFile();
      updateRequired = true;
      return;
    }

    if (selectedAction == MenuAction::TILT_PAGE_TURN_SETTINGS) {
      // Open tilt page turn settings subactivity
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      enterNewActivity(new TiltPageTurnSettingsActivity(
          renderer, mappedInput,
          [this]() {
            // On back: just return to menu
            exitActivity();
            updateRequired = true;
          }));
      xSemaphoreGive(renderingMutex);
      return;
    }
#endif  // CROSSPOINT_X3

    if (selectedAction == MenuAction::LONG_PRESS_CONFIRM_MAPPING) {
      // 循环切换长按确认键功能映射
#ifdef CROSSPOINT_X3
      constexpr uint8_t maxAction = 6;
#else
      constexpr uint8_t maxAction = 5;
#endif
      SETTINGS.longPressConfirmAction = (SETTINGS.longPressConfirmAction + 1) % (maxAction + 1);
      SETTINGS.saveToFile();
      updateRequired = true;
      return;
    }

    if (selectedAction == MenuAction::BLUETOOTH_SETTINGS) {
      // Open Bluetooth settings subactivity (使用新的SimpleBluetoothActivity)
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      enterNewActivity(new SimpleBluetoothActivity(
          renderer, mappedInput,
          [this]() {
            // On back: just return to menu
            exitActivity();
            updateRequired = true;
          }));
      xSemaphoreGive(renderingMutex);
      return;
    }

    // 1. Capture the callback and action locally
    auto actionCallback = onAction;

    // 2. Execute the callback
    actionCallback(selectedAction);

    // 3. CRITICAL: Return immediately. 'this' is likely deleted now.
    return;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Return the pending orientation to the parent so it can apply on exit.
    onBack(pendingOrientation);
    return;  // Also return here just in case
  }
}

void EpubReaderMenuActivity::renderScreen() {
  renderer.clearScreen();
  
  // 关键修复：使用统一的主题组件，与“字距、边距、下划线设置”保持一致
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();
  auto metrics = UITheme::getInstance().getMetrics();
  
  // 横屏适配：计算内容区域
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  
  // 使用统一的Header组件
  const std::string truncTitle =
      M4UiText::truncated(renderer, UI_12_FONT_ID, title.c_str(), contentWidth - 40, EpdFontFamily::BOLD);
  GUI.drawHeader(renderer, Rect{contentX, hintGutterHeight, contentWidth, metrics.headerHeight}, truncTitle.c_str());
  
  // 菜单列表区域（直接放在Header下方）
  const int listTop = hintGutterHeight + metrics.headerHeight + metrics.verticalSpacing;
  const int listHeight = pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int totalItems = static_cast<int>(menuItems.size());
  
  // 关键修复：使用统一的GUI.drawList()组件
  GUI.drawList(
      renderer, Rect{contentX, listTop, contentWidth, listHeight},
      totalItems, selectedIndex,
      [this](int index) -> std::string {
        // 左侧：菜单项名称
        return menuItems[index].label;
      },
      nullptr,  // 不需要副标题
      nullptr,  // 不需要图标
      [this](int index) -> std::string {
        // 右侧：当前值或子菜单指示符
        const auto action = menuItems[index].action;
        
        if (action == MenuAction::ROTATE_SCREEN) {
          return std::string(orientationLabels[pendingOrientation]);
        }
        if (action == MenuAction::AUTO_PAGE_TURN) {
          return std::string(autoPageTurnLabels[SETTINGS.autoPageTurnEnabled ? 1 : 0]);
        }
        if (action == MenuAction::TOGGLE_ANTI_ALIAS) {
          return std::string(antiAliasLabels[SETTINGS.textAntiAliasing ? 1 : 0]);
        }
        if (action == MenuAction::TOGGLE_DARK_MODE) {
          return std::string(darkModeLabels[SETTINGS.epubDarkMode ? 1 : 0]);
        }
        if (action == MenuAction::TOGGLE_FONT) {
          return std::string(fontLabels[SETTINGS.fontFamily == CrossPointSettings::FONT_CUSTOM ? 1 : 0]);
        }
        if (action == MenuAction::SELECT_EXTERNAL_FONT) {
          const char* fontName = getExternalFontName();
          return strlen(fontName) > 0 ? std::string(fontName) : ">";  // 没有字体名时显示子菜单指示符
        }
        if (action == MenuAction::TOGGLE_GLOBAL_NEXT_PAGE) {
          return std::string(globalNextPageLabels[SETTINGS.globalNextPageModeEnabled ? 1 : 0]);
        }
#ifdef CROSSPOINT_X3
        if (action == MenuAction::TILT_PAGE_TURN) {
          return std::string(tiltPageTurnLabels[SETTINGS.tiltPageTurnEnabled ? 1 : 0]);
        }
#endif
        if (action == MenuAction::PAGE_TURN_MODE) {
          return std::string(pageTurnModeLabels[SETTINGS.autoPageTurnMode ? 1 : 0]);
        }
        if (action == MenuAction::PAGE_TURN_INTERVAL) {
          return std::to_string(SETTINGS.autoPageTurnInterval) + "秒";
        }
        if (action == MenuAction::LONG_PRESS_CONFIRM_MAPPING) {
          const uint8_t actionIdx = SETTINGS.longPressConfirmAction;
          const char* value = (actionIdx < longPressConfirmLabels.size()) ? longPressConfirmLabels[actionIdx] : "";
          return std::string(value);
        }
        if (action == MenuAction::BLUETOOTH_SETTINGS) {
          try {
            auto& btMgr = BluetoothHIDManager::getInstance();
            return btMgr.isEnabled() ? "已开启" : "已关闭";
          } catch (...) {
            return "错误";
          }
        }
        // 关键修复：没有值的菜单项显示">"表示有子菜单
        return ">";
      });
  
  // 按键提示
  const auto labels = mappedInput.mapLabels("« 返回", "选择", "向上", "向下");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (firstPaint_) {
    firstPaint_ = false;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}

// drawScrollBar已删除，GUI.drawList()自动处理滚动条
