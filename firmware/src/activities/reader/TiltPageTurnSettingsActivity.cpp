#include "TiltPageTurnSettingsActivity.h"

#include <algorithm>
#include <GfxRenderer.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/settings/NumberSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4ListTouchPolicy.h"

namespace {
// Setting item indices (must match order in getSettingItems())
constexpr int IDX_TRIGGER_ANGLE = 0;
constexpr int IDX_RELEASE_ANGLE = 1;
constexpr int IDX_HOLD_TIME = 2;
constexpr int IDX_COOLDOWN_TIME = 3;
constexpr int IDX_LEFT_ACTION = 4;
constexpr int IDX_RIGHT_ACTION = 5;
constexpr int IDX_LARGE_LEFT_ACTION = 6;
constexpr int IDX_LARGE_RIGHT_ACTION = 7;
}  // namespace

std::vector<TiltPageTurnSettingsActivity::TiltSettingItem> TiltPageTurnSettingsActivity::getSettingItems() const {
  return {
      {"晃动角度", SettingItemType::NUMBER, 1, 90, 1, 5, "°", {}},
      {"回正角度", SettingItemType::NUMBER, 0, 89, 1, 5, "°", {}},
      {"倾斜持续时间", SettingItemType::NUMBER, 50, 2000, 50, 200, "ms", {}},
      {"冷却时间", SettingItemType::NUMBER, 100, 5000, 100, 500, "ms", {}},
      {"向左倾斜", SettingItemType::ENUM, 0, 1, 0, 0, "", {"上一页", "下一页"}},
      {"向右倾斜", SettingItemType::ENUM, 0, 1, 0, 0, "", {"上一页", "下一页"}},
      {"大幅度向左(>70°)", SettingItemType::ENUM, 0, 3, 0, 0, "", {"上一页", "下一页", "自动翻页", "打开菜单"}},
      {"大幅度向右(>70°)", SettingItemType::ENUM, 0, 3, 0, 0, "", {"上一页", "下一页", "自动翻页", "打开菜单"}},
  };
}

int TiltPageTurnSettingsActivity::getCurrentValue(const int index) const {
  switch (index) {
    case IDX_TRIGGER_ANGLE:
      return SETTINGS.tiltTriggerAngle;
    case IDX_RELEASE_ANGLE:
      return SETTINGS.tiltReleaseAngle;
    case IDX_HOLD_TIME:
      return SETTINGS.tiltHoldTimeMs;
    case IDX_COOLDOWN_TIME:
      return SETTINGS.tiltCooldownTimeMs;
    case IDX_LEFT_ACTION:
      return SETTINGS.tiltLeftAction;
    case IDX_RIGHT_ACTION:
      return SETTINGS.tiltRightAction;
    case IDX_LARGE_LEFT_ACTION:
      return SETTINGS.tiltLargeLeftAction;
    case IDX_LARGE_RIGHT_ACTION:
      return SETTINGS.tiltLargeRightAction;
    default:
      return 0;
  }
}

std::string TiltPageTurnSettingsActivity::getValueDisplay(const int index) const {
  const auto items = getSettingItems();
  if (index < 0 || index >= static_cast<int>(items.size())) return "";

  const auto& item = items[index];
  const int value = getCurrentValue(index);

  if (item.type == SettingItemType::ENUM) {
    if (value >= 0 && value < static_cast<int>(item.enumLabels.size())) {
      return item.enumLabels[value];
    }
    return "?";
  }

  // NUMBER type
  return std::to_string(value) + item.unit;
}

void TiltPageTurnSettingsActivity::toggleCurrentSetting() {
  const auto items = getSettingItems();
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(items.size())) return;

  const auto& item = items[selectedIndex];

  if (item.type == SettingItemType::ENUM) {
    // Cycle enum value
    const int current = getCurrentValue(selectedIndex);
    const int next = (current + 1) % static_cast<int>(item.enumLabels.size());
    switch (selectedIndex) {
      case IDX_LEFT_ACTION:
        SETTINGS.tiltLeftAction = next;
        break;
      case IDX_RIGHT_ACTION:
        SETTINGS.tiltRightAction = next;
        break;
      case IDX_LARGE_LEFT_ACTION:
        SETTINGS.tiltLargeLeftAction = next;
        break;
      case IDX_LARGE_RIGHT_ACTION:
        SETTINGS.tiltLargeRightAction = next;
        break;
    }
    SETTINGS.saveToFile();
    updateRequired = true;
  } else if (item.type == SettingItemType::NUMBER) {
    openNumberSelector(selectedIndex);
  }
}

void TiltPageTurnSettingsActivity::openNumberSelector(const int index) {
  const auto items = getSettingItems();
  if (index < 0 || index >= static_cast<int>(items.size())) return;

  const auto& item = items[index];
  const int currentValue = getCurrentValue(index);

  NumberSelectionActivity::Config config;
  config.title = item.label;
  config.unit = item.unit;
  config.minValue = item.minValue;
  config.maxValue = item.maxValue;
  config.smallStep = item.smallStep;
  config.largeStep = item.largeStep;

  const int settingIndex = index;
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  enterNewActivity(new NumberSelectionActivity(
      renderer, mappedInput, config, currentValue,
      [this, settingIndex](int value) {
        switch (settingIndex) {
          case IDX_TRIGGER_ANGLE:
            SETTINGS.tiltTriggerAngle = static_cast<uint8_t>(value);
            // Auto-adjust release angle to be trigger - 1 if it's >= trigger
            if (SETTINGS.tiltReleaseAngle >= SETTINGS.tiltTriggerAngle) {
              SETTINGS.tiltReleaseAngle = SETTINGS.tiltTriggerAngle > 0 ? SETTINGS.tiltTriggerAngle - 1 : 0;
            }
            break;
          case IDX_RELEASE_ANGLE:
            SETTINGS.tiltReleaseAngle = static_cast<uint8_t>(value);
            break;
          case IDX_HOLD_TIME:
            SETTINGS.tiltHoldTimeMs = static_cast<uint16_t>(value);
            break;
          case IDX_COOLDOWN_TIME:
            SETTINGS.tiltCooldownTimeMs = static_cast<uint16_t>(value);
            break;
        }
        SETTINGS.saveToFile();
        exitActivity();
        updateRequired = true;
      },
      [this]() {
        exitActivity();
        updateRequired = true;
      }));
  xSemaphoreGive(renderingMutex);
}

void TiltPageTurnSettingsActivity::onEnter() {
  Activity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();
  selectedIndex = 0;
  updateRequired = true;
  xTaskCreate(&TiltPageTurnSettingsActivity::taskTrampoline, "TiltPTSet", 4096, this, 1, &displayTaskHandle);
}

void TiltPageTurnSettingsActivity::onExit() {
  ActivityWithSubactivity::onExit();
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void TiltPageTurnSettingsActivity::taskTrampoline(void* param) {
  auto* self = static_cast<TiltPageTurnSettingsActivity*>(param);
  self->displayTaskLoop();
}

[[noreturn]] void TiltPageTurnSettingsActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired && !subActivity) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      renderScreen();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void TiltPageTurnSettingsActivity::loop() {
  if (subActivity) {
    pumpSubActivityFrame();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      (mappedInput.hasTouch() && mappedInput.wasBackGesture())) {
    onGoBack();
    return;
  }

  const auto items = getSettingItems();
  const int count = static_cast<int>(items.size());

  if (mappedInput.hasTouch() && count > 0) {
    auto metrics = UITheme::getInstance().getMetrics();
    const int pageHeight = renderer.getScreenHeight();
    const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int listHeight = pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
    M4ListTouchPolicy::Event te{};
    const auto sw = mappedInput.wasSwipe();
    if (sw == MappedInputManager::SwipeDir::Up) te.swipe = M4ListTouchPolicy::Swipe::Up;
    else if (sw == MappedInputManager::SwipeDir::Down) te.swipe = M4ListTouchPolicy::Swipe::Down;
    int dx = 0, dy = 0, tx = 0, ty = 0;
    te = M4ListTouchPolicy::mergeFrame(false, te.swipe, mappedInput.wasScreenTouchDown(dx, dy), dx, dy,
                                       mappedInput.wasScreenTapped(tx, ty), tx, ty);
    M4ListTouchPolicy::ListLayout layout;
    layout.listTop = listTop;
    layout.listHeight = listHeight;
    layout.rowStep = metrics.listRowHeight;
    layout.itemCount = count;
    layout.selectedIndex = selectedIndex;
    const int pageItems = std::max(1, listHeight / metrics.listRowHeight);
    int hit = -1;
    const auto act = M4ListTouchPolicy::resolveList(te, layout, hit);
    if (act == M4ListTouchPolicy::Action::PageDown || act == M4ListTouchPolicy::Action::PageUp) {
      selectedIndex = M4ListTouchPolicy::applyPage(selectedIndex, count, pageItems,
                                                   act == M4ListTouchPolicy::Action::PageDown);
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
      toggleCurrentSetting();
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    toggleCurrentSetting();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
      mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    selectedIndex = (selectedIndex + count - 1) % count;
    updateRequired = true;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
             mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    selectedIndex = (selectedIndex + 1) % count;
    updateRequired = true;
  }
}

void TiltPageTurnSettingsActivity::renderScreen() {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "晃动翻页设置");

  const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int listHeight = pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  const auto items = getSettingItems();
  const int count = static_cast<int>(items.size());

  GUI.drawList(
      renderer, Rect{0, listTop, pageWidth, listHeight}, count, selectedIndex,
      [&items](int index) { return std::string(items[index].label); },
      nullptr, nullptr,
      [this](int i) { return getValueDisplay(i); });

  const auto labels = mappedInput.mapLabels("« 返回", "切换", "向上", "向下");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
