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

constexpr int kReaderRowH = 52;
constexpr int kReaderWinTop = 126;  // canvas row-0 top == window top at offset 0
int readerCanvasRowY(int i);

}  // namespace

void EpubReaderSettingsActivity::loop() {
  if (subActivity) {
    // Deferred pump: never destroy a nested picker while its loop is on the stack.
    pumpSubActivityFrame();
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
    const int offset =
        (count == 0 ? kReaderWinTop : readerCanvasRowY(pageStart)) - kReaderWinTop;
    int hit = -1;
    for (int i = pageStart; i < count && i < pageStart + pageItems; ++i) {
      const int y = readerCanvasRowY(i) - offset;
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
    }));
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

// v5.1 22-row canvas (paint/touch share this table; index/paging semantics
// never see it): G0 [0] @126; G1 [1-5] @217+52k; G2 [6-9] @516+52k (four
// independent margins); G3 [10-13], G4 [14-18], G5 [19-21] continue with
// the same 39px card-to-card rhythm, cards only (no invented titles).
int readerCanvasRowY(int i) {
  static constexpr int kBase[] = {126, 217, 269, 321, 373, 425, 516, 568, 620, 672,
                                  763, 815, 867, 919, 1010, 1062, 1114, 1166, 1218,
                                  1309, 1361, 1413};
  if (i < 0) return -1;
  if (i < 22) return kBase[i];
  return kBase[21] + kReaderRowH * (i - 21);
}

int readerRowGroup(const SettingInfo& s) {
  const char* k = s.key ? s.key : "";
  if (std::strcmp(k, kM4ReaderSettingsScopeKey) == 0) return 0;
  if (std::strcmp(k, kM4ReaderFontPickerKey) == 0 || std::strcmp(k, "firstlineintented") == 0 ||
      std::strcmp(k, "readerPixelSize") == 0 || std::strcmp(k, "lineSpacing") == 0 ||
      std::strcmp(k, "wordSpacing") == 0)
    return 1;
  if (std::strcmp(k, "screenMarginTop") == 0 || std::strcmp(k, "screenMarginBottom") == 0 ||
      std::strcmp(k, "screenMarginLeft") == 0 || std::strcmp(k, "screenMarginRight") == 0)
    return 2;
  if (std::strcmp(k, "readingBackground") == 0 || std::strcmp(k, "underline") == 0 ||
      std::strcmp(k, "underlineOffset") == 0 || std::strcmp(k, "underlineStyle") == 0)
    return 3;
  if (std::strcmp(k, "extraParagraphSpacing") == 0 || std::strcmp(k, "alignment") == 0 ||
      std::strcmp(k, "showTimeInsteadOfChapter") == 0 || std::strcmp(k, "epubShowImages") == 0 ||
      std::strcmp(k, "punctWidth") == 0)
    return 4;
  return 5;
}

}  // namespace

void EpubReaderSettingsActivity::render() const {
  renderer.clearScreen();

  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = UITheme::getInstance().getMetrics();

  // v5.1 header (static band 0..91): brand + title + battery + hairline.
  // No subtitle line (spec carries none). Baselines via exact line heights.
  const int brandTop = 27 - renderer.getLineHeight(SMALL_FONT_ID);
  const int titleTop = 72 - renderer.getLineHeight(UI_12_FONT_ID);
  M4UiText::draw(renderer, SMALL_FONT_ID, 24, brandTop, "Murphy M4", true);
  M4UiText::draw(renderer, UI_12_FONT_ID, 24, titleTop, kM4ReaderSettingsTitle, true,
                 EpdFontFamily::BOLD);
  const int readerBatt = powerManager.getBatteryPercentage() > 100
                             ? 100
                             : static_cast<int>(powerManager.getBatteryPercentage());
  renderer.drawRect(431, 18, 22, 10, true);
  renderer.fillRect(433, 20, (18 * readerBatt) / 100, 6, true);
  renderer.fillRect(453, 21, 2, 4, true);
  renderer.drawLine(22, 91, 458, 91, true);

  // Window math (kept shape, new pitch): the reserved footer space stays so
  // the bottom whitespace and touch areas below the list are unchanged.
  const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int listHeight = pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int listBottom = listTop + listHeight;
  const int count = static_cast<int>(settings.size());

  const int pageItems = std::max(1, listHeight / kReaderRowH);
  const int pageStart = count == 0 ? 0 : (selectedIndex / pageItems) * pageItems;
  const int offset =
      (count == 0 ? kReaderWinTop : readerCanvasRowY(pageStart)) - kReaderWinTop;

  // v5.1 section/card chrome, canvas-relative. Only G0-G2 carry real
  // section strings; G3-G5 continue cards-only (no invented titles).
  const int lh12 = renderer.getLineHeight(UI_12_FONT_ID);
  struct SecChrome {
    int baseY;
    const char* text;
  };
  static constexpr SecChrome kSecs[] = {{116, "应用"}, {207, "字体与排版"}, {506, "页边距"}};
  for (const auto& s : kSecs) {
    const int y = s.baseY - offset;
    if (y < -24 || y > pageHeight) continue;
    M4UiText::draw(renderer, UI_12_FONT_ID, 24, y - lh12, s.text, true, EpdFontFamily::BOLD);
  }
  struct CardChrome {
    int x, y, w, h;
  };
  static constexpr CardChrome kCards[] = {{22, 126, 436, 52},   {22, 217, 436, 260},
                                          {22, 516, 436, 208},  {22, 763, 436, 208},
                                          {22, 1010, 436, 260}, {22, 1309, 436, 156}};
  for (const auto& c : kCards) {
    const int y = c.y - offset;
    if (y + c.h <= 0 || y >= pageHeight) continue;
    renderer.drawRoundedRect(c.x, y, c.w, c.h, 1, 10, true);
  }

  const int labelTopDy = 32 - lh12;
  const int valueTopDy = 32 - renderer.getLineHeight(UI_10_FONT_ID);
  for (int i = pageStart; i < count && i < pageStart + pageItems; ++i) {
    const int y = readerCanvasRowY(i) - offset;
    if (y >= listBottom) break;
    if (y + kReaderRowH <= kReaderWinTop) continue;
    const bool selected = (i == selectedIndex);
    // v5.3 B: stipple field only; the card already draws the border.
    if (selected) {
      renderer.fillRectStipple(30, y + 8, 420, 36);
    }
    const std::string title = getChineseName(settings[i].name);
    const std::string value = readerSettingValueText(settings[i]);
    const bool chevron = readerSettingNavigates(settings[i]);
    // Single value column at x418 (spec); chevron column starts at 440.
    const int valueEdge = 418;
    int titleW = valueEdge - 40 - (value.empty() ? 0 : 100);
    if (titleW < 0) titleW = 0;
    const std::string shownTitle = M4UiText::truncated(renderer, UI_12_FONT_ID, title.c_str(), titleW);
    M4UiText::draw(renderer, UI_12_FONT_ID, 40, y + labelTopDy, shownTitle.c_str(), true,
                   EpdFontFamily::BOLD);
    if (!value.empty()) {
      const std::string shownValue =
          M4UiText::truncated(renderer, UI_10_FONT_ID, value.c_str(), 240);
      const int valueW = M4UiText::textWidth(renderer, UI_10_FONT_ID, shownValue.c_str());
      M4UiText::draw(renderer, UI_10_FONT_ID, valueEdge - valueW, y + valueTopDy, shownValue.c_str(),
                     true);
    }
    if (chevron) {
      renderer.drawLine(440, y + 21, 445, y + 26, 2, true);
      renderer.drawLine(445, y + 26, 440, y + 31, 2, true);
    }
    // Divider under the row iff the next row shares its group (SVG rhythm
    // inside cards; same rule past the SVG screen). Row bottom - 1.
    if (i + 1 < count && readerRowGroup(settings[i]) == readerRowGroup(settings[i + 1])) {
      renderer.drawLine(40, y + kReaderRowH - 1, 446, y + kReaderRowH - 1, true);
    }
  }

  // Weak 1px scroll thumb, no track (paint-only; paging math above untouched).
  // v5.2 B: fixed ~30px indicator like the Settings pages; the proportional
  // bar painted a several-hundred-px line. Y mapping below is unchanged.
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
