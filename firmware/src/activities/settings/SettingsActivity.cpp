#include "SettingsActivity.h"

#include <algorithm>
#include <GfxRenderer.h>
#include <HardwareSerial.h>

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
#endif
#include <HalPowerManager.h>
#include <BluetoothHIDManager.h>
#include "activities/reader/EpubReaderSettingsActivity.h"

#include "SettingsLists.h"
#include "activities/settings/SettingsHubPolicy.h"
#include "activities/settings/SettingsSceneModel.h"
#include "ui/scene/GfxSceneRenderer.h"
#include "ui/scene/UiSceneRuntime.h"
#include "generated/murphy_settings_hub_m4theme.h"
#include "generated/murphy_settings_l2_m4theme.h"

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
}  // namespace

void SettingsActivity::taskTrampoline(void* param) {
  auto* self = static_cast<SettingsActivity*>(param);
  self->displayTaskLoop();
}

void SettingsActivity::onEnter() {
  Activity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();

  displayReadingSettings_.clear();
  keysSettings_.clear();
  networkSettings_.clear();
  systemSettings_.clear();

  for (auto& setting : getSettingsList()) {
    if (!setting.key) continue;
    // Bucket via Hub policy (M4)
    if (settingsHubContainsKey(SettingsHubCard::DisplayReading, setting.key, true, false)) {
      displayReadingSettings_.push_back(std::move(setting));
    } else if (settingsHubContainsKey(SettingsHubCard::KeysOperations, setting.key, true, false)) {
      keysSettings_.push_back(std::move(setting));
    } else if (settingsHubContainsKey(SettingsHubCard::NetworkSync, setting.key, true, false)) {
      networkSettings_.push_back(std::move(setting));
    } else if (settingsHubContainsKey(SettingsHubCard::SystemMaintenance, setting.key, true, false)) {
      systemSettings_.push_back(std::move(setting));
    }
    // Web-only categories (Reader, KOReader Sync, OPDS etc.) are ignored
  }

  // Append device-only ACTION items per Hub
  {
    auto act = SettingInfo::Action(L(Str::kReaderLayout));
    act.key = "readerLayout";
    displayReadingSettings_.push_back(std::move(act));
  }
  {
    auto act = SettingInfo::Action(L(Str::kRemapFrontButtons));
    act.key = "remapButtons";
    // Ensure remapButtons is first in Keys
    keysSettings_.insert(keysSettings_.begin(), std::move(act));
  }
  {
    auto act = SettingInfo::Action(L(Str::kBluetoothSettings));
    act.key = "bluetooth";
    networkSettings_.push_back(std::move(act));
  }
  {
    auto act = SettingInfo::Action(L(Str::kKOReaderSync));
    act.key = "koreader";
    networkSettings_.push_back(std::move(act));
  }
  {
    auto act = SettingInfo::Action(L(Str::kJianGuoConfig));
    act.key = "jianguo";
    networkSettings_.push_back(std::move(act));
  }
  {
    auto act = SettingInfo::Action(L(Str::kDataCapsuleConfig));
    act.key = "dataCapsule";
    networkSettings_.push_back(std::move(act));
  }
  {
    auto act = SettingInfo::Action(L(Str::kClearCache));
    act.key = "clearCache";
    systemSettings_.push_back(std::move(act));
  }
  {
    auto act = SettingInfo::Action(L(Str::kResetSettings));
    act.key = "resetSettings";
    systemSettings_.push_back(std::move(act));
  }
#ifdef CROSSPOINT_MURPHY_M4
  {
    auto act = SettingInfo::Action(L(Str::kDeveloperOptions));
    act.key = "developerOptions";
    systemSettings_.push_back(std::move(act));
  }
  {
    auto act = SettingInfo::Action(L(Str::kSwitchBootSlot));
    act.key = "switchBootSlot";
    systemSettings_.push_back(std::move(act));
  }
#endif

  navState_ = SettingsNavState{};
  navState_.pane = initialPane_;
  navState_.hub = initialHub_;
  navState_.selectedRow = 0;
  navState_.windowStart = 0;

  rebuildModel();
  updateRequired = true;

  xTaskCreate(&SettingsActivity::taskTrampoline, "SettingsActivityTask",
              8192, this, 1, &displayTaskHandle);
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

void SettingsActivity::loop() {
  if (subActivity) {
    if (pumpSubActivityFrame()) updateRequired = true;
    return;
  }

  // Back gesture / Back button
  if (mappedInput.hasTouch() && mappedInput.wasBackGesture()) {
    if (navState_.pane == SettingsPane::Category) {
      navState_ = settingsNavBack(navState_);
      rebuildModel();
      updateRequired = true;
      return;
    } else {
      SETTINGS.saveToFile();
      EpdFontLoader::loadFontsFromSd(renderer);
      onGoHome();
      return;
    }
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (navState_.pane == SettingsPane::Category) {
      navState_ = settingsNavBack(navState_);
      rebuildModel();
      updateRequired = true;
      return;
    } else {
      SETTINGS.saveToFile();
      EpdFontLoader::loadFontsFromSd(renderer);
      onGoHome();
      return;
    }
  }

  // Touch handling via scene hitTest
  if (mappedInput.hasTouch()) {
    // Swipe paging for L2 (vertical)
    auto swipe = mappedInput.wasSwipe();
    if (navState_.pane == SettingsPane::Category && currentHubSettingCount() > 0 &&
        (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down)) {
      const int pageItems = kSettingsL2Window;
      const int count = currentHubSettingCount();
      const bool pageDown = swipe == MappedInputManager::SwipeDir::Up;
      int next = M4ListTouchPolicy::applyPage(navState_.selectedRow, count, pageItems, pageDown);
      navState_.selectedRow = next;
      navState_ = settingsNavSyncWindow(navState_, navState_.hub, true);
      rebuildModel();
      updateRequired = true;
      return;
    }

    int tx = 0, ty = 0;
    bool tapped = mappedInput.wasScreenTapped(tx, ty);
    bool down = mappedInput.wasScreenTouchDown(tx, ty);
    if (tapped || down) {
      int x = tapped ? tx : tx;
      int y = tapped ? ty : ty;
      // Build source for hitTest from current model snapshot
      SettingsScene::SettingsSnapshot snap{};
      if (sceneModel_.copyLatest(snap)) {
        auto src = SettingsScene::SettingsSceneModel::bindingSource(snap);
        UiSceneRuntime::HitResult hit{};
        const uint8_t* pkg = nullptr;
        size_t len = 0;
        if (navState_.pane == SettingsPane::Hub) {
          pkg = murphy_settings_hub_m4theme;
          len = murphy_settings_hub_m4theme_len;
        } else {
          pkg = murphy_settings_l2_m4theme;
          len = murphy_settings_l2_m4theme_len;
        }
        if (UiSceneRuntime::hitTestScene(pkg, len, src, x, y, &hit) && hit.hit) {
          if (navState_.pane == SettingsPane::Hub && hit.action == SettingsScene::kActionOpenHubCard) {
            SettingsHubCard card = static_cast<SettingsHubCard>(hit.item.index);
            if (hit.item.index < kSettingsHubCardCount) {
              navState_.hub = card;
              openHubCard(card);
              updateRequired = true;
              return;
            }
          } else if (navState_.pane == SettingsPane::Category && hit.action == SettingsScene::kActionActivateSetting) {
            int windowIndex = hit.item.index;
            if (windowIndex >=0 && windowIndex < (int)kSettingsL2Window) {
              if (tapped) {
                handleL2TapIndex(windowIndex);
                updateRequired = true;
                return;
              } else {
                // touchDown select only
                int flatIndex = navState_.windowStart + windowIndex;
                int flatCount = settingsFlatCount(navState_.hub, true);
                if (flatIndex < flatCount) {
                  auto fr = settingsFlatAt(navState_.hub, flatIndex, true);
                  if (fr.kind == SettingsFlatKind::Setting) {
                    // Find settingIndex for this key
                    const auto& vec = currentHubSettings();
                    for (size_t i=0;i<vec.size();++i) if (vec[i].key && strcmp(vec[i].key, fr.key)==0) {
                      if ((int)i != navState_.selectedRow) {
                        navState_.selectedRow = (int)i;
                        navState_ = settingsNavSyncWindow(navState_, navState_.hub, true);
                        rebuildModel();
                        updateRequired = true;
                      }
                      break;
                    }
                  }
                }
                return;
              }
            }
          }
        }
      }
    }
  }

  // Physical keys
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (navState_.pane == SettingsPane::Hub) {
      handleHubConfirm();
      updateRequired = true;
      return;
    } else {
      handleL2Confirm();
      updateRequired = true;
      return;
    }
  }

  const bool up = mappedInput.wasReleased(MappedInputManager::Button::Up);
  const bool down = mappedInput.wasReleased(MappedInputManager::Button::Down);
  const bool left = mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool right = mappedInput.wasReleased(MappedInputManager::Button::Right);

  if (navState_.pane == SettingsPane::Hub) {
    if (up || left) {
      navState_ = settingsNavMoveHub(navState_, -1);
      rebuildModel();
      updateRequired = true;
    } else if (down || right) {
      navState_ = settingsNavMoveHub(navState_, 1);
      rebuildModel();
      updateRequired = true;
    }
  } else {
    int count = currentHubSettingCount();
    if (count>0 && (up || left)) {
      navState_ = settingsNavMoveRow(navState_, -1, count);
      navState_ = settingsNavSyncWindow(navState_, navState_.hub, true);
      rebuildModel();
      updateRequired = true;
    } else if (count>0 && (down || right)) {
      navState_ = settingsNavMoveRow(navState_, 1, count);
      navState_ = settingsNavSyncWindow(navState_, navState_.hub, true);
      rebuildModel();
      updateRequired = true;
    }
  }
}

void SettingsActivity::rebuildModel() {
  sceneModel_.begin(UiScene::DataState::Ready);
  // Battery
  uint16_t bat = 0;
  // HalPowerManager may not be available on host; guard.
#ifdef CROSSPOINT_MURPHY_M4
  bat = powerManager.getBatteryPercentage();
#else
  bat = 80;
#endif
  sceneModel_.setBattery(bat);
  sceneModel_.setPane(navState_.pane);
  sceneModel_.setHub(navState_.hub);
  if (navState_.pane == SettingsPane::Hub) {
    sceneModel_.setPageTitle(L(Str::kSystemSettings));
    sceneModel_.populateHubFromPolicy();
    // Ensure hub selection is reflected (populateHubFromPolicy sets titles, selected via resolve)
  } else {
    const char* title = settingsHubCardTitleZh(navState_.hub);
    sceneModel_.setPageTitle(title);
    // Build window rows with real values
    sceneModel_.clearWindow();
    int flatCount = settingsFlatCount(navState_.hub, true);
    const auto& vec = currentHubSettings();
    for (int i=0;i<(int)SettingsScene::kMaxWindowRows;++i) {
      int flatIndex = navState_.windowStart + i;
      if (flatIndex < flatCount) {
        auto fr = settingsFlatAt(navState_.hub, flatIndex, true);
        if (fr.kind == SettingsFlatKind::Section) {
          // Resolve section title via I18n where possible, else policy title
          const char* secTitle = fr.titleZh;
          // Try to use I18n for known sections
          if (fr.section == 0 && navState_.hub == SettingsHubCard::DisplayReading) secTitle = L(Str::kSectionUiChrome);
          else if (fr.section == 1 && navState_.hub == SettingsHubCard::DisplayReading) secTitle = L(Str::kSectionEinkRefresh);
          else if (fr.section == 2 && navState_.hub == SettingsHubCard::DisplayReading) secTitle = L(Str::kSectionFrontlight);
          else if (fr.section == 3 && navState_.hub == SettingsHubCard::DisplayReading) secTitle = L(Str::kSectionReaderDoor);
          else if (fr.section == 4 && navState_.hub == SettingsHubCard::DisplayReading) secTitle = L(Str::kSectionPageTurn);
          else if (fr.section == 0 && navState_.hub == SettingsHubCard::NetworkSync) secTitle = L(Str::kSectionNetworkToggles);
          else if (fr.section == 1 && navState_.hub == SettingsHubCard::NetworkSync) secTitle = L(Str::kSectionSyncDoorways);
          else if (fr.section == 0 && navState_.hub == SettingsHubCard::SystemMaintenance) secTitle = L(Str::kSectionSystem);
          else if (fr.section == 1 && navState_.hub == SettingsHubCard::SystemMaintenance) secTitle = L(Str::kSectionMaintenance);
          sceneModel_.setWindowRow(i, "", secTitle, "", true, false);
        } else {
          // Setting row
          const SettingInfo* info = nullptr;
          for (auto &s : vec) if (s.key && strcmp(s.key, fr.key)==0) { info = &s; break; }
          std::string value;
          const char* title = fr.key;
          if (info) {
            title = info->name;
            value = valueTextForSetting(*info);
          } else {
            title = fr.titleZh ? fr.titleZh : fr.key;
          }
          int flatOfSelected = settingsFlatIndexOfSetting(navState_.hub, navState_.selectedRow, true);
          bool selected = (flatIndex == flatOfSelected);
          sceneModel_.setWindowRow(i, fr.key, title, value.c_str(), false, selected);
        }
      } else {
        sceneModel_.setWindowRow(i, "", "", "", false, false);
        // ensure isRow false for empty
      }
    }
  }
  sceneModel_.publish();
}

const std::vector<SettingInfo>& SettingsActivity::currentHubSettings() const {
  switch (navState_.hub) {
    case SettingsHubCard::DisplayReading: return displayReadingSettings_;
    case SettingsHubCard::KeysOperations: return keysSettings_;
    case SettingsHubCard::NetworkSync: return networkSettings_;
    case SettingsHubCard::SystemMaintenance: return systemSettings_;
    default: return displayReadingSettings_;
  }
}
std::vector<SettingInfo>& SettingsActivity::currentHubSettings() {
  switch (navState_.hub) {
    case SettingsHubCard::DisplayReading: return displayReadingSettings_;
    case SettingsHubCard::KeysOperations: return keysSettings_;
    case SettingsHubCard::NetworkSync: return networkSettings_;
    case SettingsHubCard::SystemMaintenance: return systemSettings_;
    default: return displayReadingSettings_;
  }
}
int SettingsActivity::currentHubSettingCount() const {
  return static_cast<int>(currentHubSettings().size());
}
const SettingInfo* SettingsActivity::findSettingByKey(const char* key) const {
  if (!key) return nullptr;
  const auto& vec = currentHubSettings();
  for (auto &s : vec) if (s.key && strcmp(s.key, key)==0) return &s;
  return nullptr;
}
std::string SettingsActivity::valueTextForSetting(const SettingInfo& info) const {
  std::string valueText;
  if (info.type == SettingType::TOGGLE && info.valuePtr != nullptr) {
    bool v = SETTINGS.*(info.valuePtr);
    valueText = v ? L(Str::kOn) : L(Str::kOff);
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
    if (info.key && strcmp(info.key, "lineSpacing")==0) {
      valueText = std::to_string(v/10) + "." + std::to_string(v%10) + L(Str::kValTimes);
    } else if (info.key && strcmp(info.key, "refreshFrequency")==0) {
      valueText = std::to_string(v) + L(Str::kValPagesFullRefresh);
    } else {
      valueText = std::to_string(v);
    }
  } else if (info.type == SettingType::ACTION && info.key && strcmp(info.key, "bluetooth")==0) {
    try { auto &bt = BluetoothHIDManager::getInstance(); valueText = bt.isEnabled() ? L(Str::kOn) : L(Str::kOff); } catch(...) { valueText = L(Str::kError); }
  } else if (info.type == SettingType::ACTION && info.key && strcmp(info.key, "switchBootSlot")==0) {
#ifdef CROSSPOINT_MURPHY_M4
    valueText = runningOtaLabel();
#endif
  }
  return valueText;
}
void SettingsActivity::toggleCurrentSetting() {
  int idx = navState_.selectedRow;
  auto &vec = currentHubSettings();
  if (idx <0 || idx >= (int)vec.size()) return;
  const auto &setting = vec[idx];
  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    bool cur = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !cur;
    SETTINGS.saveToFile();
    rebuildModel();
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    size_t n = setting.enumValues.size();
    if (n==0) return;
    uint8_t cur = SETTINGS.*(setting.valuePtr);
    if (cur >= n) cur = 0;
    SETTINGS.*(setting.valuePtr) = (cur+1)%n;
    SETTINGS.saveToFile();
    rebuildModel();
  } else if (setting.type == SettingType::ENUM && setting.valueGetter && setting.valueSetter) {
    size_t n = setting.enumValues.size();
    if (n==0) return;
    uint8_t cur = setting.valueGetter();
    uint8_t safe = cur < n ? cur : 0;
    setting.valueSetter((safe+1)%n);
#ifdef CROSSPOINT_MURPHY_M4
    if (setting.key && strcmp(setting.key, "uiFontSize")==0) {
      EpdFontLoader::applySystemChrome(renderer);
    }
#endif
    rebuildModel();
  } else if (setting.type == SettingType::VALUE && setting.signedValuePtr != nullptr) {
    int cur = SETTINGS.*(setting.signedValuePtr);
    NumberSelectionActivity::Config cfg;
    cfg.title = vec[idx].name;
    cfg.minValue = setting.valueRange.min;
    cfg.maxValue = setting.valueRange.max;
    cfg.smallStep = setting.valueRange.step;
    cfg.largeStep = setting.valueRange.step*5;
    cfg.isSigned = true;
    auto ptr = setting.signedValuePtr;
    int savedRow = navState_.selectedRow;
    SettingsHubCard savedHub = navState_.hub;
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    exitActivity();
    enterNewActivity(new NumberSelectionActivity(renderer, mappedInput, cfg, cur,
      [this, ptr, savedRow, savedHub](int v){
        SETTINGS.*(ptr) = (int8_t)v;
        SETTINGS.saveToFile();
        exitActivity();
        navState_.selectedRow = savedRow;
        navState_.hub = savedHub;
        navState_ = settingsNavSyncWindow(navState_, navState_.hub, true);
        rebuildModel();
        updateRequired = true;
      },
      [this, savedRow, savedHub](){
        exitActivity();
        navState_.selectedRow = savedRow;
        navState_.hub = savedHub;
        navState_ = settingsNavSyncWindow(navState_, navState_.hub, true);
        rebuildModel();
        updateRequired = true;
      }));
    xSemaphoreGive(renderingMutex);
    return;
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    int cur = SETTINGS.*(setting.valuePtr);
    NumberSelectionActivity::Config cfg;
    cfg.title = vec[idx].name;
    cfg.minValue = setting.valueRange.min;
    cfg.maxValue = setting.valueRange.max;
    cfg.smallStep = setting.valueRange.step;
    cfg.largeStep = setting.valueRange.step*5;
    cfg.isSigned = false;
    if (strcmp(setting.name, L(Str::kRefreshFrequency))==0) {
      cfg.displayFormatter = [](int v){ return std::to_string(v)+L(Str::kValPagesFullRefresh); };
    } else if (strcmp(setting.name, L(Str::kLineSpacing))==0) {
      cfg.displayFormatter = [](int v){ return std::to_string(v/10)+"."+std::to_string(v%10)+L(Str::kValTimes); };
    }
    auto ptr = setting.valuePtr;
    int savedRow = navState_.selectedRow;
    SettingsHubCard savedHub = navState_.hub;
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    exitActivity();
    enterNewActivity(new NumberSelectionActivity(renderer, mappedInput, cfg, cur,
      [this, ptr, savedRow, savedHub](int v){
        SETTINGS.*(ptr) = (uint8_t)v;
        SETTINGS.saveToFile();
        exitActivity();
        navState_.selectedRow = savedRow;
        navState_.hub = savedHub;
        navState_ = settingsNavSyncWindow(navState_, navState_.hub, true);
        rebuildModel();
        updateRequired = true;
      },
      [this, savedRow, savedHub](){
        exitActivity();
        navState_.selectedRow = savedRow;
        navState_.hub = savedHub;
        navState_ = settingsNavSyncWindow(navState_, navState_.hub, true);
        rebuildModel();
        updateRequired = true;
      }));
    xSemaphoreGive(renderingMutex);
    return;
  } else if (setting.type == SettingType::ACTION) {
    const char* k = setting.key ? setting.key : "";
    if (strcmp(k, "remapButtons")==0) {
      int savedRow = navState_.selectedRow; SettingsHubCard savedHub = navState_.hub;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new ButtonRemapActivity(renderer, mappedInput, [this, savedRow, savedHub]{
        exitActivity();
        navState_.selectedRow = savedRow; navState_.hub = savedHub;
        navState_ = settingsNavSyncWindow(navState_, navState_.hub, true);
        rebuildModel(); updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(k, "readerLayout")==0) {
      int savedRow = navState_.selectedRow; SettingsHubCard savedHub = navState_.hub;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new EpubReaderSettingsActivity(renderer, mappedInput, [this, savedRow, savedHub]{
        exitActivity();
        navState_.selectedRow = savedRow; navState_.hub = savedHub;
        navState_ = settingsNavSyncWindow(navState_, navState_.hub, true);
        rebuildModel(); updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(k, "bluetooth")==0) {
      int savedRow = navState_.selectedRow; SettingsHubCard savedHub = navState_.hub;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new SimpleBluetoothActivity(renderer, mappedInput, [this, savedRow, savedHub]{
        exitActivity();
        navState_.selectedRow = savedRow; navState_.hub = savedHub;
        navState_ = settingsNavSyncWindow(navState_, navState_.hub, true);
        rebuildModel(); updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(k, "koreader")==0) {
      int savedRow = navState_.selectedRow; SettingsHubCard savedHub = navState_.hub;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new KOReaderSettingsActivity(renderer, mappedInput, [this, savedRow, savedHub]{
        exitActivity();
        navState_.selectedRow = savedRow; navState_.hub = savedHub;
        navState_ = settingsNavSyncWindow(navState_, navState_.hub, true);
        rebuildModel(); updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(k, "jianguo")==0) {
      int savedRow = navState_.selectedRow; SettingsHubCard savedHub = navState_.hub;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new JianGuoYunSettingsActivity(renderer, mappedInput, [this, savedRow, savedHub]{
        exitActivity();
        navState_.selectedRow = savedRow; navState_.hub = savedHub;
        navState_ = settingsNavSyncWindow(navState_, navState_.hub, true);
        rebuildModel(); updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(k, "dataCapsule")==0) {
      int savedRow = navState_.selectedRow; SettingsHubCard savedHub = navState_.hub;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new DataCapsuleSettingsActivity(renderer, mappedInput, [this, savedRow, savedHub]{
        exitActivity();
        navState_.selectedRow = savedRow; navState_.hub = savedHub;
        navState_ = settingsNavSyncWindow(navState_, navState_.hub, true);
        rebuildModel(); updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(k, "clearCache")==0) {
      int savedRow = navState_.selectedRow; SettingsHubCard savedHub = navState_.hub;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new ClearCacheActivity(renderer, mappedInput, [this, savedRow, savedHub]{
        exitActivity();
        navState_.selectedRow = savedRow; navState_.hub = savedHub;
        navState_ = settingsNavSyncWindow(navState_, navState_.hub, true);
        rebuildModel(); updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(k, "resetSettings")==0) {
      int savedRow = navState_.selectedRow; SettingsHubCard savedHub = navState_.hub;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new ResetSettingsActivity(renderer, mappedInput, [this, savedRow, savedHub]{
        exitActivity();
        navState_.selectedRow = savedRow; navState_.hub = savedHub;
        navState_ = settingsNavSyncWindow(navState_, navState_.hub, true);
        rebuildModel(); updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
#ifdef CROSSPOINT_MURPHY_M4
    } else if (strcmp(k, "developerOptions")==0) {
      int savedRow = navState_.selectedRow; SettingsHubCard savedHub = navState_.hub;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new DeveloperOptionsActivity(renderer, mappedInput, [this, savedRow, savedHub]{
        exitActivity();
        navState_.selectedRow = savedRow; navState_.hub = savedHub;
        navState_ = settingsNavSyncWindow(navState_, navState_.hub, true);
        rebuildModel(); updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(k, "switchBootSlot")==0) {
      const auto* running = runningOtaPartition();
      const char* target = (running && running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) ? L(Str::kApp1Custom) : L(Str::kApp0Official);
      if (switchToOtherOtaSlot()) {
        GUI.drawPopup(renderer, target);
        delay(350);
        ESP.restart();
      } else {
        GUI.drawPopup(renderer, L(Str::kUnknownBootSlot));
        delay(700);
        rebuildModel();
        updateRequired = true;
      }
#endif
    } else if (strcmp(k, "bluetooth")==0 || strcmp(k, "resetSettings")==0) {
      // handled
    }
  } else {
    return;
  }
  SETTINGS.saveToFile();
  rebuildModel();
}

void SettingsActivity::openHubCard(SettingsHubCard card) {
  navState_ = settingsNavOpenCard(navState_, card);
  navState_ = settingsNavSyncWindow(navState_, card, true);
  rebuildModel();
}
void SettingsActivity::handleHubConfirm() {
  openHubCard(navState_.hub);
}
void SettingsActivity::handleL2Confirm() {
  toggleCurrentSetting();
}
void SettingsActivity::handleL2TapIndex(int windowIndex) {
  int flatIndex = navState_.windowStart + windowIndex;
  int flatCount = settingsFlatCount(navState_.hub, true);
  if (flatIndex <0 || flatIndex >= flatCount) return;
  auto fr = settingsFlatAt(navState_.hub, flatIndex, true);
  if (fr.kind == SettingsFlatKind::Section) return;
  // Find settingIndex for this key
  const auto& vec = currentHubSettings();
  for (size_t i=0;i<vec.size();++i) if (vec[i].key && strcmp(vec[i].key, fr.key)==0) {
    navState_.selectedRow = (int)i;
    navState_ = settingsNavSyncWindow(navState_, navState_.hub, true);
    rebuildModel();
    toggleCurrentSetting();
    return;
  }
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
  renderer.clearScreen();
  SettingsScene::SettingsSnapshot snap{};
  bool hasSnap = sceneModel_.copyLatest(snap);
  if (!hasSnap) {
    // fallback to empty
    snap.state = UiScene::DataState::Loading;
  }
  auto src = SettingsScene::SettingsSceneModel::bindingSource(snap);
  UiScene::UiSceneAssets assets;
  assets.clear();
  UiScene::GfxSceneRenderer r;
  if (navState_.pane == SettingsPane::Hub) {
    r.render(murphy_settings_hub_m4theme, murphy_settings_hub_m4theme_len, src, assets, renderer);
  } else {
    r.render(murphy_settings_l2_m4theme, murphy_settings_l2_m4theme_len, src, assets, renderer);
    // Right-edge scroll progress (same grammar as FengyanTheme::drawList).
    // Theme JSON stays clear of progress nodes; overlay sits in the 24px margin
    // past the L2 repeat (x=24..456) so value text is not clipped.
    const int flatCount = settingsFlatCount(navState_.hub, true);
    if (flatCount > kSettingsL2Window) {
      constexpr int kBarW = 4;
      constexpr int kBarX = 468;
      const int trackY = kSettingsContentTop;
      const int trackH =
          kSettingsL2Window * kSettingsL2ItemH + (kSettingsL2Window - 1) * kSettingsL2Gap;
      int thumbH = (trackH * kSettingsL2Window) / flatCount;
      if (thumbH < 24) thumbH = 24;
      if (thumbH > trackH) thumbH = trackH;
      const int maxStart = flatCount - kSettingsL2Window;
      int thumbY = trackY;
      if (maxStart > 0) {
        thumbY = trackY + ((trackH - thumbH) * navState_.windowStart) / maxStart;
      }
      renderer.drawLine(kBarX + kBarW / 2, trackY, kBarX + kBarW / 2, trackY + trackH, true);
      renderer.fillRect(kBarX, thumbY, kBarW, thumbH, true);
    }
  }
  const auto labels = mappedInput.mapLabels(L(Str::kBack), L(Str::kConfirm), L(Str::kUp), L(Str::kDown));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
