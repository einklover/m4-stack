#include "EpubReaderMenuActivity.h"

#include <EpdFontLoader.h>
#include <GfxRenderer.h>

#include "AutoPageTurnIntervalActivity.h"
#include "CrossPointSettings.h"
#include "I18n.h"
#include "MappedInputManager.h"
#include "activities/settings/FontSelectionActivity.h"
#include "activities/settings/SimpleBluetoothActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4ListTouchPolicy.h"
#include "util/M4UiText.h"
#include "util/TouchHitGeometry.h"
#include <BluetoothHIDManager.h>
#include <algorithm>

namespace {
// BaseTheme::drawProgressBar needs >4px height for a visible fill and also
// draws percentage text below the bar. Reserve a fixed 32px block so that text
// never overlaps the first 52px touch row while six quick actions still fit in
// Murphy M4 landscape mode.
constexpr int kQuickProgressBarHeight = 8;
constexpr int kQuickProgressBlockHeight = 32;
}  // namespace

void EpubReaderMenuActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();
  menuLayer_ = MenuLayer::QUICK;
  selectedIndex = 0;
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

  const auto& items = activeMenuItems();
  if (items.empty()) return;

  // Touch geometry intentionally mirrors renderScreen(). This also fixes the
  // old portrait-inverted mismatch where rendering used a 50px top gutter but
  // hit-testing did not, causing taps to activate the row above the visible one.
  if (mappedInput.hasTouch()) {
    if (mappedInput.wasBackGesture()) {
      if (returnToQuickMenu()) return;
      onBack(pendingOrientation);
      return;
    }

    const auto metrics = UITheme::getInstance().getMetrics();
    const int pageHeight = renderer.getScreenHeight();
    const bool isPortraitInverted =
        renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
    const int hintGutterHeight = isPortraitInverted ? 50 : 0;
    int listTop = hintGutterHeight + metrics.headerHeight + metrics.verticalSpacing;
    if (menuLayer_ == MenuLayer::QUICK) {
      listTop += kQuickProgressBlockHeight;
    }
    const int listHeight = pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
    const int totalItems = static_cast<int>(items.size());
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
    layout.selectedIndex = selectedIndex;

    int hit = -1;
    const auto act = M4ListTouchPolicy::resolveList(te, layout, hit);
    if (act == M4ListTouchPolicy::Action::PageDown || act == M4ListTouchPolicy::Action::PageUp) {
      selectedIndex = M4ListTouchPolicy::applyPage(
          selectedIndex, totalItems, pageItems, act == M4ListTouchPolicy::Action::PageDown);
      updateRequired = true;
      return;
    }
    if (act == M4ListTouchPolicy::Action::Select && hit >= 0) {
      if (selectedIndex != hit) {
        selectedIndex = hit;
        updateRequired = true;
      }
      return;
    }
    if (act == M4ListTouchPolicy::Action::Activate && hit >= 0) {
      selectedIndex = hit;
      goto activate_menu_item;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
      mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    selectedIndex = (selectedIndex + static_cast<int>(items.size()) - 1) % static_cast<int>(items.size());
    updateRequired = true;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
             mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    selectedIndex = (selectedIndex + 1) % static_cast<int>(items.size());
    updateRequired = true;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
  activate_menu_item:
    const auto selectedItem = items[static_cast<size_t>(selectedIndex)];

    // “更多” is internal navigation, not a reader callback. Keeping it inside
    // this Activity avoids destroying/recreating the reader just to expose the
    // legacy settings list.
    if (selectedItem.opensMore) {
      menuLayer_ = MenuLayer::MORE;
      selectedIndex = 0;
      updateRequired = true;
      return;
    }

    const auto selectedAction = selectedItem.action;
    if (selectedAction == MenuAction::ROTATE_SCREEN) {
      // Cycle orientation preview locally; actual rotation happens on menu exit.
      pendingOrientation = (pendingOrientation + 1) % orientationLabels.size();
      updateRequired = true;
      return;
    }

    if (selectedAction == MenuAction::AUTO_PAGE_TURN) {
      SETTINGS.autoPageTurnEnabled = SETTINGS.autoPageTurnEnabled ? 0 : 1;
      SETTINGS.saveToFile();
      updateRequired = true;
      return;
    }

    if (selectedAction == MenuAction::TOGGLE_ANTI_ALIAS) {
      SETTINGS.textAntiAliasing = SETTINGS.textAntiAliasing ? 0 : 1;
      SETTINGS.saveToFile();
      updateRequired = true;
      return;
    }

    if (selectedAction == MenuAction::TOGGLE_DARK_MODE) {
      SETTINGS.epubDarkMode = SETTINGS.epubDarkMode ? 0 : 1;
      SETTINGS.saveToFile();
      updateRequired = true;
      return;
    }

    if (selectedAction == MenuAction::TOGGLE_FONT) {
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
      SETTINGS.globalNextPageModeEnabled = SETTINGS.globalNextPageModeEnabled ? 0 : 1;
      SETTINGS.saveToFile();
      updateRequired = true;
      return;
    }

    if (selectedAction == MenuAction::PAGE_TURN_MODE) {
      SETTINGS.autoPageTurnMode = SETTINGS.autoPageTurnMode ? 0 : 1;
      SETTINGS.saveToFile();
      updateRequired = true;
      return;
    }

    if (selectedAction == MenuAction::PAGE_TURN_INTERVAL) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      enterNewActivity(new AutoPageTurnIntervalActivity(
          renderer, mappedInput, SETTINGS.autoPageTurnInterval,
          [this](const int interval) {
            SETTINGS.autoPageTurnInterval = interval;
            SETTINGS.saveToFile();
            exitActivity();
            updateRequired = true;
          },
          [this]() {
            exitActivity();
            updateRequired = true;
          }));
      xSemaphoreGive(renderingMutex);
      return;
    }

#ifdef CROSSPOINT_X3
    if (selectedAction == MenuAction::TILT_PAGE_TURN) {
      SETTINGS.tiltPageTurnEnabled = SETTINGS.tiltPageTurnEnabled ? 0 : 1;
      SETTINGS.saveToFile();
      updateRequired = true;
      return;
    }

    if (selectedAction == MenuAction::TILT_PAGE_TURN_SETTINGS) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      enterNewActivity(new TiltPageTurnSettingsActivity(
          renderer, mappedInput,
          [this]() {
            exitActivity();
            updateRequired = true;
          }));
      xSemaphoreGive(renderingMutex);
      return;
    }
#endif  // CROSSPOINT_X3

    if (selectedAction == MenuAction::LONG_PRESS_CONFIRM_MAPPING) {
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
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      enterNewActivity(new SimpleBluetoothActivity(
          renderer, mappedInput,
          [this]() {
            exitActivity();
            updateRequired = true;
          }));
      xSemaphoreGive(renderingMutex);
      return;
    }

    // The callback can delete this menu (reader swaps subactivities), so capture
    // it locally and return immediately after invoking it.
    auto actionCallback = onAction;
    actionCallback(selectedAction);
    return;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (returnToQuickMenu()) return;
    onBack(pendingOrientation);
    return;
  }
}

void EpubReaderMenuActivity::renderScreen() {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();
  const auto metrics = UITheme::getInstance().getMetrics();

  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;

  std::string headerTitle = title;
  if (menuLayer_ == MenuLayer::MORE) headerTitle += " · 更多";
  const std::string truncTitle =
      M4UiText::truncated(renderer, UI_12_FONT_ID, headerTitle.c_str(), contentWidth - 40, EpdFontFamily::BOLD);
  GUI.drawHeader(renderer, Rect{contentX, hintGutterHeight, contentWidth, metrics.headerHeight}, truncTitle.c_str());

  int listTop = hintGutterHeight + metrics.headerHeight + metrics.verticalSpacing;

  // The quick layer shows actual whole-book progress without opening another
  // screen. drawProgressBar also renders the percentage text below the bar, so
  // the whole block is reserved before list hit-testing begins.
  if (menuLayer_ == MenuLayer::QUICK) {
    const int progress = std::max(0, std::min(bookProgressPercent, 100));
    const int sidePadding = std::max(12, metrics.contentSidePadding);
    const int progressWidth = std::max(1, contentWidth - sidePadding * 2);
    GUI.drawProgressBar(renderer,
                        Rect{contentX + sidePadding, listTop, progressWidth, kQuickProgressBarHeight},
                        static_cast<size_t>(progress), static_cast<size_t>(100));
    listTop += kQuickProgressBlockHeight;
  }

  const int listHeight = pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const auto& items = activeMenuItems();
  const int totalItems = static_cast<int>(items.size());

  GUI.drawList(
      renderer, Rect{contentX, listTop, contentWidth, listHeight}, totalItems, selectedIndex,
      [&items](int index) -> std::string { return items[static_cast<size_t>(index)].label; },
      nullptr,
      nullptr,
      [this, &items](int index) -> std::string {
        const auto& item = items[static_cast<size_t>(index)];
        if (item.opensMore) return ">";

        const auto action = item.action;
        if (action == MenuAction::GO_TO_PERCENT) {
          return std::to_string(std::max(0, std::min(bookProgressPercent, 100))) + "%";
        }
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
          return strlen(fontName) > 0 ? std::string(fontName) : ">";
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
          const char* value = actionIdx < longPressConfirmLabels.size() ? longPressConfirmLabels[actionIdx] : "";
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
        return ">";
      });

  const char* backHint = menuLayer_ == MenuLayer::MORE ? "« 快捷" : "« 阅读";
  const auto labels = mappedInput.mapLabels(backHint, "选择", "向上", "向下");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (firstPaint_) {
    firstPaint_ = false;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}
