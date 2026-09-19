#include "SettingsActivity.h"

#include <algorithm>
#include <cstring>
#include <GfxRenderer.h>
#include <HardwareSerial.h>

#include "util/ButtonNavigator.h"
#include "ButtonRemapActivity.h"
#include "util/M4ListTouchPolicy.h"
#include "util/TouchHitGeometry.h"
#include "ClearCacheActivity.h"
#include "CrossPointSettings.h"
#include "I18n.h"
#include "KOReaderSettingsActivity.h"
#include "MappedInputManager.h"
#include "NumberSelectionActivity.h"
#include "SimpleBluetoothActivity.h"
#include "components/UITheme.h"
#include <EpdFontLoader.h>
#include "FontSelectionActivity.h"
#include "fontIds.h"
#include "util/M4UiText.h"
#include "JianGuoYunSettingsActivity.h"
#include "DataCapsuleSettingsActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "ResetSettingsActivity.h"
#ifdef CROSSPOINT_MURPHY_M4
#include "DeveloperOptionsActivity.h"
#include <esp_ota_ops.h>
#include <WiFi.h>
#endif
#include <HalPowerManager.h>
#include <BluetoothHIDManager.h>
#include "activities/reader/EpubReaderSettingsActivity.h"
#include "activities/network/WifiSelectionActivity.h"

#include "SettingsLists.h"
#include "activities/settings/M4SettingsAction.h"
#include "activities/settings/M4SettingsCatalog.h"
#include "activities/settings/M4SettingsFocus.h"
#include "activities/settings/M4SettingsNav.h"
#include "activities/settings/M4SettingsPaint.h"
#include "activities/settings/M4SettingsRootUi.h"
#include "activities/settings/SettingsSceneModel.h"
#include "ui/scene/GfxSceneRenderer.h"
#include "ui/scene/UiSceneRuntime.h"
#include "generated/murphy_settings_l2_m4theme.h"
#include "generated/murphy_settings_root_m4theme.h"
#include "generated/murphy_settings_maintenance_m4theme.h"
#include "generated/murphy_settings_frontlight_m4theme.h"
#include "generated/murphy_settings_keys_m4theme.h"
#include "generated/murphy_settings_choice_m4theme.h"

namespace {
#ifdef CROSSPOINT_MURPHY_M4
const esp_partition_t* runningOtaPartition() {
  return esp_ota_get_running_partition();
}
const char* runningOtaLabel() {
  const auto* running = runningOtaPartition();
  if (running && running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) return L(Str::kApp0Official);
  if (running && running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) return L(Str::kApp1Custom);
  return L(Str::kUnknownBootSlot);
}
bool switchToOtherOtaSlot() {
  const auto* running = runningOtaPartition();
  if (!running) return false;
  const esp_partition_subtype_t targetSubtype =
      running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ? ESP_PARTITION_SUBTYPE_APP_OTA_1
                                                          : ESP_PARTITION_SUBTYPE_APP_OTA_0;
  const auto* target = esp_partition_find_first(ESP_PARTITION_TYPE_APP, targetSubtype, nullptr);
  return target && esp_ota_set_boot_partition(target) == ESP_OK;
}
#endif

void appendAction(std::vector<SettingInfo>& out, const char* name, const char* key) {
  auto act = SettingInfo::Action(name);
  act.key = key;
  out.push_back(std::move(act));
}

bool isRootDoorKey(const char* key) {
  return key && (std::strcmp(key, "wifi") == 0 || std::strcmp(key, "frontlight") == 0 ||
                 std::strcmp(key, "readerLayout") == 0 || std::strcmp(key, "keys") == 0 ||
                 std::strcmp(key, "maintenance") == 0 || std::strcmp(key, "advanced") == 0);
}

const char* footerI18n(M4SettingsControl c) {
  switch (c) {
    case M4SettingsControl::Toggle:
      return L(Str::kToggle);
    case M4SettingsControl::Choice:
    case M4SettingsControl::Number:
      return L(Str::kSelect);
    case M4SettingsControl::Navigate:
    case M4SettingsControl::Confirm:
    default:
      return L(Str::kOpen);
  }
}

// Advanced v5.1 group-layout row text: same single-line semantics as the
// scene text node it replaces (measure, UTF-8 ellipsis at maxW, optional
// right align), painted at the shared Y table.
void drawAdvancedRowText(const GfxRenderer& renderer, int fontId, int x, int y, int maxW,
                         const char* text, bool alignRight) {
  if (!text || text[0] == '\0' || maxW <= 0) return;
  const char* s = text;
  char lineBuf[256];
  int fullW = renderer.getTextWidth(fontId, text);
  if (fullW < 0) fullW = 0;
  if (fullW > maxW) {
    static const char kEllipsis[] = "\xE2\x80\xA6";
    const int ellW = renderer.getTextWidth(fontId, kEllipsis);
    const int budget = maxW > ellW ? maxW - ellW : 0;
    size_t take = UiScene::GfxSceneRenderer::fitUtf8Prefix(renderer, fontId, text, std::strlen(text),
                                                           budget);
    if (take > sizeof(lineBuf) - 4) take = sizeof(lineBuf) - 4;
    std::memcpy(lineBuf, text, take);
    std::memcpy(lineBuf + take, kEllipsis, 3);
    lineBuf[take + 3] = '\0';
    s = lineBuf;
    fullW = maxW;
  }
  const int xx = alignRight ? x + maxW - (fullW < maxW ? fullW : maxW) : x;
  renderer.drawText(fontId, xx, y, s, true);
}
}  // namespace

void SettingsActivity::taskTrampoline(void* param) {
  auto* self = static_cast<SettingsActivity*>(param);
  self->displayTaskLoop();
}

void SettingsActivity::onEnter() {
  Activity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();

  allSettings_.clear();
  for (auto& setting : getSettingsList()) {
    if (setting.key) allSettings_.push_back(std::move(setting));
  }
  appendAction(allSettings_, L(Str::kReaderLayout), "readerLayout");
  appendAction(allSettings_, L(Str::kRemapFrontButtons), "remapButtons");
  appendAction(allSettings_, L(Str::kBluetoothSettings), "bluetooth");
  appendAction(allSettings_, L(Str::kKOReaderSync), "koreader");
  appendAction(allSettings_, L(Str::kJianGuoConfig), "jianguo");
  appendAction(allSettings_, L(Str::kDataCapsuleConfig), "dataCapsule");
  appendAction(allSettings_, L(Str::kClearCache), "clearCache");
  appendAction(allSettings_, L(Str::kResetSettings), "resetSettings");
#ifdef CROSSPOINT_MURPHY_M4
  appendAction(allSettings_, L(Str::kDeveloperOptions), "developerOptions");
  appendAction(allSettings_, L(Str::kSwitchBootSlot), "switchBootSlot");
#endif

  navState_ = SettingsNavState{};
  navState_.pane = SettingsPane::Category;
  m4SettingsUiEnterRoot(ui_);
  m4SettingsPaintResetFull(paint_, ui_);
  m4SettingsCopyKey(navState_.selectedKey, (int)sizeof(navState_.selectedKey), ui_.selectedKey);

  rebuildModel();
  updateRequired = true;

  xTaskCreate(&SettingsActivity::taskTrampoline, "SettingsActivityTask", 8192, this, 1, &displayTaskHandle);
}

void SettingsActivity::onExit() {
  ActivityWithSubactivity::onExit();
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
  UITheme::getInstance().reload();
}

void SettingsActivity::handleBack() {
  if (ui_.page == M4SettingsPageKind::Confirm) {
    m4SettingsUiConfirmDecide(ui_, M4ConfirmButton::Back, true);
    m4SettingsPaintResetFull(paint_, ui_);
    rebuildModel();
    updateRequired = true;
    return;
  }
  if (ui_.page == M4SettingsPageKind::Choice) {
    const char* key = ui_.choiceKey[0] ? ui_.choiceKey : ui_.selectedKey;
    ui_.page = ui_.returnPage;
    if (ui_.page == M4SettingsPageKind::ChildList) {
      m4SettingsUiSelectKey(ui_, ui_.selectedKey);
    } else {
      m4SettingsUiEnterRoot(ui_);
      m4SettingsUiSelectKey(ui_, key);
    }
    m4SettingsPaintResetFull(paint_, ui_);
    rebuildModel();
    updateRequired = true;
    return;
  }
  if (ui_.page == M4SettingsPageKind::ChildList) {
    char parent[32]{};
    m4SettingsCopyKey(parent, (int)sizeof(parent), ui_.parentKey);
    m4SettingsUiEnterRoot(ui_);
    m4SettingsUiSelectKey(ui_, parent);
    m4SettingsPaintResetFull(paint_, ui_);
    rebuildModel();
    updateRequired = true;
    return;
  }
  SETTINGS.saveToFile();
  EpdFontLoader::loadFontsFromSd(renderer);
  onGoHome();
}

void SettingsActivity::handleRowHit(int windowIndex, bool activate) {
  const int count = m4SettingsUiVisibleCount(ui_);
  const int index = ui_.windowStart + windowIndex;
  if (index < 0 || index >= count) return;
  const int oldSlot = ui_.selectedSlot;
  const int oldWin = ui_.windowStart;
  if (ui_.page == M4SettingsPageKind::Choice) {
    m4SettingsUiMove(ui_, index - oldSlot);
  } else {
    const M4SettingsRow* row = m4SettingsUiVisibleRow(ui_, index);
    if (!row || !row->key) return;
    m4SettingsUiSelectKey(ui_, row->key);
    m4SettingsCopyKey(navState_.selectedKey, (int)sizeof(navState_.selectedKey), ui_.selectedKey);
  }
  if (oldSlot != ui_.selectedSlot || oldWin != ui_.windowStart) {
    if (ui_.page == M4SettingsPageKind::Root) {
      m4SettingsPaintNoteMoveRoot(ui_, paint_, oldSlot, ui_.selectedSlot);
    } else {
      m4SettingsPaintNoteMove(ui_, paint_, oldSlot, ui_.selectedSlot, m4SettingsUiListItemH(ui_),
                              m4SettingsUiListItemGap(ui_), oldWin);
    }
  }
  if (activate) activateCurrent();
  else {
    rebuildModel();
    updateRequired = true;
  }
}

void SettingsActivity::loop() {
  if (subActivity) {
    if (pumpSubActivityFrame()) updateRequired = true;
    return;
  }

  if (ui_.page == M4SettingsPageKind::Confirm) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Power)) {
      m4SettingsUiConfirmDecide(ui_, M4ConfirmButton::Power, true);
      return;
    }
    if (mappedInput.hasTouch() && mappedInput.wasBackGesture()) {
      handleBack();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      handleBack();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateCurrent();
      return;
    }
    return;
  }

  if (mappedInput.hasTouch() && mappedInput.wasBackGesture()) {
    handleBack();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    handleBack();
    return;
  }

  if (mappedInput.hasTouch()) {
    auto swipe = mappedInput.wasSwipe();
    const int count = m4SettingsUiVisibleCount(ui_);
    if (count > 0 && (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down)) {
      const bool pageDown = swipe == MappedInputManager::SwipeDir::Up;
      const int oldSlot = ui_.selectedSlot;
      const int oldWin = ui_.windowStart;
      int next = M4ListTouchPolicy::applyPage(ui_.selectedSlot, count, 8, pageDown);
      next = m4SettingsNavMoveClamp(next, 0, count);
      m4SettingsUiMove(ui_, next - oldSlot);
      if (ui_.page == M4SettingsPageKind::Root) {
        m4SettingsPaintNoteMoveRoot(ui_, paint_, oldSlot, ui_.selectedSlot);
      } else {
        m4SettingsPaintNoteMove(ui_, paint_, oldSlot, ui_.selectedSlot, m4SettingsUiListItemH(ui_),
                                m4SettingsUiListItemGap(ui_), oldWin);
      }
      m4SettingsCopyKey(navState_.selectedKey, (int)sizeof(navState_.selectedKey), ui_.selectedKey);
      rebuildModel();
      updateRequired = true;
      return;
    }

    int tx = 0, ty = 0;
    bool tapped = mappedInput.wasScreenTapped(tx, ty);
    bool down = mappedInput.wasScreenTouchDown(tx, ty);
    if (tapped || down) {
      // Advanced hit uses the group Y table (same table the rows paint);
      // every other list keeps the contiguous whole-row helper.
      const int hit = (ui_.page == M4SettingsPageKind::Root) ? m4SettingsRootRowHit(tx, ty, 480)
                      : m4SettingsUiIsAdvancedPage(ui_)     ? m4SettingsAdvancedRowHit(tx, ty,
                                                                                       ui_.windowStart)
                                                            : m4SettingsWholeRowHit(
                                                                  tx, ty, 0,
                                                                  m4SettingsUiContentOriginY(ui_),
                                                                  m4SettingsUiListItemH(ui_),
                                                                  m4SettingsUiListItemGap(ui_), 480);
      if (hit >= 0) {
        handleRowHit(hit, tapped);
        return;
      }
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateCurrent();
    return;
  }

  const int count = m4SettingsUiVisibleCount(ui_);
  if (count > 0) {
    ButtonNavigator navigator;
    navigator.onPreviousRelease([this] {
      const int oldSlot = ui_.selectedSlot;
      const int oldWin = ui_.windowStart;
      m4SettingsUiMove(ui_, -1);
      if (ui_.page == M4SettingsPageKind::Root) {
        m4SettingsPaintNoteMoveRoot(ui_, paint_, oldSlot, ui_.selectedSlot);
      } else {
        m4SettingsPaintNoteMove(ui_, paint_, oldSlot, ui_.selectedSlot, m4SettingsUiListItemH(ui_),
                                m4SettingsUiListItemGap(ui_), oldWin);
      }
      m4SettingsCopyKey(navState_.selectedKey, (int)sizeof(navState_.selectedKey), ui_.selectedKey);
      rebuildModel();
      updateRequired = true;
    });
    navigator.onNextRelease([this] {
      const int oldSlot = ui_.selectedSlot;
      const int oldWin = ui_.windowStart;
      m4SettingsUiMove(ui_, 1);
      if (ui_.page == M4SettingsPageKind::Root) {
        m4SettingsPaintNoteMoveRoot(ui_, paint_, oldSlot, ui_.selectedSlot);
      } else {
        m4SettingsPaintNoteMove(ui_, paint_, oldSlot, ui_.selectedSlot, m4SettingsUiListItemH(ui_),
                                m4SettingsUiListItemGap(ui_), oldWin);
      }
      m4SettingsCopyKey(navState_.selectedKey, (int)sizeof(navState_.selectedKey), ui_.selectedKey);
      rebuildModel();
      updateRequired = true;
    });
  }
}

void SettingsActivity::rebuildModel() {
  sceneModel_.begin(UiScene::DataState::Ready);
  uint16_t bat = 80;
#ifdef CROSSPOINT_MURPHY_M4
  bat = powerManager.getBatteryPercentage();
#endif
  sceneModel_.setBattery(bat);
  sceneModel_.setPane(SettingsPane::Category);
  navState_.pane = SettingsPane::Category;
  m4SettingsCopyKey(navState_.selectedKey, (int)sizeof(navState_.selectedKey), ui_.selectedKey);

  if (ui_.page == M4SettingsPageKind::Root) {
    char wifi[32]{};
    char fl[48]{};
#ifdef CROSSPOINT_MURPHY_M4
    const char* ssid = (WiFi.status() == WL_CONNECTED) ? WiFi.SSID().c_str() : "";
#else
    const char* ssid = "";
#endif
    m4SettingsFormatWifiValue(ssid, wifi, 32);
    m4SettingsFormatFrontlightValue((int)SETTINGS.frontlightBrightness, (int)SETTINGS.frontlightWarmth, fl, 48);
    const std::string reader = std::to_string((int)SETTINGS.getReaderPixelSize());
    const std::string sleep = liveValueForKey("sleepTimeout");
    const std::string lock = liveValueForKey("sleepScreen");
    sceneModel_.populateRootFromCatalog(ui_.selectedKey, wifi, fl, reader.c_str(), sleep.c_str(), lock.c_str(),
                                        "", "");
  } else {
    sceneModel_.clearWindow();
    const char* title = "设置";
    if (ui_.page == M4SettingsPageKind::ChildList) {
      const M4SettingsRow* parent =
          m4SettingsRowByKey(m4SettingsRootCatalog(), kM4SettingsRootCount, ui_.parentKey);
      if (parent && parent->titleZh) title = parent->titleZh;
    } else if (ui_.page == M4SettingsPageKind::Choice) {
      const SettingInfo* info = findSettingByKey(ui_.choiceKey[0] ? ui_.choiceKey : ui_.selectedKey);
      title = info && info->name ? info->name : "选择";
    }
    sceneModel_.setPageTitle(title);
    const int count = m4SettingsUiVisibleCount(ui_);
    for (int i = 0; i < (int)SettingsScene::kMaxWindowRows; ++i) {
      const int idx = ui_.windowStart + i;
      if (idx < 0 || idx >= count) {
        sceneModel_.setWindowRow((uint8_t)i, "", "", "", false, false);
        continue;
      }
      if (ui_.page == M4SettingsPageKind::Choice) {
        const char* label = (idx < (int)choiceLabels_.size()) ? choiceLabels_[idx].c_str() : "";
        const bool checked = m4SettingsUiChoiceChecked(ui_, idx);
        sceneModel_.setWindowRow((uint8_t)i, "", label, checked ? "✓" : "", false, idx == ui_.selectedSlot);
      } else {
        const M4SettingsRow* row = m4SettingsUiVisibleRow(ui_, idx);
        if (!row) {
          sceneModel_.setWindowRow((uint8_t)i, "", "", "", false, false);
          continue;
        }
        const SettingInfo* info = findSettingByKey(row->key);
        const char* rowTitle = info && info->name ? info->name : row->titleZh;
        const std::string value = liveValueForKey(row->key);
        // Advanced v5.1 chevron truth: mirrors activateCurrent's
        // leave-the-list decision. Toggle rows and in-place ENUM-cycle rows
        // flip where they stand (value, no chevron). Other L2 pages keep
        // today's paint (navigates defaults to true).
        bool navigates = true;
        if (m4SettingsUiIsAdvancedPage(ui_)) {
          navigates = row->control != M4SettingsControl::Toggle;
          if (navigates && info && info->type == SettingType::ENUM &&
              (int)info->enumValues.size() < 3 && row->control != M4SettingsControl::Choice) {
            navigates = false;
          }
        }
        sceneModel_.setWindowRow((uint8_t)i, row->key, rowTitle, value.c_str(), false,
                                 idx == ui_.selectedSlot, navigates);
      }
    }
    if (m4SettingsUiIsAdvancedPage(ui_)) {
      // Rows are painted by the C++ group layout below (section gaps need
      // per-group Y the uniform scene repeat cannot express): hide the scene
      // rows so the repeat paints chrome only. Snapshot rows stay filled for
      // the C++ painter.
      sceneModel_.setWindowCount(0);
    }
  }
  sceneModel_.publish();
}

const SettingInfo* SettingsActivity::findSettingByKey(const char* key) const {
  if (!key) return nullptr;
  for (const auto& s : allSettings_) {
    if (s.key && std::strcmp(s.key, key) == 0) return &s;
  }
  return nullptr;
}

std::string SettingsActivity::valueTextForSetting(const SettingInfo& info) const {
  std::string valueText;
  if (info.type == SettingType::TOGGLE && info.valuePtr != nullptr) {
    bool v = SETTINGS.*(info.valuePtr);
    valueText = v ? "开" : "关";
  } else if (info.type == SettingType::ENUM && info.valuePtr != nullptr) {
    uint8_t v = SETTINGS.*(info.valuePtr);
    if (v < info.enumValues.size()) valueText = info.enumValues[v];
    else valueText = "?";
  } else if (info.type == SettingType::ENUM && info.valueGetter) {
    uint8_t idx = info.valueGetter();
    if (idx < info.enumValues.size()) valueText = info.enumValues[idx];
  } else if (info.type == SettingType::VALUE && info.signedValuePtr != nullptr) {
    valueText = std::to_string((int)(SETTINGS.*(info.signedValuePtr)));
  } else if (info.type == SettingType::VALUE && info.valuePtr != nullptr) {
    int v = SETTINGS.*(info.valuePtr);
    if (info.key && strcmp(info.key, "lineSpacing") == 0) {
      valueText = std::to_string(v / 10) + "." + std::to_string(v % 10) + L(Str::kValTimes);
    } else if (info.key && strcmp(info.key, "refreshFrequency") == 0) {
      valueText = std::to_string(v) + L(Str::kValPagesFullRefresh);
    } else if (info.key && (strcmp(info.key, "frontlightBrightness") == 0 ||
                            strcmp(info.key, "frontlightWarmth") == 0)) {
      valueText = std::to_string(v) + "%";
    } else {
      valueText = std::to_string(v);
    }
  } else if (info.type == SettingType::ACTION && info.key && strcmp(info.key, "bluetooth") == 0) {
    try {
      auto& bt = BluetoothHIDManager::getInstance();
      valueText = bt.isEnabled() ? "开" : "关";
    } catch (...) {
      valueText = L(Str::kError);
    }
  } else if (info.type == SettingType::ACTION && info.key && strcmp(info.key, "switchBootSlot") == 0) {
#ifdef CROSSPOINT_MURPHY_M4
    valueText = runningOtaLabel();
#endif
  }
  return valueText;
}

std::string SettingsActivity::liveValueForKey(const char* key) const {
  const SettingInfo* info = findSettingByKey(key);
  if (!info) return {};
  return valueTextForSetting(*info);
}

M4SettingsControl SettingsActivity::controlForKey(const char* key) const {
  if (ui_.page == M4SettingsPageKind::Choice) return M4SettingsControl::Choice;
  if (ui_.page == M4SettingsPageKind::Root) {
    const M4SettingsRow* row = m4SettingsRowByKey(m4SettingsRootCatalog(), kM4SettingsRootCount, key);
    if (row) return row->control;
  }
  if (ui_.page == M4SettingsPageKind::ChildList) {
    const M4SettingsRow* row = m4SettingsChildAt(ui_.parentKey, ui_.selectedSlot);
    if (row) return row->control;
  }
  const SettingInfo* info = findSettingByKey(key);
  if (!info) return M4SettingsControl::Navigate;
  const bool danger = info->key && (std::strcmp(info->key, "clearCache") == 0 ||
                                    std::strcmp(info->key, "resetSettings") == 0);
  const bool door = info->type == SettingType::ACTION;
  const int optionCount = (int)info->enumValues.size();
  return m4SettingsControlForKind((uint8_t)info->type, optionCount, danger, door);
}

void SettingsActivity::restoreSavedKey(bool committed) {
  m4SettingsUiPopEditor(ui_, committed);
  m4SettingsUiSelectKey(ui_, savedKey_[0] ? savedKey_ : ui_.selectedKey);
  m4SettingsCopyKey(navState_.selectedKey, (int)sizeof(navState_.selectedKey), ui_.selectedKey);
  m4SettingsPaintResetFull(paint_, ui_);
  rebuildModel();
  updateRequired = true;
}

void SettingsActivity::applyChoiceIndex(const char* key, int index) {
  const SettingInfo* found = findSettingByKey(key);
  if (!found || index < 0) return;
  const auto& setting = *found;
  if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    if (index >= (int)setting.enumValues.size()) return;
    SETTINGS.*(setting.valuePtr) = (uint8_t)index;
    SETTINGS.saveToFile();
  } else if (setting.type == SettingType::ENUM && setting.valueSetter) {
    setting.valueSetter((uint8_t)index);
#ifdef CROSSPOINT_MURPHY_M4
    if (setting.key && strcmp(setting.key, "uiFontSize") == 0) {
      EpdFontLoader::applySystemChrome(renderer);
    }
#endif
  }
}

void SettingsActivity::openNumberPicker(const SettingInfo& setting) {
  m4SettingsCopyKey(savedKey_, (int)sizeof(savedKey_), setting.key ? setting.key : ui_.selectedKey);
  m4SettingsUiPushEditor(ui_);
  NumberSelectionActivity::Config cfg;
  cfg.title = setting.name;
  cfg.minValue = setting.valueRange.min;
  cfg.maxValue = setting.valueRange.max;
  cfg.smallStep = setting.valueRange.step;
  cfg.largeStep = setting.valueRange.step * 5;
  cfg.isSigned = setting.signedValuePtr != nullptr;
  if (setting.name && strcmp(setting.name, L(Str::kRefreshFrequency)) == 0) {
    cfg.displayFormatter = [](int v) { return std::to_string(v) + L(Str::kValPagesFullRefresh); };
  } else if (setting.name && strcmp(setting.name, L(Str::kLineSpacing)) == 0) {
    cfg.displayFormatter = [](int v) {
      return std::to_string(v / 10) + "." + std::to_string(v % 10) + L(Str::kValTimes);
    };
  }
  int cur = 0;
  if (setting.signedValuePtr != nullptr) cur = SETTINGS.*(setting.signedValuePtr);
  else if (setting.valuePtr != nullptr) cur = SETTINGS.*(setting.valuePtr);
  auto signedPtr = setting.signedValuePtr;
  auto valuePtr = setting.valuePtr;
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  exitActivity();
  enterNewActivity(new NumberSelectionActivity(
      renderer, mappedInput, cfg, cur,
      [this, signedPtr, valuePtr](int v) {
        if (signedPtr) SETTINGS.*(signedPtr) = (int8_t)v;
        else if (valuePtr) SETTINGS.*(valuePtr) = (uint8_t)v;
        SETTINGS.saveToFile();
        exitActivity();
        restoreSavedKey(true);
      },
      [this]() {
        exitActivity();
        restoreSavedKey(false);
      }));
  xSemaphoreGive(renderingMutex);
}

void SettingsActivity::launchAction(const SettingInfo& setting) {
  const char* k = setting.key ? setting.key : "";
  m4SettingsCopyKey(savedKey_, (int)sizeof(savedKey_), k[0] ? k : ui_.selectedKey);
  auto restore = [this]() {
    exitActivity();
    m4SettingsUiSelectKey(ui_, savedKey_);
    m4SettingsCopyKey(navState_.selectedKey, (int)sizeof(navState_.selectedKey), ui_.selectedKey);
    m4SettingsPaintResetFull(paint_, ui_);
    rebuildModel();
    updateRequired = true;
  };

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  exitActivity();
  if (strcmp(k, "remapButtons") == 0) {
    enterNewActivity(new ButtonRemapActivity(renderer, mappedInput, restore));
  } else if (strcmp(k, "readerLayout") == 0) {
    enterNewActivity(new EpubReaderSettingsActivity(renderer, mappedInput, restore));
  } else if (strcmp(k, "bluetooth") == 0) {
    enterNewActivity(new SimpleBluetoothActivity(renderer, mappedInput, restore));
  } else if (strcmp(k, "koreader") == 0) {
    enterNewActivity(new KOReaderSettingsActivity(renderer, mappedInput, restore));
  } else if (strcmp(k, "jianguo") == 0) {
    enterNewActivity(new JianGuoYunSettingsActivity(renderer, mappedInput, restore));
  } else if (strcmp(k, "dataCapsule") == 0) {
    enterNewActivity(new DataCapsuleSettingsActivity(renderer, mappedInput, restore));
  } else if (strcmp(k, "clearCache") == 0) {
    enterNewActivity(new ClearCacheActivity(renderer, mappedInput, restore));
  } else if (strcmp(k, "resetSettings") == 0) {
    enterNewActivity(new ResetSettingsActivity(renderer, mappedInput, restore));
#ifdef CROSSPOINT_MURPHY_M4
  } else if (strcmp(k, "developerOptions") == 0) {
    enterNewActivity(new DeveloperOptionsActivity(renderer, mappedInput, restore));
#endif
  } else if (strcmp(k, "wifi") == 0) {
    enterNewActivity(new WifiSelectionActivity(renderer, mappedInput, [restore](bool) { restore(); }));
  } else {
    xSemaphoreGive(renderingMutex);
    return;
  }
  xSemaphoreGive(renderingMutex);
}

void SettingsActivity::toggleCurrentSetting() {
  const char* key = m4SettingsUiActivateIdentity(ui_);
  if (!key || !key[0]) {
    key = navState_.selectedKey[0] ? navState_.selectedKey : nullptr;
  }
  if (isRootDoorKey(key) && ui_.page == M4SettingsPageKind::Root) {
    if (std::strcmp(key, "wifi") == 0) {
      SettingInfo wifi = SettingInfo::Action("Wi-Fi");
      wifi.key = "wifi";
      launchAction(wifi);
      return;
    }
    if (std::strcmp(key, "readerLayout") == 0) {
      const SettingInfo* info = findSettingByKey(key);
      if (info) launchAction(*info);
      return;
    }
    m4SettingsUiOpenChildList(ui_, key);
    m4SettingsPaintResetFull(paint_, ui_);
    rebuildModel();
    updateRequired = true;
    return;
  }

  const SettingInfo* found = findSettingByKey(key);
  if (!found) return;
  const auto& setting = *found;
  const M4SettingsControl control = controlForKey(setting.key);

  if (control == M4SettingsControl::Choice ||
      (setting.type == SettingType::ENUM && (int)setting.enumValues.size() >= 3)) {
    uint8_t cur = 0;
    if (setting.valuePtr != nullptr) cur = SETTINGS.*(setting.valuePtr);
    else if (setting.valueGetter) cur = setting.valueGetter();
    choiceLabels_ = setting.enumValues;
    m4SettingsUiOpenChoice(ui_, setting.key, (int)setting.enumValues.size(), (int)cur);
    m4SettingsPaintResetFull(paint_, ui_);
    rebuildModel();
    updateRequired = true;
    return;
  }

  if (setting.key && std::strcmp(setting.key, "switchBootSlot") == 0) {
    m4SettingsUiOpenConfirm(ui_, setting.key);
    m4SettingsPaintResetFull(paint_, ui_);
    rebuildModel();
    updateRequired = true;
    return;
  }

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    bool cur = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !cur;
    SETTINGS.saveToFile();
    rebuildModel();
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    size_t n = setting.enumValues.size();
    if (n == 0) return;
    uint8_t cur = SETTINGS.*(setting.valuePtr);
    if (cur >= n) cur = 0;
    SETTINGS.*(setting.valuePtr) = (uint8_t)((cur + 1) % n);
    SETTINGS.saveToFile();
    rebuildModel();
  } else if (setting.type == SettingType::ENUM && setting.valueGetter && setting.valueSetter) {
    size_t n = setting.enumValues.size();
    if (n == 0) return;
    uint8_t cur = setting.valueGetter();
    uint8_t safe = cur < n ? cur : 0;
    setting.valueSetter((uint8_t)((safe + 1) % n));
#ifdef CROSSPOINT_MURPHY_M4
    if (setting.key && strcmp(setting.key, "uiFontSize") == 0) {
      EpdFontLoader::applySystemChrome(renderer);
    }
#endif
    rebuildModel();
  } else if (setting.type == SettingType::VALUE) {
    openNumberPicker(setting);
    return;
  } else if (setting.type == SettingType::ACTION) {
    launchAction(setting);
    return;
  } else {
    return;
  }
  SETTINGS.saveToFile();
  rebuildModel();
}

void SettingsActivity::activateCurrent() {
  if (ui_.page == M4SettingsPageKind::Choice) {
    const int committed = m4SettingsUiChoiceCommit(ui_);
    applyChoiceIndex(ui_.selectedKey, committed);
    m4SettingsPaintResetFull(paint_, ui_);
    rebuildModel();
    updateRequired = true;
    return;
  }
  if (ui_.page == M4SettingsPageKind::Confirm) {
    if (m4SettingsUiConfirmDecide(ui_, M4ConfirmButton::Confirm, true) && ui_.bootSlotSwitchRequested) {
      performSwitchBootSlot();
    }
    return;
  }
  toggleCurrentSetting();
  updateRequired = true;
}

void SettingsActivity::performSwitchBootSlot() {
#ifdef CROSSPOINT_MURPHY_M4
  const auto* running = runningOtaPartition();
  const char* target = (running && running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) ? L(Str::kApp1Custom)
                                                                                        : L(Str::kApp0Official);
  if (switchToOtherOtaSlot()) {
    GUI.drawPopup(renderer, target);
    delay(350);
    ESP.restart();
  } else {
    GUI.drawPopup(renderer, L(Str::kUnknownBootSlot));
    delay(700);
    m4SettingsPaintResetFull(paint_, ui_);
    rebuildModel();
    updateRequired = true;
  }
#endif
}

void SettingsActivity::submitDisplay(const M4SettingsRefreshRequest& req) const {
  struct Ctx {
    const GfxRenderer* renderer;
  } ctx{&renderer};
  M4SettingsDisplayPort port{
      [](void* p) { static_cast<Ctx*>(p)->renderer->displayBuffer(); },
      [](void* p, int x, int y, int w, int h) {
        static_cast<Ctx*>(p)->renderer->displayWindow(x, y, w, h);
      },
      &ctx,
  };
  m4SettingsDisplaySubmit(port, req);
}

void SettingsActivity::displayTaskLoop() {
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

void SettingsActivity::render() const {
  const M4SettingsRefreshRequest req =
      m4SettingsPaintTake(paint_, m4SettingsUiContentOriginY(ui_), 480, 800);

  if (ui_.page == M4SettingsPageKind::Confirm) {
    renderer.clearScreen();
    const auto pageHeight = renderer.getScreenHeight();
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, 15, L(Str::kSwitchBootSlot), true, EpdFontFamily::BOLD);
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, pageHeight / 2 - 20, L(Str::kSwitchBootSlot), true);
    const auto labels = mappedInput.mapLabels(L(Str::kCancel), L(Str::kToggle), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    submitDisplay(req);
    return;
  }

  renderer.clearScreen();
  SettingsScene::SettingsSnapshot snap{};
  bool hasSnap = sceneModel_.copyLatest(snap);
  if (!hasSnap) {
    snap.state = UiScene::DataState::Loading;
  }
  auto src = SettingsScene::SettingsSceneModel::bindingSource(snap);
  UiScene::UiSceneAssets assets;
  assets.clear();
  UiScene::GfxSceneRenderer r;
  if (ui_.page == M4SettingsPageKind::Root) {
    r.render(murphy_settings_root_m4theme, murphy_settings_root_m4theme_len, src, assets, renderer);
  } else if (ui_.page == M4SettingsPageKind::Choice) {
    r.render(murphy_settings_choice_m4theme, murphy_settings_choice_m4theme_len, src, assets, renderer);
  } else if (ui_.page == M4SettingsPageKind::ChildList &&
             std::strcmp(ui_.parentKey, "frontlight") == 0) {
    r.render(murphy_settings_frontlight_m4theme, murphy_settings_frontlight_m4theme_len, src, assets,
             renderer);
  } else if (ui_.page == M4SettingsPageKind::ChildList && std::strcmp(ui_.parentKey, "keys") == 0) {
    r.render(murphy_settings_keys_m4theme, murphy_settings_keys_m4theme_len, src, assets, renderer);
  } else if (ui_.page == M4SettingsPageKind::ChildList &&
             std::strcmp(ui_.parentKey, "maintenance") == 0) {
    r.render(murphy_settings_maintenance_m4theme, murphy_settings_maintenance_m4theme_len, src, assets,
             renderer);
  } else {
    r.render(murphy_settings_l2_m4theme, murphy_settings_l2_m4theme_len, src, assets, renderer);
  }

  const int count = m4SettingsUiVisibleCount(ui_);
  const bool isAdvanced = m4SettingsUiIsAdvancedPage(ui_);
  const int winCap = isAdvanced ? 9 : 8;
  if (count > winCap) {
    // Advanced v5.1: fixed short 34px 1px thumb at x470, no track.
    // Every other page keeps its restrained proportional thumb.
    const int barW = isAdvanced ? 1 : 4;
    const int barX = isAdvanced ? 470 : 468;
    const int trackY = isAdvanced ? kM4SettingsAdvancedOriginY : kM4SettingsL2OriginY;
    const int itemH = m4SettingsUiListItemH(ui_);
    const int itemGap = m4SettingsUiListItemGap(ui_);
    const int trackH = winCap * itemH + (winCap - 1) * itemGap;
    // v5.1 spec short thumb; Y stays proportional to windowStart below.
    int thumbH = isAdvanced ? 34 : (trackH * 8) / count;
    if (thumbH < 24) thumbH = 24;
    if (thumbH > trackH) thumbH = trackH;
    const int maxStart = count - winCap;
    int thumbY = trackY;
    if (maxStart > 0) thumbY = trackY + ((trackH - thumbH) * ui_.windowStart) / maxStart;
    // Restrained indicator: thumb only, no track line.
    renderer.fillRect(barX, thumbY, barW, thumbH, true);
  }

  // Advanced groups, paint-only. Group intervals live in
  // kM4SettingsAdvancedGroupStarts (shared with touch/tests); order/count/
  // selection/window math never see groups.
  // Group names reuse the real subtitle classification (no invented titles).
  static constexpr const char* kAdvGroupNames[] = {"显示与刷新", "图标与文字", "动画",
                                                   "连接与同步", "系统"};
  if (isAdvanced) {
    // Group-layout row paint (layout-v5.1-advanced): the scene repeat is
    // empty for this page (see rebuildModel) and rows are painted here at
    // the shared Y table with the exact l2 row geometry (436x52 pitch,
    // title/value/divider/chevron rects, NOTOSANS_18/14 faces). Boundary
    // slots also draw the new group's section title in the 42px gap.
    const int advSlots = count - ui_.windowStart < 9 ? count - ui_.windowStart : 9;
    for (int i = 0; i < advSlots; ++i) {
      const auto& row = snap.window[i];
      if (!row.isRow) continue;
      const int ry = m4SettingsAdvancedRowY(ui_.windowStart, i);
      constexpr int kRowX = 22;
      // v5.3 B: stipple field only; the card already draws the border.
      if (row.selected) {
        renderer.fillRectStipple(kRowX + 8, ry + 8, 420, 36);
      }
      char titleBuf[256]{};
      char valueBuf[256]{};
      auto copyRef = [&](SettingsScene::SettingsTextRef ref, char* out) {
        auto tv = snap.textView(ref);
        size_t n = tv.size < 255 ? tv.size : 255;
        for (size_t k = 0; k < n; ++k) out[k] = static_cast<char>(tv.readByte(static_cast<uint16_t>(k)));
      };
      copyRef(row.title, titleBuf);
      copyRef(row.value, valueBuf);
      drawAdvancedRowText(renderer, NOTOSANS_18_FONT_ID, kRowX + 18, ry + 18, 200, titleBuf, false);
      drawAdvancedRowText(renderer, NOTOSANS_14_FONT_ID, kRowX + 222, ry + 20, 174, valueBuf, true);
      renderer.drawLine(kRowX + 18, ry + 51, kRowX + 424, ry + 51, true);
      if (row.navigates) {
        renderer.drawLine(kRowX + 418, ry + 21, kRowX + 423, ry + 26, 2, true);
        renderer.drawLine(kRowX + 423, ry + 26, kRowX + 418, ry + 31, 2, true);
      }
      if (m4SettingsAdvancedSlotStartsGroup(ui_.windowStart, i)) {
        const int g = m4SettingsAdvancedGroupOf(ui_.windowStart + i);
        const int baseline = ry - kM4SettingsAdvancedSectionGap +
                             kM4SettingsAdvancedSectionTitleBaselineDy;
        const int top = baseline - renderer.getLineHeight(UI_12_FONT_ID);
        M4UiText::draw(renderer, UI_12_FONT_ID, 24, top, kAdvGroupNames[g], true, EpdFontFamily::BOLD);
      }
    }
  }
  const int advVisible = count - ui_.windowStart < 9 ? count - ui_.windowStart : 9;
  if (isAdvanced && ui_.windowStart == 0) {
    // First-screen legend: G0's section + card (v5.1: 5 rows x 52px).
    const int sectionTop = 116 - renderer.getLineHeight(UI_12_FONT_ID);
    M4UiText::draw(renderer, UI_12_FONT_ID, 24, sectionTop, "阅读与刷新", true, EpdFontFamily::BOLD);
    renderer.drawRoundedRect(22, 126, 436, 260, 1, 10, true);
  }

  // Sticky group title for scrolled windows, paint-only: the y92-126 zone
  // is always empty (rows start at 126), so the top visible row's real group
  // is named there without moving any row.
  if (isAdvanced && ui_.windowStart > 0) {
    int g = m4SettingsAdvancedGroupOf(ui_.windowStart);
    const int sectionTop = 116 - renderer.getLineHeight(UI_12_FONT_ID);
    M4UiText::draw(renderer, UI_12_FONT_ID, 24, sectionTop, kAdvGroupNames[g], true,
                   EpdFontFamily::BOLD);
  }

  if (isAdvanced) {
    // One shared light container per visible group segment, clipped to the
    // viewport by construction (only visible slots): corners round only
    // where the group truly starts/ends. G0 at windowStart==0 is covered
    // by the legend card above, so it is skipped, never double-drawn.
    for (int g = 0; g < kM4SettingsAdvancedGroupCount; ++g) {
      const int gs = kM4SettingsAdvancedGroupStarts[g];
      const int ge = (g + 1 < kM4SettingsAdvancedGroupCount) ? kM4SettingsAdvancedGroupStarts[g + 1] : count;
      const int s0 = gs > ui_.windowStart ? gs : ui_.windowStart;
      const int end = ui_.windowStart + advVisible;
      const int s1 = ge < end ? ge : end;
      if (s0 >= s1) continue;
      if (ui_.windowStart == 0 && g == 0) continue;
      const int y0 = m4SettingsAdvancedRowY(ui_.windowStart, s0 - ui_.windowStart);
      const int y1 =
          m4SettingsAdvancedRowY(ui_.windowStart, s1 - ui_.windowStart - 1) + kM4SettingsAdvancedItemH;
      const bool rt = (s0 == gs);
      const bool rb = (s1 == ge);
      renderer.drawRoundedRect(22, y0, 436, y1 - y0, 1, 12, rt, rt, rb, rb, true);
    }
  }

  // Apple-minimal: no persistent footer/hint bar on normal list pages.
  // Physical keys keep working via mappedInput; only the Confirm page
  // (dangerous boot-slot switch above) retains one-time button hints.
  submitDisplay(req);
}
