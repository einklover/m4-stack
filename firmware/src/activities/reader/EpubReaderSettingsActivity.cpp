#include "EpubReaderSettingsActivity.h"

#include <algorithm>
#include <EpdFontLoader.h>
#include <GfxRenderer.h>
#include <HalPowerManager.h>

#include "CrossPointSettings.h"
#include "LanguageMapper.h"
#include "MappedInputManager.h"
#include "SettingsLists.h"
#include "activities/reader/M4ReaderSettingsCatalog.h"
#include "activities/settings/FontSelectionActivity.h"
#include "activities/settings/NumberSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4ListTouchPolicy.h"
#include "util/M4UiText.h"

EpubReaderSettingsActivity::EpubReaderSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                       const std::function<void()>& onGoBack)
    : ActivityWithSubactivity("EpubReaderSettings", renderer, mappedInput), onGoBack(onGoBack) {}

void EpubReaderSettingsActivity::onEnter() {
  Activity::onEnter();
  firstPaint_ = true;
  touchHandoffFrames_ = 2;
  renderingMutex = xSemaphoreCreateMutex();

  // Unique full 阅读设置 page. Scope is process-global 全部书籍.
  // Filter uses m4ReaderSettingsPageIncludesKey (drops system UI size,
  // decode quality, and LUT waveform knobs). User page-turn on/dir stay.
  settings.clear();
  settings.push_back(SettingInfo::DynamicEnum(
      kM4ReaderSettingsScopeTitle, {kM4ReaderSettingsScopeCopy}, []() -> uint8_t { return 0; },
      [](uint8_t) {}, kM4ReaderSettingsScopeKey, "Reader"));
  SettingInfo fontPicker = SettingInfo::Action(kM4ReaderFontPickerLabel);
  fontPicker.key = kM4ReaderFontPickerKey;
  fontPicker.category = "Reader";
  settings.push_back(std::move(fontPicker));
  for (auto& s : getSettingsList()) {
    if (!(s.category && strcmp(s.category, "Reader") == 0)) continue;
    if (!m4ReaderSettingsPageIncludesKey(s.key)) continue;
    if (const char* label = m4ReaderSettingsPageLabelForKey(s.key)) s.name = label;
    settings.push_back(std::move(s));
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

namespace {

constexpr int kReaderRowH = 58;
constexpr int kReaderWinTop = 130;
constexpr int kReaderCardX = 8;
constexpr int kReaderCardW = 464;

}  // namespace

void EpubReaderSettingsActivity::loop() {
  if (subActivity) {
    // Deferred pump: never destroy a nested picker while its loop is on the stack.
    pumpSubActivityFrame();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) {
    SETTINGS.saveToFile();
    // Parent reloads after this settings display task has exited.
    onGoBack();
    return;
  }

  const int count = static_cast<int>(settings.size());

  // Touch: same list geometry as render() drawList
  if (touchHandoffFrames_ > 0) {
    --touchHandoffFrames_;
    // Drain edge-triggered touch from the opener frame. Ignoring without
    // consuming leaves the page-turn/menu tap pending for the next loop and
    // can activate the first settings row immediately.
    int dx = 0, dy = 0, tx = 0, ty = 0;
    (void)mappedInput.wasSwipe();
    (void)mappedInput.wasScreenTouchDown(dx, dy);
    (void)mappedInput.wasScreenTapped(tx, ty);
  } else if (mappedInput.hasTouch() && count > 0) {
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
    // Table-driven hit on the v4 canvas (same window as paint below); gesture
    // dispatch order (Back, swipe pages, tap=Activate, down=Select) mirrors
    // M4ListTouchPolicy::resolveList exactly — only the y-mapping changed so
    // touch keeps matching displayed rows. Index/paging/input semantics same.
    const int pageItems = std::max(1, listHeight / kReaderRowH);
    const int pageStart = count == 0 ? 0 : (selectedIndex / pageItems) * pageItems;
    int hit = -1;
    for (int i = pageStart; i < count && i < pageStart + pageItems; ++i) {
      const int y = kReaderWinTop + (i - pageStart) * kReaderRowH;
      if (y >= listTop + listHeight) break;
      if (te.y >= y && te.y < y + kReaderRowH) {
        hit = i;
        break;
      }
    }
    M4ListTouchPolicy::Action act = M4ListTouchPolicy::Action::None;
    if (te.backGesture) {
      act = M4ListTouchPolicy::Action::Back;
    } else if (te.swipe == M4ListTouchPolicy::Swipe::Up || te.swipe == M4ListTouchPolicy::Swipe::Left) {
      act = M4ListTouchPolicy::Action::PageDown;
    } else if (te.swipe == M4ListTouchPolicy::Swipe::Down ||
               te.swipe == M4ListTouchPolicy::Swipe::Right) {
      act = M4ListTouchPolicy::Action::PageUp;
    } else if (te.tap) {
      if (hit >= 0) act = M4ListTouchPolicy::Action::Activate;
    } else if (te.touchDown) {
      if (hit >= 0) act = M4ListTouchPolicy::Action::Select;
    }
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

  if (setting.type == SettingType::ACTION && setting.key &&
      strcmp(setting.key, kM4ReaderFontPickerKey) == 0) {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    exitActivity();
    enterNewActivity(new FontSelectionActivity(renderer, mappedInput, [this](bool) {
      exitActivity();
      updateRequired = true;
    }, FontSelectionActivity::Target::Reader, true));
    xSemaphoreGive(renderingMutex);
    return;
  }

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

namespace {

std::string readerSettingValueText(const SettingInfo& s) {
  std::string valueText;
  if (s.type == SettingType::TOGGLE && s.valuePtr != nullptr) {
    valueText = SETTINGS.*(s.valuePtr) ? "开启" : "关闭";
  } else if (s.type == SettingType::ENUM && s.valuePtr != nullptr) {
    const uint8_t value = SETTINGS.*(s.valuePtr);
    valueText = getChineseName(s.enumValues[value].c_str());
  } else if (s.type == SettingType::ENUM && s.valueGetter) {
    const uint8_t idx = s.valueGetter();
    if (idx < static_cast<uint8_t>(s.enumValues.size())) {
      valueText = getChineseName(s.enumValues[idx].c_str());
    }
  } else if (s.type == SettingType::VALUE && s.signedValuePtr != nullptr) {
    valueText = std::to_string(static_cast<int>(SETTINGS.*(s.signedValuePtr)));
  } else if (s.type == SettingType::VALUE && s.valuePtr != nullptr) {
    const int v = SETTINGS.*(s.valuePtr);
    if (strcmp(s.name, "行间距") == 0) {
      valueText = std::to_string(v / 10) + "." + std::to_string(v % 10) + "倍";
    } else {
      valueText = std::to_string(v);
    }
  } else if (s.type == SettingType::ACTION && s.key &&
             strcmp(s.key, kM4ReaderFontPickerKey) == 0) {
    if (SETTINGS.fontFamily == CrossPointSettings::FONT_CUSTOM && SETTINGS.customFontFamily[0]) {
      valueText = SETTINGS.customFontFamily;
    } else {
      valueText = "系统";
    }
  }
  return valueText;
}

// Rows that push a sub-page (font picker, number editors) carry a chevron;
// toggles and in-place enums show only their value.
bool readerSettingNavigates(const SettingInfo& s) {
  return s.type == SettingType::ACTION || s.type == SettingType::VALUE;
}

}  // namespace

void EpubReaderSettingsActivity::render() const {
  renderer.clearScreen();

  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = UITheme::getInstance().getMetrics();

  M4UiText::draw(renderer, NOTOSANS_14_FONT_ID, 24, 15, "Murphy M4", true);
  M4UiText::draw(renderer, NOTOSANS_18_FONT_ID, 24, 48, kM4ReaderSettingsTitle, true, EpdFontFamily::BOLD);
  const int readerBatt = powerManager.getBatteryPercentage() > 100
                             ? 100
                             : static_cast<int>(powerManager.getBatteryPercentage());
  renderer.drawRect(431, 18, 22, 10, true);
  renderer.fillRect(433, 20, (18 * readerBatt) / 100, 6, true);
  renderer.fillRect(453, 21, 2, 4, true);
  renderer.drawLine(8, 91, 472, 91, 2, true);

  const int listHeight = pageHeight - kReaderWinTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int count = static_cast<int>(settings.size());
  const int pageItems = std::max(1, listHeight / kReaderRowH);
  const int pageStart = count == 0 ? 0 : (selectedIndex / pageItems) * pageItems;
  const int visible = std::min(pageItems, std::max(0, count - pageStart));
  renderer.drawRoundedRect(kReaderCardX, kReaderWinTop, kReaderCardW, visible * kReaderRowH, 2, 11, true);

  for (int i = pageStart; i < count && i < pageStart + pageItems; ++i) {
    const int y = kReaderWinTop + (i - pageStart) * kReaderRowH;
    const bool selected = (i == selectedIndex);
    if (selected) {
      renderer.fillRectStipple(kReaderCardX + 2, y + 4, 460, 50);
    }
    const std::string title = getChineseName(settings[i].name);
    const std::string value = readerSettingValueText(settings[i]);
    const bool chevron = readerSettingNavigates(settings[i]);
    const std::string shownTitle = M4UiText::truncated(renderer, NOTOSANS_18_FONT_ID, title.c_str(), 220);
    M4UiText::draw(renderer, NOTOSANS_18_FONT_ID, 26, y + 16, shownTitle.c_str(), true);
    if (!value.empty()) {
      const std::string shownValue =
          M4UiText::truncated(renderer, NOTOSANS_14_FONT_ID, value.c_str(), 174);
      const int valueW = M4UiText::textWidth(renderer, NOTOSANS_14_FONT_ID, shownValue.c_str());
      M4UiText::draw(renderer, NOTOSANS_14_FONT_ID, 444 - valueW, y + 20, shownValue.c_str(), true);
    }
    if (chevron) {
      renderer.drawLine(447, y + 24, 453, y + 30, 3, true);
      renderer.drawLine(453, y + 30, 447, y + 36, 3, true);
    }
    renderer.drawLine(26, y + 57, 464, y + 57, 2, true);
  }

  if (count > pageItems) {
    const int trackY = kReaderWinTop;
    const int trackH = pageItems * kReaderRowH;
    int thumbH = 30;
    if (thumbH > trackH) thumbH = trackH;
    const int maxStart = count - pageItems;
    const int thumbY = trackY + ((trackH - thumbH) * pageStart) / maxStart;
    renderer.fillRect(470, thumbY, 1, thumbH, true);
  }

  // No persistent footer hints (ordinary Settings pages keep none). Touch areas,
  // input events, navigation, and the list-height math above are unchanged.

  if (firstPaint_) {
    firstPaint_ = false;
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}
