#pragma once

#include <cstdint>

// Host-testable Settings control grammar. Arduino-free.
// settingType ordinals match SettingsActivity::SettingType:
// 0 TOGGLE, 1 ENUM, 2 ACTION, 3 VALUE, 4 STRING.

enum class M4SettingsControl : uint8_t {
  Navigate = 0,
  Toggle = 1,
  Choice = 2,
  Number = 3,
  Confirm = 4,
};

inline M4SettingsControl m4SettingsControlForKind(uint8_t settingType, int optionCount, bool danger,
                                                  bool isDoor) {
  if (danger) return M4SettingsControl::Confirm;
  if (isDoor) return M4SettingsControl::Navigate;
  if (settingType == 0) return M4SettingsControl::Toggle;          // TOGGLE
  if (settingType == 3) return M4SettingsControl::Number;          // VALUE
  if (settingType == 1) {                                          // ENUM
    if (optionCount >= 3) return M4SettingsControl::Choice;
    return M4SettingsControl::Toggle;
  }
  return M4SettingsControl::Navigate;  // ACTION / STRING
}

inline const char* m4SettingsFooterConfirmLabel(M4SettingsControl c) {
  switch (c) {
    case M4SettingsControl::Toggle:
      return "切换";
    case M4SettingsControl::Choice:
    case M4SettingsControl::Number:
      return "选择";
    case M4SettingsControl::Navigate:
    case M4SettingsControl::Confirm:
    default:
      return "打开";
  }
}

inline uint8_t m4AutoConnectKnownNetworksFromAlwaysReselect(uint8_t wifiAlwaysReselect) {
  return wifiAlwaysReselect ? 0 : 1;
}
