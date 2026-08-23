#include "EpubReaderSettingsActivity.h"

#include <algorithm>
#include <EpdFontLoader.h>
#include <GfxRenderer.h>

#include "CrossPointSettings.h"
#include "LanguageMapper.h"
#include "MappedInputManager.h"
#include "SettingsLists.h"
#include "activities/settings/NumberSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4ListTouchPolicy.h"

EpubReaderSettingsActivity::EpubReaderSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                       const std::function<void()>& onGoBack)
    : ActivityWithSubactivity("EpubReaderSettings", renderer, mappedInput), onGoBack(onGoBack) {}

void EpubReaderSettingsActivity::onEnter() {
  Activity::onEnter();
  firstPaint_ = true;
  touchHandoffFrames_ = 1;
  renderingMutex = xSemaphoreCreateMutex();

  // Load only "Reader" category settings
  settings.clear();
  for (auto& s : getSettingsList()) {
    if (s.category && strcmp(s.category, "Reader") == 0) {
      settings.push_back(std::move(s));
    }
  }

  selectedIndex = 0;
  updateRequired = true;
  xTaskCreate(&EpubReaderSettingsActivity::taskTrampoline, "EpubRdrSet", 8192, this, 1, &displayTaskHandle);
}

void EpubReaderSettingsActivity::onExit() {
  ActivityWithSubactivity::onExit();
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void EpubReaderSettingsActivity::taskTrampoline(void* param) {
  auto* self = static_cast<EpubReaderSettingsActivity*>(param);
  self->displayTaskLoop();
}

[[noreturn]] void EpubReaderSettingsActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired && !subActivity) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void EpubReaderSettingsActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) {
    SETTINGS.saveToFile();
    EpdFontLoader::loadFontsFromSd(renderer);
    onGoBack();
    return;
  }

  const int count = static_cast<int>(settings.size());

  // Touch: same list geometry as render() drawList
  const bool ignoreTouchHandoff = touchHandoffFrames_ > 0;
  if (ignoreTouchHandoff) --touchHandoffFrames_;
  if (!ignoreTouchHandoff && mappedInput.hasTouch() && count > 0) {
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
      // Swipe scrolls by one page so options beyond the first screen stay reachable.
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
      updateRequired = true;
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    toggleCurrentSetting();
    updateRequired = true;
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

void EpubReaderSettingsActivity::toggleCurrentSetting() {
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(settings.size())) return;
  const auto& setting = settings[selectedIndex];

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    const bool currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentValue;
    SETTINGS.saveToFile();
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    const uint8_t currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = (currentValue + 1) % static_cast<uint8_t>(setting.enumValues.size());
    SETTINGS.saveToFile();
  } else if (setting.type == SettingType::ENUM && setting.valueGetter && setting.valueSetter) {
    const uint8_t currentIndex = setting.valueGetter();
    setting.valueSetter((currentIndex + 1) % static_cast<uint8_t>(setting.enumValues.size()));
  } else if (setting.type == SettingType::VALUE && setting.signedValuePtr != nullptr) {
    // 有符号数值类型：打开数字选择器
    const int currentValue = SETTINGS.*(setting.signedValuePtr);
    NumberSelectionActivity::Config config;
    config.title = getChineseName(setting.name);
    config.minValue = setting.valueRange.min;
    config.maxValue = setting.valueRange.max;
    config.smallStep = setting.valueRange.step;
    config.largeStep = setting.valueRange.step * 5;
    config.isSigned = true;
    
    auto signedPtr = setting.signedValuePtr;
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    exitActivity();
    enterNewActivity(new NumberSelectionActivity(
        renderer, mappedInput, config, currentValue,
        [this, signedPtr](int value) {
          SETTINGS.*(signedPtr) = static_cast<int8_t>(value);
          SETTINGS.saveToFile();
          exitActivity();
          updateRequired = true;
        },
        [this]() {
          exitActivity();
          updateRequired = true;
        }));
    xSemaphoreGive(renderingMutex);
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    // 无符号数值类型：打开数字选择器
    const int currentValue = SETTINGS.*(setting.valuePtr);
    NumberSelectionActivity::Config config;
    config.title = getChineseName(setting.name);
    config.minValue = setting.valueRange.min;
    config.maxValue = setting.valueRange.max;
    config.smallStep = setting.valueRange.step;
    config.largeStep = setting.valueRange.step * 5;
    config.isSigned = false;
    // 行间距特殊格式化：5-15 -> 0.5倍-1.5倍
    if (strcmp(setting.name, "行间距") == 0) {
      config.displayFormatter = [](int v) -> std::string {
        return std::to_string(v / 10) + "." + std::to_string(v % 10) + "倍";
      };
    }
    
    auto valuePtr = setting.valuePtr;
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    exitActivity();
    enterNewActivity(new NumberSelectionActivity(
        renderer, mappedInput, config, currentValue,
        [this, valuePtr](int value) {
          SETTINGS.*(valuePtr) = static_cast<uint8_t>(value);
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
}

void EpubReaderSettingsActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "阅读设置");

  const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int listHeight = pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int count = static_cast<int>(settings.size());

  GUI.drawList(
      renderer, Rect{0, listTop, pageWidth, listHeight}, count, selectedIndex,
      [this](int index) { return std::string(getChineseName(settings[index].name)); },
      nullptr, nullptr,
      [this](int i) {
        std::string valueText;
        if (settings[i].type == SettingType::TOGGLE && settings[i].valuePtr != nullptr) {
          valueText = SETTINGS.*(settings[i].valuePtr) ? "开启" : "关闭";
        } else if (settings[i].type == SettingType::ENUM && settings[i].valuePtr != nullptr) {
          const uint8_t value = SETTINGS.*(settings[i].valuePtr);
          valueText = getChineseName(settings[i].enumValues[value].c_str());
        } else if (settings[i].type == SettingType::ENUM && settings[i].valueGetter) {
          const uint8_t idx = settings[i].valueGetter();
          if (idx < static_cast<uint8_t>(settings[i].enumValues.size())) {
            valueText = getChineseName(settings[i].enumValues[idx].c_str());
          }
        } else if (settings[i].type == SettingType::VALUE && settings[i].signedValuePtr != nullptr) {
          valueText = std::to_string(static_cast<int>(SETTINGS.*(settings[i].signedValuePtr)));
        } else if (settings[i].type == SettingType::VALUE && settings[i].valuePtr != nullptr) {
          const int v = SETTINGS.*(settings[i].valuePtr);
          if (strcmp(settings[i].name, "行间距") == 0) {
            valueText = std::to_string(v / 10) + "." + std::to_string(v % 10) + "倍";
          } else {
            valueText = std::to_string(v);
          }
        }
        return valueText;
      });

  const auto labels = mappedInput.mapLabels("« 返回", "切换", "向上", "向下");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (firstPaint_) {
    firstPaint_ = false;
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}
