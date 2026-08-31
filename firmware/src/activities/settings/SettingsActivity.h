#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "activities/ActivityWithSubactivity.h"
#include "activities/settings/SettingsHubPolicy.h"
#include "activities/settings/SettingsSceneModel.h"

class CrossPointSettings;

enum class SettingType { TOGGLE, ENUM, ACTION, VALUE,STRING };

struct SettingInfo {
  const char* name;
  SettingType type;
  uint8_t CrossPointSettings::* valuePtr=nullptr;
  int8_t CrossPointSettings::* signedValuePtr=nullptr;
  std::vector<std::string> enumValues;

  struct ValueRange {
    int8_t min;
    int8_t max;
    int8_t step;
  };
  ValueRange valueRange ={};


  const char* key = nullptr;
  const char* category = nullptr;

  char* stringPtr = nullptr;
  size_t stringMaxLen = 0;

  std::function<uint8_t()> valueGetter;
  std::function<void(uint8_t)> valueSetter;
  std::function<std::string()> stringGetter;
  std::function<void(const std::string&)> stringSetter;

  static SettingInfo Toggle(const char* name, uint8_t CrossPointSettings::* ptr, const char* key = nullptr,
                            const char* category = nullptr) {
    SettingInfo s;
    s.name = name;
    s.type = SettingType::TOGGLE;
    s.valuePtr = ptr;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo Enum(const char* name, uint8_t CrossPointSettings::* ptr, std::vector<std::string> values,
                          const char* key = nullptr, const char* category = nullptr) {
    SettingInfo s;
    s.name = name;
    s.type = SettingType::ENUM;
    s.valuePtr = ptr;
    s.enumValues = std::move(values);
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo Action(const char* name) {
    SettingInfo s;
    s.name = name;
    s.type = SettingType::ACTION;
    return s;
  }

  static SettingInfo String(const char* name, char* ptr, size_t maxLen, const char* key = nullptr,
                            const char* category = nullptr) {
    SettingInfo s;
    s.name = name;
    s.type = SettingType::STRING;
    s.stringPtr = ptr;
    s.stringMaxLen = maxLen;
    s.key = key;
    s.category = category;
    return s;
  }


static SettingInfo Value(const char* name, uint8_t CrossPointSettings::* ptr,
                         int8_t minVal, int8_t maxVal, int8_t stepVal,
                         const char* key = nullptr,
                         const char* category = nullptr) {
    SettingInfo s;
    s.name = name;
    s.type = SettingType::VALUE;
    s.valuePtr = ptr;
    s.valueRange.min = minVal;
    s.valueRange.max = maxVal;
    s.valueRange.step = stepVal;
    s.key = key;
    s.category = category;
    return s;
}

static SettingInfo SignedValue(const char* name, int8_t CrossPointSettings::* ptr,
                               int8_t minVal, int8_t maxVal, int8_t stepVal,
                               const char* key = nullptr,
                               const char* category = nullptr) {
    SettingInfo s;
    s.name = name;
    s.type = SettingType::VALUE;
    s.signedValuePtr = ptr;
    s.valueRange.min = minVal;
    s.valueRange.max = maxVal;
    s.valueRange.step = stepVal;
    s.key = key;
    s.category = category;
    return s;
}


  static SettingInfo DynamicEnum(const char* name, std::vector<std::string> values, std::function<uint8_t()> getter,
                                 std::function<void(uint8_t)> setter, const char* key = nullptr,
                                 const char* category = nullptr) {
    SettingInfo s;
    s.name = name;
    s.type = SettingType::ENUM;
    s.enumValues = std::move(values);
    s.valueGetter = std::move(getter);
    s.valueSetter = std::move(setter);
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo DynamicString(const char* name, std::function<std::string()> getter,
                                   std::function<void(const std::string&)> setter, const char* key = nullptr,
                                   const char* category = nullptr) {
    SettingInfo s;
    s.name = name;
    s.type = SettingType::STRING;
    s.stringGetter = std::move(getter);
    s.stringSetter = std::move(setter);
    s.key = key;
    s.category = category;
    return s;
  }
};

class SettingsActivity final : public ActivityWithSubactivity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;
  SettingsNavState navState_;
  SettingsScene::SettingsSceneModel sceneModel_;
  std::vector<SettingInfo> displayReadingSettings_;
  std::vector<SettingInfo> keysSettings_;
  std::vector<SettingInfo> networkSettings_;
  std::vector<SettingInfo> systemSettings_;

  const std::function<void()> onGoHome;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;
  void rebuildModel();
  const std::vector<SettingInfo>& currentHubSettings() const;
  std::vector<SettingInfo>& currentHubSettings();
  int currentHubSettingCount() const;
  const SettingInfo* findSettingByKey(const char* key) const;
  std::string valueTextForSetting(const SettingInfo& info) const;
  void toggleCurrentSetting();
  void openHubCard(SettingsHubCard card);
  void handleHubConfirm();
  void handleL2Confirm();
  void handleL2TapIndex(int windowIndex);

 public:
  explicit SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                            const std::function<void()>& onGoHome)
      : ActivityWithSubactivity("Settings", renderer, mappedInput), onGoHome(onGoHome) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
