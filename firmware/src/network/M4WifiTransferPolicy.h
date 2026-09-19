#pragma once

#include <cstdio>
#include <cstring>
#include <cstdint>

#include "activities/settings/M4SettingsAction.h"
#include "activities/settings/M4SettingsConfirm.h"
#include "network/M4NetworkOccupancy.h"

// Call-layer policy for Phase 1 Wi-Fi / 传书. Does not change the frozen
// Foundation occupancy signatures: NeedNetwork is share; radio-off steal is
// m4NetworkTryDeinit.

enum class M4WifiSelectionPurpose : uint8_t { SessionJoin, SystemNetworking };

enum class M4WifiSelectAction : uint8_t { Connect, StayOnListCheckmark };

constexpr int kM4KnownNetworkCap = 8;

inline M4WifiSelectAction m4WifiSelectActionForSsid(
    const char* ssid, const char* connectedSsid,
    M4WifiSelectionPurpose purpose = M4WifiSelectionPurpose::SessionJoin) {
  if (purpose != M4WifiSelectionPurpose::SystemNetworking) return M4WifiSelectAction::Connect;
  if (ssid && connectedSsid && ssid[0] && connectedSsid[0] && std::strcmp(ssid, connectedSsid) == 0) {
    return M4WifiSelectAction::StayOnListCheckmark;
  }
  return M4WifiSelectAction::Connect;
}

inline bool m4WifiScanShouldDisconnectExistingSta(bool staConnected) { return !staConnected; }

inline bool m4WifiSystemEntryStartsWebServer() { return false; }

inline bool m4WifiSelectionCompletesOnCheckmark() { return false; }

inline const char* m4WifiHiddenNetworkLabel() { return "加入隐藏网络"; }

inline const char* m4WifiKnownNetworkFullCopy() { return "已保存网络已满 8/8，本次未保存"; }

inline const char* m4WifiBtWouldDisconnectWifiCopy() { return "打开蓝牙会断开 Wi-Fi"; }

inline const char* m4WifiWouldDisconnectBtCopy() { return "连接 Wi-Fi 会断开蓝牙翻页"; }

inline const char* m4WifiTransferHoldsCopy() { return "传书正在使用网络，无法切换 Wi-Fi"; }

inline int m4WifiListRowCount(int scannedCount) {
  if (scannedCount < 0) scannedCount = 0;
  return scannedCount + 1;  // trailing hidden-network row
}

inline bool m4WifiListRowIsHidden(int index, int scannedCount) {
  if (scannedCount < 0) scannedCount = 0;
  return index == scannedCount;
}

inline void m4WifiFormatKnownCount(char* buf, unsigned n, int saved, int cap) {
  if (!buf || n == 0) return;
  std::snprintf(buf, n, "%d/%d", saved, cap);
}

// Hub/product default for the persisted invert key: 0 → canonical auto-connect ON.
constexpr uint8_t kM4WifiAlwaysReselectProductDefault = 0;

inline uint8_t m4WifiAutoConnectKnownDefaultOn(uint8_t wifiAlwaysReselect) {
  return m4AutoConnectKnownNetworksFromAlwaysReselect(wifiAlwaysReselect);
}

inline bool m4WifiShouldAutoConnectKnown(uint8_t wifiAlwaysReselect) {
  return m4AutoConnectKnownNetworksFromAlwaysReselect(wifiAlwaysReselect) != 0;
}

inline bool m4WifiBtConfirmAccepts(M4ConfirmButton b, bool footerPrimaryOrRow) {
  return m4SettingsDangerAccepts(b, footerPrimaryOrRow);
}

inline M4NetworkAcquireResult m4WifiNeedNetwork(M4NetworkOwner owner) { return m4NeedNetwork(owner); }

inline void m4WifiReleaseNetwork(M4NetworkOwner owner) { m4ReleaseNetwork(owner); }

inline M4NetworkAcquireResult m4WifiTryRadioOff(M4NetworkOwner requester) {
  return m4NetworkTryDeinit(requester);
}

inline M4NetworkAcquireResult m4WifiSettingsExclusiveWhileTransfer() {
  if (m4NetworkHeldBy(M4NetworkOwner::Transfer)) return M4NetworkAcquireResult::DeniedHeldByOther;
  return m4NeedNetwork(M4NetworkOwner::WifiSettings);
}

inline void m4WifiReleaseAllOccupancyForConfirmedBt() {
  m4ReleaseNetwork(M4NetworkOwner::WifiSettings);
  m4ReleaseNetwork(M4NetworkOwner::Transfer);
  m4ReleaseNetwork(M4NetworkOwner::Ntp);
  m4ReleaseNetwork(M4NetworkOwner::Cloud);
  m4ReleaseNetwork(M4NetworkOwner::Ota);
  m4ReleaseNetwork(M4NetworkOwner::Other);
}

inline bool m4WifiHolderOtherThan(M4NetworkOwner self) {
  const M4NetworkOwner kOwners[] = {
      M4NetworkOwner::WifiSettings, M4NetworkOwner::Transfer, M4NetworkOwner::Ntp,
      M4NetworkOwner::Cloud,        M4NetworkOwner::Ota,      M4NetworkOwner::Other};
  for (M4NetworkOwner owner : kOwners) {
    if (owner == self) continue;
    if (m4NetworkHeldBy(owner)) return true;
  }
  return false;
}

inline bool m4WifiMayTeardownLink(M4NetworkOwner requester) {
  if (m4NetworkRadioMayOff()) return true;
  return m4NetworkHeldBy(requester) && !m4WifiHolderOtherThan(requester);
}

inline bool m4TransferStartsWithExistingSta(bool staConnected, bool autoStart, bool qemuPluginDebug) {
  (void)qemuPluginDebug;
  return staConnected && autoStart;
}
