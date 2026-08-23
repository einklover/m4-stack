#include "SettingsActivity.h"

#include <algorithm>
#include <GfxRenderer.h>
#include <HardwareSerial.h>

#include "ButtonRemapActivity.h"
#include "util/M4ListTouchPolicy.h"
#include "util/TouchHitGeometry.h"
#include "CalibreSettingsActivity.h"
#include "ClearCacheActivity.h"
#include "CrossPointSettings.h"
#include "I18n.h"
#include "KOReaderSettingsActivity.h"
#include "MappedInputManager.h"
#include "NumberSelectionActivity.h"
#include "OtaUpdateActivity.h"
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
#include <SDCardManager.h>

#include "SettingsLists.h"

const char* SettingsActivity::categoryNames[categoryCount] = {"Display", "Controls", "System"};

namespace {
constexpr int changeTabsMs = 700;

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

  // Build per-category vectors from the shared settings list
  displaySettings.clear();
  readerSettings.clear();
  controlsSettings.clear();
  systemSettings.clear();

  for (auto& setting : getSettingsList()) {
    if (!setting.category) continue;
    if (strcmp(setting.category, "Display") == 0) {
      displaySettings.push_back(std::move(setting));
    } else if (strcmp(setting.category, "Controls") == 0) {
      controlsSettings.push_back(std::move(setting));
    } else if (strcmp(setting.category, "System") == 0) {
      systemSettings.push_back(std::move(setting));
    }
    // Web-only categories (Reader, KOReader Sync, OPDS Browser) are skipped for device UI
  }

  // Append device-only ACTION items (use key as stable identifier for matching)
  {
    auto act = SettingInfo::Action(L(Str::kRemapFrontButtons));
    act.key = "remapButtons";
    controlsSettings.insert(controlsSettings.begin(), std::move(act));
  }
  {
    auto act = SettingInfo::Action(L(Str::kBluetoothSettings));
    act.key = "bluetooth";
    systemSettings.push_back(std::move(act));
  }
  {
    auto act = SettingInfo::Action(L(Str::kKOReaderSync));
    act.key = "koreader";
    systemSettings.push_back(std::move(act));
  }
  {
    auto act = SettingInfo::Action(L(Str::kJianGuoConfig));
    act.key = "jianguo";
    systemSettings.push_back(std::move(act));
  }
  {
    auto act = SettingInfo::Action(L(Str::kDataCapsuleConfig));
    act.key = "dataCapsule";
    systemSettings.push_back(std::move(act));
  }
  {
    auto act = SettingInfo::Action(L(Str::kClearCache));
    act.key = "clearCache";
    systemSettings.push_back(std::move(act));
  }
  if (SdMan.exists("/update/firmware.bin")) {
    auto act = SettingInfo::Action(L(Str::kSdCardUpdate));
    act.key = "sdOta";
    systemSettings.push_back(std::move(act));
  }
  {
    auto act = SettingInfo::Action(L(Str::kResetSettings));
    act.key = "resetSettings";
    systemSettings.push_back(std::move(act));
  }
#ifdef CROSSPOINT_MURPHY_M4
  // Device-only: never added to getSettingsList() / web settings API.
  {
    auto act = SettingInfo::Action(L(Str::kDeveloperOptions));
    act.key = "developerOptions";
    systemSettings.push_back(std::move(act));
  }
  {
    auto act = SettingInfo::Action(L(Str::kSwitchBootSlot));
    act.key = "switchBootSlot";
    systemSettings.push_back(std::move(act));
  }
#endif


  // Reset selection to first category
  selectedCategoryIndex = 0;
  selectedSettingIndex = 0;

  // Initialize with first category (Display)
  currentSettings = &displaySettings;
  settingsCount = static_cast<int>(displaySettings.size());

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&SettingsActivity::taskTrampoline, "SettingsActivityTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void SettingsActivity::onExit() {
  ActivityWithSubactivity::onExit();

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;

  UITheme::getInstance().reload();  // Re-apply theme in case it was changed
}

void SettingsActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }
  bool hasChangedCategory = false;

  // Touch: tab bar + settings rows share metrics with render().
  if (mappedInput.hasTouch()) {
    if (mappedInput.wasBackGesture()) {
      SETTINGS.saveToFile();
      EpdFontLoader::loadFontsFromSd(renderer);
      onGoHome();
      return;
    }

    const auto metrics = UITheme::getInstance().getMetrics();
    const int pageHeight = renderer.getScreenHeight();
    const int tabTop = metrics.topPadding + metrics.headerHeight + 4;
    const int tabH = metrics.tabBarHeight;
    const int listTop = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
    const int listHeight =
        pageHeight - (metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.buttonHintsHeight +
                      metrics.verticalSpacing * 2);
    const int pageItems = std::max(1, listHeight / metrics.listRowHeight);

    // Exact geometry mirrors SettingsActivity::render() / FengyanTheme::drawTabBar:
    //   tabs are left-packed by label width (not equal screen thirds).
    //   list: y = topPadding+headerHeight+tabBarHeight+verticalSpacing
    // Swipe pages the list only (vertical). Horizontal was stealing edge/tab gestures.
    const auto swipe = mappedInput.wasSwipe();
    if (settingsCount > 0 &&
        (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down)) {
      const int listSel = selectedSettingIndex > 0 ? selectedSettingIndex - 1 : 0;
      const bool pageDown = swipe == MappedInputManager::SwipeDir::Up;
      const int next = M4ListTouchPolicy::applyPage(listSel, settingsCount, pageItems, pageDown);
      selectedSettingIndex = next + 1;
      updateRequired = true;
      return;
    }

    // Tab labels match render() (must stay in sync for hit geometry).
    const char* tabLabels[categoryCount] = {
        L(Str::kCategoryDisplay),
        L(Str::kCategoryControls),
        L(Str::kCategorySystem),
    };
    // FengyanTheme / LyraTheme both use hPaddingInSelection = 8 on tabs.
    constexpr int kTabHPad = 8;
    int tabTextWidths[categoryCount];
    for (int i = 0; i < categoryCount; ++i) {
      tabTextWidths[i] = M4UiText::textWidth(renderer, UI_10_FONT_ID, tabLabels[i]);
    }

    auto hitTab = [&](int tx, int ty, int& outTab) -> bool {
      return TouchHitGeometry::settingsTabFromPoint(tx, ty, tabTop, tabH, metrics.contentSidePadding,
                                                    metrics.tabSpacing, kTabHPad, tabTextWidths, categoryCount,
                                                    outTab);
    };

    int tx = 0;
    int ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty)) {
      int tab = -1;
      if (hitTab(tx, ty, tab)) {
        if (tab != selectedCategoryIndex) {
          selectedCategoryIndex = tab;
          hasChangedCategory = true;
          updateRequired = true;
        }
      } else {
        int sel = -1;
        if (TouchHitGeometry::settingsRowFromPoint(ty, listTop, listHeight, metrics.listRowHeight, settingsCount,
                                                   selectedSettingIndex, sel)) {
          selectedSettingIndex = sel;
          toggleCurrentSetting();
          updateRequired = true;
          return;
        }
      }
    }
    if (mappedInput.wasScreenTouchDown(tx, ty)) {
      int tab = -1;
      if (hitTab(tx, ty, tab)) {
        if (tab != selectedCategoryIndex) {
          selectedCategoryIndex = tab;
          hasChangedCategory = true;
          updateRequired = true;
        }
      } else {
        int sel = -1;
        if (ty >= listTop &&
            TouchHitGeometry::settingsRowFromPoint(ty, listTop, listHeight, metrics.listRowHeight, settingsCount,
                                                   selectedSettingIndex, sel)) {
          if (selectedSettingIndex != sel) {
            selectedSettingIndex = sel;
            updateRequired = true;
          }
        }
      }
    }
  }

  // Handle actions with early return
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedSettingIndex == 0) {
      selectedCategoryIndex = (selectedCategoryIndex < categoryCount - 1) ? (selectedCategoryIndex + 1) : 0;
      hasChangedCategory = true;
      updateRequired = true;
    } else {
      toggleCurrentSetting();
      updateRequired = true;
      return;
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    SETTINGS.saveToFile();
    EpdFontLoader::loadFontsFromSd(renderer);
    onGoHome();
    return;
  }

  const bool upReleased = mappedInput.wasReleased(MappedInputManager::Button::Up);
  const bool downReleased = mappedInput.wasReleased(MappedInputManager::Button::Down);
  const bool leftReleased = mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool rightReleased = mappedInput.wasReleased(MappedInputManager::Button::Right);
  const bool changeTab = mappedInput.getHeldTime() > changeTabsMs;

  // Handle navigation
  if (upReleased && changeTab) {
    hasChangedCategory = true;
    selectedCategoryIndex = (selectedCategoryIndex > 0) ? (selectedCategoryIndex - 1) : (categoryCount - 1);
    updateRequired = true;
  } else if (downReleased && changeTab) {
    hasChangedCategory = true;
    selectedCategoryIndex = (selectedCategoryIndex < categoryCount - 1) ? (selectedCategoryIndex + 1) : 0;
    updateRequired = true;
  } else if (upReleased || leftReleased) {
    selectedSettingIndex = (selectedSettingIndex > 0) ? (selectedSettingIndex - 1) : (settingsCount);
    updateRequired = true;
  } else if (rightReleased || downReleased) {
    selectedSettingIndex = (selectedSettingIndex < settingsCount) ? (selectedSettingIndex + 1) : 0;
    updateRequired = true;
  }

  if (hasChangedCategory) {
    selectedSettingIndex = (selectedSettingIndex == 0) ? 0 : 1;
    switch (selectedCategoryIndex) {
      case 0:  // Display
        currentSettings = &displaySettings;
        break;
      case 1:  // Controls
        currentSettings = &controlsSettings;
        break;
      case 2:  // System
        currentSettings = &systemSettings;
        break;
    }
     settingsCount = static_cast<int>(currentSettings->size());
  }
}

void SettingsActivity::toggleCurrentSetting() {
  int selectedSetting = selectedSettingIndex - 1;
  if (selectedSetting < 0 || selectedSetting >= settingsCount) {
    return;
  }

  const auto& setting = (*currentSettings)[selectedSetting];

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    // Toggle the boolean value using the member pointer
    const bool currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentValue;
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    // Treat the stored field as an index. Clamp first so a corrupt/out-of-range
    // value (or a non-index field bound by mistake) cannot walk off enumValues.
    const size_t n = setting.enumValues.size();
    if (n == 0) return;
    uint8_t currentValue = SETTINGS.*(setting.valuePtr);
    if (currentValue >= static_cast<uint8_t>(n)) currentValue = 0;
    SETTINGS.*(setting.valuePtr) = static_cast<uint8_t>((currentValue + 1) % n);
#ifdef CROSSPOINT_X3
    // 切换到全局生效时，自动开启晃动翻页
    if (setting.valuePtr == &CrossPointSettings::tiltScope &&
        SETTINGS.tiltScope == 1 && !SETTINGS.tiltPageTurnEnabled) {
      SETTINGS.tiltPageTurnEnabled = 1;
    }
#endif
  } else if (setting.type == SettingType::ENUM && setting.valueGetter && setting.valueSetter) {
    const size_t n = setting.enumValues.size();
    if (n == 0) return;
    const uint8_t currentIndex = setting.valueGetter();
    const uint8_t safe = (currentIndex < static_cast<uint8_t>(n)) ? currentIndex : 0;
    setting.valueSetter(static_cast<uint8_t>((safe + 1) % n));
} else if (setting.type == SettingType::VALUE && setting.signedValuePtr != nullptr) {
    // 有符号数值类型：打开数字选择器
    const int currentValue = SETTINGS.*(setting.signedValuePtr);
    NumberSelectionActivity::Config config;
    config.title = (*currentSettings)[selectedSetting].name;
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
    return;  // 不在这里保存，回调里保存
} else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    // 无符号数值类型：打开数字选择器
    const int currentValue = SETTINGS.*(setting.valuePtr);
    NumberSelectionActivity::Config config;
    config.title = (*currentSettings)[selectedSetting].name;
    config.minValue = setting.valueRange.min;
    config.maxValue = setting.valueRange.max;
    config.smallStep = setting.valueRange.step;
    config.largeStep = setting.valueRange.step * 5;
    config.isSigned = false;
    // 根据设置名称设置单位或格式化函数
    if (strcmp(setting.name, L(Str::kRefreshFrequency)) == 0) {
      config.displayFormatter = [](int v) -> std::string {
        return std::to_string(v) + L(Str::kValPagesFullRefresh);
      };
    } else if (strcmp(setting.name, L(Str::kLineSpacing)) == 0) {
      config.displayFormatter = [](int v) -> std::string {
        return std::to_string(v / 10) + "." + std::to_string(v % 10) + L(Str::kValTimes);
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
    return;  // 不在这里保存，回调里保存
} else if (setting.type == SettingType::ACTION) {
    // Use key (stable identifier) for ACTION matching, not name (which is translated)
    const char* actKey = setting.key ? setting.key : "";
    if (strcmp(actKey, "remapButtons") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new ButtonRemapActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(actKey, "koreader") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new KOReaderSettingsActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(actKey, "opds") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new CalibreSettingsActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
            }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(actKey, "jianguo") == 0) {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    exitActivity();
    enterNewActivity(new JianGuoYunSettingsActivity(renderer, mappedInput, [this] {
      exitActivity();
      updateRequired = true;
    }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(actKey, "dataCapsule") == 0) {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    exitActivity();
    enterNewActivity(new DataCapsuleSettingsActivity(renderer, mappedInput, [this] {
      exitActivity();
      updateRequired = true;
    }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(actKey, "clearCache") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new ClearCacheActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
#ifdef CROSSPOINT_MURPHY_M4
    } else if (strcmp(actKey, "developerOptions") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new DeveloperOptionsActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(actKey, "switchBootSlot") == 0) {
      // Only otadata is updated; the app images, partition table and NVS stay
      // untouched. Reboot into the other validated OTA app afterwards.
      const auto* running = runningOtaPartition();
      const char* target = (running && running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0)
                               ? L(Str::kApp1Custom)
                               : L(Str::kApp0Official);
      if (switchToOtherOtaSlot()) {
        GUI.drawPopup(renderer, target);
        delay(350);
        ESP.restart();
      } else {
        GUI.drawPopup(renderer, L(Str::kUnknownBootSlot));
        delay(700);
        updateRequired = true;
      }
#endif
    } else if (strcmp(actKey, "sdOta") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new SdOtaUpdateActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(actKey, "sdOta") == 0) {
      // duplicate sdOta key handled above
      ;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new SdOtaUpdateActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(actKey, "bluetooth") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new SimpleBluetoothActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(actKey, "resetSettings") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new ResetSettingsActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    }
  } else {
    return;
  }

  SETTINGS.saveToFile();
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

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  auto metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, L(Str::kSystemSettings));

  std::vector<TabInfo> tabs;
  tabs.reserve(categoryCount);
  for (int i = 0; i < categoryCount; i++) {
    const char* tabName;
    switch (i) {
      case 0: tabName = L(Str::kCategoryDisplay); break;
      case 1: tabName = L(Str::kCategoryControls); break;
      case 2: tabName = L(Str::kCategorySystem); break;
      default: tabName = ""; break;
    }
    tabs.push_back({tabName, selectedCategoryIndex == i});
  }
  GUI.drawTabBar(renderer, Rect{0, metrics.topPadding + metrics.headerHeight + 4, pageWidth, metrics.tabBarHeight}, tabs,
                 selectedSettingIndex == 0);


  const auto& settings = *currentSettings;        
  GUI.drawList(
      renderer,
      Rect{0, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing, pageWidth,
          pageHeight - (metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.buttonHintsHeight +
                        metrics.verticalSpacing * 2)},
      settingsCount, selectedSettingIndex - 1, 
      // 设置项名称：直接显示 name（已通过 L() 国际化）
      [&settings](int index) { 
          return std::string(settings[index].name); 
      },
      nullptr, nullptr,
      // 第二个回调：设置项值（国际化显示）
      [&settings](int i) {
        std::string valueText = "";
        if (settings[i].type == SettingType::TOGGLE && settings[i].valuePtr != nullptr) {
          const bool value = SETTINGS.*(settings[i].valuePtr);
          valueText = value ? L(Str::kOn) : L(Str::kOff);
        } else if (settings[i].type == SettingType::ENUM && settings[i].valuePtr != nullptr) {
          // Bound-check: raw SETTINGS fields must be indices into enumValues.
          // Binding a non-index field (e.g. LUT frame-rate byte 0x88) OOBs and
          // reboots when the System category is rendered.
          const uint8_t value = SETTINGS.*(settings[i].valuePtr);
          if (value < static_cast<uint8_t>(settings[i].enumValues.size())) {
            valueText = settings[i].enumValues[value];
          } else {
            valueText = "?";
          }
        } else if (settings[i].type == SettingType::ENUM && settings[i].valueGetter) {
          const uint8_t idx = settings[i].valueGetter();
          if (idx < static_cast<uint8_t>(settings[i].enumValues.size())) {
            valueText = settings[i].enumValues[idx];
          }
        } else if (settings[i].type == SettingType::VALUE && settings[i].signedValuePtr != nullptr) {
          valueText = std::to_string(static_cast<int>(SETTINGS.*(settings[i].signedValuePtr)));
        } else if (settings[i].type == SettingType::VALUE && settings[i].valuePtr != nullptr) {
          // 数值型：特殊处理行间距显示为X.X倍格式
          const int v = SETTINGS.*(settings[i].valuePtr);
          if (settings[i].key && strcmp(settings[i].key, "lineSpacing") == 0) {
            valueText = std::to_string(v / 10) + "." + std::to_string(v % 10) + L(Str::kValTimes);
          } else if (settings[i].key && strcmp(settings[i].key, "refreshFrequency") == 0) {
            valueText = std::to_string(v) + L(Str::kValPagesFullRefresh);
          } else {
            valueText = std::to_string(v);
          }
        } else if (settings[i].type == SettingType::ACTION &&
            settings[i].key && strcmp(settings[i].key, "bluetooth") == 0) {
          // 显示蓝牙状态
          try {
            auto& btMgr = BluetoothHIDManager::getInstance();
            valueText = btMgr.isEnabled() ? L(Str::kOn) : L(Str::kOff);
          } catch (...) {
            valueText = L(Str::kError);
          }
        } else if (settings[i].type == SettingType::ACTION &&
                   settings[i].key && strcmp(settings[i].key, "switchBootSlot") == 0) {
#ifdef CROSSPOINT_MURPHY_M4
          valueText = runningOtaLabel();
#endif
        }
        return valueText;
      });

  // Draw help text
  const auto labels = mappedInput.mapLabels(L(Str::kBack), L(Str::kToggle), L(Str::kUp), L(Str::kDown));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Always use standard refresh for settings screen
  renderer.displayBuffer();
}
