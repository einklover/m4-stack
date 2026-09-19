#pragma once

#include <cstring>

#include "activities/settings/M4SettingsAction.h"
#include "activities/settings/SettingsHubPolicy.h"

// Device Settings root catalog. Single source for Phase 1 root keys (spec §4.1).
// Hub-era bridge helpers resolve policy/flatten keys so activate is never a sort index.

struct M4SettingsRow {
  const char* key;
  const char* titleZh;
  M4SettingsControl control;
  bool actionable;
};

constexpr int kM4SettingsRootCount = 8;

inline const M4SettingsRow* m4SettingsRootCatalog() {
  static const M4SettingsRow kRows[kM4SettingsRootCount] = {
      {"wifi", "Wi-Fi", M4SettingsControl::Navigate, true},
      {"frontlight", "前光", M4SettingsControl::Navigate, true},
      {"readerLayout", "阅读", M4SettingsControl::Navigate, true},
      {"sleepTimeout", "休眠", M4SettingsControl::Choice, true},
      {"sleepScreen", "锁屏", M4SettingsControl::Choice, true},
      {"keys", "按键", M4SettingsControl::Navigate, true},
      {"maintenance", "维护", M4SettingsControl::Navigate, true},
      {"advanced", "更多/高级", M4SettingsControl::Navigate, true},
  };
  return kRows;
}

// Root visual groups. Presentation-only metadata: flat catalog order, selection
// identity, window math, and touch bands never see groups. Mapping is by key
// so a ledger-approved reorder never misfiles a row.
constexpr int kM4SettingsRootGroupCount = 4;
constexpr int kM4SettingsRootGroupCapacity = 3;

inline const char* m4SettingsRootGroupTitle(int group) {
  switch (group) {
    case 0: return "连接与设备";
    case 1: return "阅读与显示";
    case 2: return "设备与操作";
    case 3: return "系统与其他";
    default: return "";
  }
}

inline int m4SettingsRootGroupOfKey(const char* key) {
  if (!key) return -1;
  if (std::strcmp(key, "wifi") == 0) return 0;
  if (std::strcmp(key, "frontlight") == 0 || std::strcmp(key, "readerLayout") == 0) return 1;
  if (std::strcmp(key, "sleepTimeout") == 0 || std::strcmp(key, "sleepScreen") == 0 ||
      std::strcmp(key, "keys") == 0)
    return 2;
  if (std::strcmp(key, "maintenance") == 0 || std::strcmp(key, "advanced") == 0) return 3;
  return -1;
}

inline int m4SettingsIndexOfKey(const M4SettingsRow* rows, int count, const char* key) {
  if (!rows || !key || count <= 0) return -1;
  for (int i = 0; i < count; ++i) {
    if (rows[i].key && std::strcmp(rows[i].key, key) == 0) return i;
  }
  return -1;
}

inline const M4SettingsRow* m4SettingsRowByKey(const M4SettingsRow* rows, int count, const char* key) {
  const int i = m4SettingsIndexOfKey(rows, count, key);
  return i < 0 ? nullptr : &rows[i];
}

inline int m4SettingsStoreIndexForKey(const char* const* storeKeys, int storeCount, const char* key) {
  if (!storeKeys || !key || storeCount <= 0) return -1;
  for (int i = 0; i < storeCount; ++i) {
    if (storeKeys[i] && std::strcmp(storeKeys[i], key) == 0) return i;
  }
  return -1;
}

inline const char* m4SettingsPolicyKeyAt(SettingsHubCard card, int policyIndex, bool m4Build) {
  return settingsHubRowAt(card, policyIndex, m4Build, false).key;
}

inline int m4SettingsPolicyIndexOfKey(SettingsHubCard card, const char* key, bool m4Build) {
  if (!key) return -1;
  const int n = settingsHubRowCount(card, m4Build, false);
  for (int i = 0; i < n; ++i) {
    const auto row = settingsHubRowAt(card, i, m4Build, false);
    if (row.key && std::strcmp(row.key, key) == 0) return i;
  }
  return -1;
}

inline int m4SettingsFlattenSettingIndexOfKey(SettingsHubCard card, const char* key, bool m4Build) {
  if (!key) return -1;
  int seen = 0;
  const int flatCount = settingsFlatCount(card, m4Build);
  for (int f = 0; f < flatCount; ++f) {
    const auto row = settingsFlatAt(card, f, m4Build);
    if (row.kind != SettingsFlatKind::Setting) continue;
    if (row.key && std::strcmp(row.key, key) == 0) return seen;
    ++seen;
  }
  return -1;
}

inline const char* m4SettingsFlattenKeyAtSettingIndex(SettingsHubCard card, int settingIndex, bool m4Build) {
  const int flat = settingsFlatIndexOfSetting(card, settingIndex, m4Build);
  if (flat < 0) return "";
  const auto row = settingsFlatAt(card, flat, m4Build);
  return row.key ? row.key : "";
}

inline void m4SettingsCopyKey(char* dst, int dstLen, const char* key) {
  if (!dst || dstLen <= 0) return;
  dst[0] = 0;
  if (!key) return;
  int i = 0;
  for (; key[i] && i < dstLen - 1; ++i) dst[i] = key[i];
  dst[i] = 0;
}

inline void m4SettingsSyncSelectedKey(SettingsNavState& state, bool m4Build) {
  const char* key = m4SettingsFlattenKeyAtSettingIndex(state.hub, state.selectedRow, m4Build);
  m4SettingsCopyKey(state.selectedKey, (int)sizeof(state.selectedKey), key);
}
