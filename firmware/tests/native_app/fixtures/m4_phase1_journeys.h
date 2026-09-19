#pragma once

#include "activities/settings/M4SettingsPaint.h"
#include "activities/settings/M4SettingsRootUi.h"

// Host-only J0–J3 walk oracles. Production already implements these walks;
// this header names OldRed vs NewGreen so tests cannot silently skip Hub-era
// failure modes. Not compiled into firmware.

constexpr const char* kM4JourneyJ0OldRed =
    "Hub-era n1 required settingsNavMoveHub / settingsNavMoveRow+SyncWindow";
constexpr const char* kM4JourneyJ0NewGreen = "8-row clamp selectedKey whole-row catalog";

constexpr const char* kM4JourneyJ1OldRed = "Hub cards + wrap + Power-as-Confirm";
constexpr const char* kM4JourneyJ1NewGreen =
    "root doors restore selectedKey; danger confirm; Advanced window";

constexpr const char* kM4JourneyJ2OldRed =
    "onGoToNetwork aliased 传书; STA skipped picker; auto-connect OFF";
constexpr const char* kM4JourneyJ2NewGreen =
    "WLAN list vs Transfer picker; hidden/cap8/auto-on; BT exclusive confirm";

constexpr const char* kM4JourneyJ3OldRed =
    "STYLE/MORE unique catalog + missing page-turn user keys";
constexpr const char* kM4JourneyJ3NewGreen =
    "Quick 排版 all-books; font+Reading Settings unique; Enabled/Dir reachable";

inline void m4JourneyEnterRoot(M4SettingsUiState& st) { m4SettingsUiEnterRoot(st); }

// ChildList Back: copy parentKey, enter root, reselect parent (SettingsActivity::handleBack).
inline void m4JourneySettingsBackFromChild(M4SettingsUiState& st) {
  char parent[32]{};
  m4SettingsCopyKey(parent, (int)sizeof(parent), st.parentKey);
  m4SettingsUiEnterRoot(st);
  m4SettingsUiSelectKey(st, parent);
}

// Wi-Fi pop restore: 2-arg ResetFull zeros burstCount (1-arg does not).
inline void m4JourneyWifiPopReset(M4SettingsPaintSession& paint, M4SettingsUiState& st) {
  m4SettingsPaintResetFull(paint, st);
}

// External door pop (阅读 / WLAN): re-enter root and restore savedKey.
inline void m4JourneySimulateExternalPop(M4SettingsUiState& st, const char* savedKey) {
  m4SettingsUiEnterRoot(st);
  if (savedKey && savedKey[0]) m4SettingsUiSelectKey(st, savedKey);
}
