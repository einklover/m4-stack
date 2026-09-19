// M4 UI Redesign Phase 1 — Wi-Fi / Transfer contracts
// Build:
//   /opt/homebrew/bin/g++-14 -std=c++17 -I firmware/src \
//     firmware/tests/native_app/test_m4_wifi_transfer_phase1.cpp \
//     -o /tmp/test_m4_wifi_transfer_phase1 && /tmp/test_m4_wifi_transfer_phase1
//
// RED until M4WifiTransferPolicy.h lands and production call sites wire
// occupancy / system-Wi-Fi / checkmark / hidden / cap / BT confirm copy.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#if __has_include("network/M4WifiTransferPolicy.h")
#include "network/M4WifiTransferPolicy.h"
#define HAS_WIFI_POLICY 1
#else
#define HAS_WIFI_POLICY 0
#endif

#if __has_include("network/M4NetworkOccupancy.h")
#include "network/M4NetworkOccupancy.h"
#define HAS_OCC 1
#else
#define HAS_OCC 0
#endif

#if __has_include("activities/settings/M4SettingsAction.h")
#include "activities/settings/M4SettingsAction.h"
#define HAS_ACTION 1
#else
#define HAS_ACTION 0
#endif

#if __has_include("activities/settings/M4SettingsConfirm.h")
#include "activities/settings/M4SettingsConfirm.h"
#define HAS_CONFIRM 1
#else
#define HAS_CONFIRM 0
#endif

namespace {

std::string readFile(const char* path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string loadFirst(const char* const* candidates) {
  for (auto p = candidates; *p; ++p) {
    std::string c = readFile(*p);
    if (!c.empty()) return c;
  }
  return {};
}

std::string loadMainCpp() {
  const char* c[] = {"firmware/src/main.cpp", "./firmware/src/main.cpp",
                     "../firmware/src/main.cpp", "../../firmware/src/main.cpp", nullptr};
  return loadFirst(c);
}

std::string loadWifiCpp() {
  const char* c[] = {"firmware/src/activities/network/WifiSelectionActivity.cpp",
                     "./firmware/src/activities/network/WifiSelectionActivity.cpp",
                     "../firmware/src/activities/network/WifiSelectionActivity.cpp",
                     "../../firmware/src/activities/network/WifiSelectionActivity.cpp",
                     nullptr};
  return loadFirst(c);
}

std::string loadWifiH() {
  const char* c[] = {"firmware/src/activities/network/WifiSelectionActivity.h",
                     "./firmware/src/activities/network/WifiSelectionActivity.h",
                     "../firmware/src/activities/network/WifiSelectionActivity.h",
                     "../../firmware/src/activities/network/WifiSelectionActivity.h",
                     nullptr};
  return loadFirst(c);
}

std::string loadTransferCpp() {
  const char* c[] = {"firmware/src/activities/network/CrossPointWebServerActivity.cpp",
                     "./firmware/src/activities/network/CrossPointWebServerActivity.cpp",
                     "../firmware/src/activities/network/CrossPointWebServerActivity.cpp",
                     "../../firmware/src/activities/network/CrossPointWebServerActivity.cpp",
                     nullptr};
  return loadFirst(c);
}

std::string loadServiceCpp() {
  const char* c[] = {"firmware/src/network/M4FileTransferService.cpp",
                     "./firmware/src/network/M4FileTransferService.cpp",
                     "../firmware/src/network/M4FileTransferService.cpp",
                     "../../firmware/src/network/M4FileTransferService.cpp", nullptr};
  return loadFirst(c);
}

std::string loadBtCpp() {
  const char* c[] = {"firmware/src/activities/settings/SimpleBluetoothActivity.cpp",
                     "./firmware/src/activities/settings/SimpleBluetoothActivity.cpp",
                     "../firmware/src/activities/settings/SimpleBluetoothActivity.cpp",
                     "../../firmware/src/activities/settings/SimpleBluetoothActivity.cpp",
                     nullptr};
  return loadFirst(c);
}

std::string loadPolicyH() {
  const char* c[] = {"firmware/src/network/M4WifiTransferPolicy.h",
                     "./firmware/src/network/M4WifiTransferPolicy.h",
                     "../firmware/src/network/M4WifiTransferPolicy.h",
                     "../../firmware/src/network/M4WifiTransferPolicy.h", nullptr};
  return loadFirst(c);
}

std::string loadJ2() {
  const char* c[] = {"simulator/journeys/j2_wifi_list_no_crash.json",
                     "./simulator/journeys/j2_wifi_list_no_crash.json",
                     "../simulator/journeys/j2_wifi_list_no_crash.json",
                     "../../simulator/journeys/j2_wifi_list_no_crash.json", nullptr};
  return loadFirst(c);
}

std::string loadCpsH() {
  const char* c[] = {"firmware/src/CrossPointSettings.h", "./firmware/src/CrossPointSettings.h",
                     "../firmware/src/CrossPointSettings.h", "../../firmware/src/CrossPointSettings.h",
                     nullptr};
  return loadFirst(c);
}

std::string loadCpsCpp() {
  const char* c[] = {"firmware/src/CrossPointSettings.cpp", "./firmware/src/CrossPointSettings.cpp",
                     "../firmware/src/CrossPointSettings.cpp",
                     "../../firmware/src/CrossPointSettings.cpp", nullptr};
  return loadFirst(c);
}

std::string extractFn(const std::string& src, const char* sig) {
  auto pos = src.rfind(sig);
  if (pos == std::string::npos) return {};
  auto brace = src.find('{', pos);
  if (brace == std::string::npos) return {};
  int depth = 0;
  for (size_t i = brace; i < src.size(); ++i) {
    if (src[i] == '{') ++depth;
    else if (src[i] == '}') {
      --depth;
      if (depth == 0) return src.substr(brace, i - brace + 1);
    }
  }
  return {};
}

bool streq(const char* a, const char* b) {
  if (!a || !b) return a == b;
  return std::strcmp(a, b) == 0;
}

}  // namespace

#if !(HAS_WIFI_POLICY && HAS_OCC && HAS_ACTION && HAS_CONFIRM)

int main() {
  printf("RED: Wi-Fi/Transfer Phase 1 policy missing (policy=%d occ=%d action=%d confirm=%d)\n",
         HAS_WIFI_POLICY, HAS_OCC, HAS_ACTION, HAS_CONFIRM);
  fflush(stdout);
  assert(false && "RED: M4WifiTransferPolicy.h not yet present");
  return 1;
}

#else

namespace {

void testAutoConnectHelperDefaultOn() {
  assert(kM4WifiAlwaysReselectProductDefault == 0);
  assert(m4WifiAutoConnectKnownDefaultOn(1) == 0);
  assert(m4WifiAutoConnectKnownDefaultOn(0) == 1);
  assert(m4AutoConnectKnownNetworksFromAlwaysReselect(0) == 1);
  assert(m4WifiShouldAutoConnectKnown(kM4WifiAlwaysReselectProductDefault));
  assert(!m4WifiShouldAutoConnectKnown(1));
  assert(m4WifiShouldAutoConnectKnown(0));
  const std::string policy = loadPolicyH();
  assert(policy.find("m4WifiShouldAutoConnectKnownMigrated") == std::string::npos &&
         "do not fake missing-key migration in a helper production never calls");
  printf("auto-connect helper PASS\n");
}

void testAutoConnectPersistDefaultOn() {
  const std::string cpsH = loadCpsH();
  const std::string cps = loadCpsCpp();
  assert(!cpsH.empty() && !cps.empty());
  assert(cpsH.find("uint8_t wifiAlwaysReselect = 0;") != std::string::npos &&
         "fresh CrossPointSettings must default auto-connect ON (always-reselect 0)");
  assert(cpsH.find("uint8_t wifiAlwaysReselect = 1;") == std::string::npos &&
         "field initializer must not remain 1 (auto-connect OFF)");

  const std::string reset = extractFn(cps, "void CrossPointSettings::resetToDefaults(");
  assert(!reset.empty());
  const auto resetAssign = reset.find("wifiAlwaysReselect");
  assert(resetAssign != std::string::npos);
  const auto resetSemi = reset.find(';', resetAssign);
  const std::string resetStmt = reset.substr(resetAssign, resetSemi - resetAssign + 1);
  assert(resetStmt.find("= 0") != std::string::npos &&
         "resetToDefaults must restore always-reselect 0 (auto-connect ON)");
  assert(resetStmt.find("= 1") == std::string::npos);

  const std::string load = extractFn(cps, "bool CrossPointSettings::loadFromFile(");
  assert(!load.empty());
  const auto docAt = load.find("doc[\"wifiAlwaysReselect\"]");
  assert(docAt != std::string::npos && "JSON load must read wifiAlwaysReselect");
  const auto stmtStart = load.rfind("wifiAlwaysReselect", docAt);
  const auto stmtEnd = load.find(';', docAt);
  assert(stmtStart != std::string::npos && stmtEnd != std::string::npos);
  const std::string loadStmt = load.substr(stmtStart, stmtEnd - stmtStart + 1);
  assert(loadStmt.find("| (uint8_t)0") != std::string::npos &&
         "JSON missing key must fall back to 0 (auto-connect ON), not 1");
  assert(loadStmt.find("| (uint8_t)1") == std::string::npos &&
         "JSON missing-key fallback 1 would keep auto-connect OFF");
  assert(loadStmt.find("doc[\"wifiAlwaysReselect\"]") != std::string::npos &&
         "present JSON value (including explicit 1) must be kept via ArduinoJson |");

  const std::string save = extractFn(cps, "bool CrossPointSettings::saveToFile(");
  assert(!save.empty());
  assert(save.find("doc[\"wifiAlwaysReselect\"]") != std::string::npos &&
         "save must persist the field so an explicit user 1 stays OFF");

  const std::string binary = extractFn(cps, "bool CrossPointSettings::loadFromBinaryFile(");
  assert(!binary.empty());
  assert(binary.find("wifiAlwaysReselect") == std::string::npos &&
         "binary format has no wifiAlwaysReselect; missing field uses struct default 0");
  printf("auto-connect persist default ON PASS\n");
}

void testKnownNetworkCap() {
  assert(kM4KnownNetworkCap == 8);
  char buf[16];
  m4WifiFormatKnownCount(buf, sizeof(buf), 3, kM4KnownNetworkCap);
  assert(streq(buf, "3/8"));
  m4WifiFormatKnownCount(buf, sizeof(buf), 8, kM4KnownNetworkCap);
  assert(streq(buf, "8/8"));
  assert(std::strstr(m4WifiKnownNetworkFullCopy(), "8/8") != nullptr);
  printf("known-network n/8 PASS\n");
}

void testHiddenNetworkRow() {
  assert(streq(m4WifiHiddenNetworkLabel(), "加入隐藏网络"));
  assert(m4WifiListRowCount(0) == 1);
  assert(m4WifiListRowCount(4) == 5);
  assert(m4WifiListRowIsHidden(4, 4));
  assert(!m4WifiListRowIsHidden(0, 4));
  printf("hidden-network row PASS\n");
}

void testConnectedSsidCheckmarkDoesNotStartWebServer() {
  // StayOnListCheckmark is SystemNetworking-only. SessionJoin (default, 2-arg)
  // tapping the current SSID is Connect so production can onComplete(true).
  assert(m4WifiSelectActionForSsid("Home", "Home") == M4WifiSelectAction::Connect);
  assert(m4WifiSelectActionForSsid("Home", "Home", M4WifiSelectionPurpose::SessionJoin) ==
         M4WifiSelectAction::Connect);
  assert(m4WifiSelectActionForSsid("Home", "Home", M4WifiSelectionPurpose::SystemNetworking) ==
         M4WifiSelectAction::StayOnListCheckmark);
  assert(m4WifiSelectActionForSsid("Office", "Home", M4WifiSelectionPurpose::SystemNetworking) ==
         M4WifiSelectAction::Connect);
  assert(m4WifiSelectActionForSsid("Home", "") == M4WifiSelectAction::Connect);
  assert(!m4WifiSystemEntryStartsWebServer());
  assert(m4WifiSelectionCompletesOnCheckmark() == false);
  printf("connected SSID checkmark PASS\n");
}

void testBtConfirmCopyAndPower() {
  assert(streq(m4WifiBtWouldDisconnectWifiCopy(), "打开蓝牙会断开 Wi-Fi"));
  assert(streq(m4WifiWouldDisconnectBtCopy(), "连接 Wi-Fi 会断开蓝牙翻页"));
  assert(!m4SettingsPowerIsPrimary(M4ConfirmButton::Power));
  assert(!m4WifiBtConfirmAccepts(M4ConfirmButton::Power, true));
  assert(m4WifiBtConfirmAccepts(M4ConfirmButton::Confirm, true));
  assert(!m4WifiBtConfirmAccepts(M4ConfirmButton::Back, true));
  printf("BT confirm copy PASS\n");
}

void testOccupancyCallLayer() {
  m4NetworkOccupancyResetForTest();
  assert(m4WifiNeedNetwork(M4NetworkOwner::Transfer) == M4NetworkAcquireResult::Ok);
  assert(m4NetworkHeldBy(M4NetworkOwner::Transfer));
  // Foundation NeedNetwork is share; exclusive WifiSettings steal is denied.
  assert(m4WifiSettingsExclusiveWhileTransfer() == M4NetworkAcquireResult::DeniedHeldByOther);
  assert(m4WifiTryRadioOff(M4NetworkOwner::WifiSettings) == M4NetworkAcquireResult::DeniedHeldByOther);
  m4WifiReleaseNetwork(M4NetworkOwner::Transfer);
  assert(m4NetworkRadioMayOff());
  assert(m4WifiSettingsExclusiveWhileTransfer() == M4NetworkAcquireResult::Ok);
  assert(m4NetworkHeldBy(M4NetworkOwner::WifiSettings));
  m4WifiReleaseNetwork(M4NetworkOwner::WifiSettings);
  assert(m4NetworkRadioMayOff());
  printf("occupancy call layer PASS\n");
}

void testTransferExistingStaPolicy() {
  assert(!m4TransferStartsWithExistingSta(false, false, false));
  assert(!m4TransferStartsWithExistingSta(false, true, false));
  // Home 传书 (autoStart=false) keeps hotspot/STA/Calibre picker even if STA is up.
  assert(!m4TransferStartsWithExistingSta(true, false, false));
  assert(m4TransferStartsWithExistingSta(true, true, false));    // USB autoStartSavedSta
  assert(!m4TransferStartsWithExistingSta(true, false, true));   // QEMU Home 传书 keeps picker
  assert(m4TransferStartsWithExistingSta(true, true, true));     // QEMU USB autoStart still starts
  printf("transfer existing-STA policy PASS\n");
}

void testScanDisconnectGate() {
  assert(!m4WifiScanShouldDisconnectExistingSta(true));
  assert(m4WifiScanShouldDisconnectExistingSta(false));
  std::string scan = extractFn(loadWifiCpp(), "void WifiSelectionActivity::startWifiScan");
  assert(!scan.empty());
  const auto helperAt = scan.find("m4WifiScanShouldDisconnectExistingSta");
  assert(helperAt != std::string::npos && "startWifiScan must consult scan-disconnect policy");
  const auto discAt = scan.find("WiFi.disconnect()");
  assert(discAt != std::string::npos);
  assert(discAt > helperAt && "must not WiFi.disconnect() before the occupancy/STA gate");
  printf("scan disconnect gate PASS\n");
}

void testOccupancyTeardownMayLink() {
  m4NetworkOccupancyResetForTest();
  assert(m4WifiMayTeardownLink(M4NetworkOwner::Transfer));  // empty occupancy
  assert(!m4WifiHolderOtherThan(M4NetworkOwner::Transfer));
  m4WifiNeedNetwork(M4NetworkOwner::Transfer);
  assert(m4WifiMayTeardownLink(M4NetworkOwner::Transfer));  // sole holder
  m4WifiNeedNetwork(M4NetworkOwner::Ntp);
  assert(m4WifiHolderOtherThan(M4NetworkOwner::Transfer));
  assert(!m4WifiMayTeardownLink(M4NetworkOwner::Transfer));
  m4WifiReleaseNetwork(M4NetworkOwner::Ntp);
  m4WifiReleaseNetwork(M4NetworkOwner::Transfer);
  assert(m4NetworkRadioMayOff());
  printf("occupancy teardown gate PASS\n");
}

void testOnGoToNetworkIsSystemWifi() {
  std::string main = loadMainCpp();
  assert(!main.empty() && "main.cpp must be readable");
  std::string body = extractFn(main, "void onGoToNetwork()");
  assert(!body.empty() && "onGoToNetwork body not found");
  assert(body.find("onGoToFileTransfer();") == std::string::npos &&
         "onGoToNetwork must not alias 传书");
  assert(body.find("WifiSelectionActivity") != std::string::npos &&
         "onGoToNetwork must open the system Wi-Fi list");
  assert(body.find("CrossPointWebServerActivity") == std::string::npos);
  std::string transfer = extractFn(main, "void onGoToFileTransfer()");
  assert(transfer.find("CrossPointWebServerActivity") != std::string::npos);
  printf("onGoToNetwork system Wi-Fi PASS\n");
}

void testWifiSelectionWiring() {
  std::string wifi = loadWifiCpp();
  std::string wifiH = loadWifiH();
  assert(!wifi.empty() && !wifiH.empty());
  assert(wifi.find("M4WifiTransferPolicy.h") != std::string::npos);
  assert(wifi.find("m4NeedNetwork") != std::string::npos ||
         wifi.find("m4WifiNeedNetwork") != std::string::npos);
  assert(wifi.find("configTime") != std::string::npos);
  assert(wifi.find("m4NeedNetwork(M4NetworkOwner::Ntp)") != std::string::npos ||
         wifi.find("m4WifiNeedNetwork(M4NetworkOwner::Ntp)") != std::string::npos);
  assert(wifi.find(m4WifiHiddenNetworkLabel()) != std::string::npos);
  assert(wifi.find("3/8") != std::string::npos || wifi.find("%d/%d") != std::string::npos ||
         wifi.find("m4WifiFormatKnownCount") != std::string::npos);
  assert(wifi.find(m4WifiKnownNetworkFullCopy()) != std::string::npos);
  assert(wifi.find(m4WifiWouldDisconnectBtCopy()) != std::string::npos);
  assert(wifi.find("btMgr.disable()") == std::string::npos ||
         wifi.find(m4WifiWouldDisconnectBtCopy()) != std::string::npos);
  std::string select = extractFn(wifi, "void WifiSelectionActivity::selectNetwork");
  assert(select.find("m4WifiSelectActionForSsid") != std::string::npos);
  assert(select.find("purpose") != std::string::npos);
  assert(select.find("StayOnListCheckmark") != std::string::npos);
  assert(select.find("onComplete(true)") != std::string::npos &&
         "SessionJoin same-SSID must complete rather than StayOnListCheckmark");
  assert(select.find("attemptConnection()") != std::string::npos);
  assert(wifi.find("m4QemuNetWifiCompatConnected") != std::string::npos);
  assert(wifiH.find("SystemNetworking") != std::string::npos);
  assert(wifiH.find("occupancyDenied") != std::string::npos);
  printf("WifiSelection wiring PASS\n");
}

void testOccupancyDeniedDoesNotFallToNetworkList() {
  std::string wifi = loadWifiCpp();
  std::string loop = extractFn(wifi, "void WifiSelectionActivity::loop");
  const auto failedAt = loop.find("CONNECTION_FAILED");
  assert(failedAt != std::string::npos);
  const auto nextAt = loop.find("if (state !=", failedAt);
  std::string failed = loop.substr(failedAt, nextAt == std::string::npos ? std::string::npos : nextAt - failedAt);
  assert(failed.find("occupancyDenied") != std::string::npos);
  assert(failed.find("onComplete(false)") != std::string::npos);
  std::string scan = extractFn(wifi, "void WifiSelectionActivity::startWifiScan");
  assert(scan.find("m4WifiSettingsExclusiveWhileTransfer") != std::string::npos &&
         "startWifiScan must re-check exclusive lease before touching radio");
  printf("occupancy-denied exit PASS\n");
}

void testJ2EntersViaHomeTransfer() {
  std::string j2 = loadJ2();
  assert(!j2.empty() && "j2_wifi_list_no_crash.json must be readable");
  assert(j2.find("AppList") == std::string::npos && "J2 must not wait AppList");
  assert(j2.find("网络管理") == std::string::npos &&
         "J2 must not claim 网络管理 opens CrossPointWebServer");
  assert(j2.find("0.53125") != std::string::npos && j2.find("0.4979") != std::string::npos &&
         "JOIN option-2 tap nx=0.53125 ny=0.4979 must remain");
  assert(j2.find("0.2667") != std::string::npos && j2.find("0.57875") != std::string::npos &&
         "Home 传书 tile center tap missing");
  assert((j2.find("onGoToFileTransfer") != std::string::npos || j2.find("文件传输") != std::string::npos) &&
         "J2 must name Home 传书 / onGoToFileTransfer");
  assert(j2.find("wait_activity") != std::string::npos && j2.find("CrossPointWebServer") != std::string::npos);
  printf("J2 Home 传书 entry PASS\n");
}

void testAutoConnectProductionWiring() {
  std::string wifi = loadWifiCpp();
  assert(wifi.find("CrossPointSettings.h") != std::string::npos);
  assert(wifi.find("SETTINGS.wifiAlwaysReselect") != std::string::npos);
  std::string results = extractFn(wifi, "void WifiSelectionActivity::processWifiScanResults");
  assert(!results.empty());
  assert((results.find("m4WifiShouldAutoConnectKnown") != std::string::npos ||
          wifi.find("maybeAutoConnectKnown") != std::string::npos) &&
         "scan results must invoke auto-connect, not helper-only");
  assert(results.find("selectNetwork") != std::string::npos ||
         wifi.find("maybeAutoConnectKnown") != std::string::npos);
  assert(results.find("currentConnectedSsid") != std::string::npos);
  printf("auto-connect production wiring PASS\n");
}

void testTransferOccupancyWiring() {
  std::string xfer = loadTransferCpp();
  std::string svc = loadServiceCpp();
  assert(!xfer.empty() && !svc.empty());
  assert(xfer.find("m4NeedNetwork(M4NetworkOwner::Transfer)") != std::string::npos ||
         xfer.find("m4WifiNeedNetwork(M4NetworkOwner::Transfer)") != std::string::npos);
  assert(xfer.find("m4ReleaseNetwork(M4NetworkOwner::Transfer)") != std::string::npos ||
         xfer.find("m4WifiReleaseNetwork(M4NetworkOwner::Transfer)") != std::string::npos);
  assert(svc.find("m4NetworkTryDeinit") != std::string::npos ||
         svc.find("m4WifiTryRadioOff") != std::string::npos);
  assert(svc.find("WIFI_OFF") != std::string::npos);
  std::string stop = extractFn(svc, "void M4FileTransferService::stop(");
  std::string stopErr = extractFn(svc, "void M4FileTransferService::stopForSetupError");
  assert(!stop.empty() && !stopErr.empty());
  auto productionTail = [](const std::string& fn) {
    const auto elseAt = fn.find("#else");
    return elseAt == std::string::npos ? fn : fn.substr(elseAt);
  };
  const std::string stopProd = productionTail(stop);
  const std::string errProd = productionTail(stopErr);
  assert(stopProd.find("m4WifiMayTeardownLink") != std::string::npos &&
         "stop() disconnect/softAP must respect occupancy, not only WIFI_OFF");
  assert(errProd.find("m4WifiMayTeardownLink") != std::string::npos &&
         "stopForSetupError AP teardown must respect occupancy");
  const auto gateAt = stopProd.find("m4WifiMayTeardownLink");
  const auto discAt = stopProd.find("WiFi.disconnect");
  const auto apAt = stopProd.find("softAPdisconnect");
  assert(gateAt != std::string::npos);
  if (discAt != std::string::npos) assert(discAt > gateAt);
  if (apAt != std::string::npos) assert(apAt > gateAt);
  printf("Transfer occupancy wiring PASS\n");
}

void testBluetoothConfirmWiring() {
  std::string bt = loadBtCpp();
  assert(!bt.empty());
  assert(bt.find(m4WifiBtWouldDisconnectWifiCopy()) != std::string::npos);
  assert(bt.find("m4WifiBtConfirmAccepts") != std::string::npos ||
         bt.find("m4SettingsDangerAccepts") != std::string::npos);
  printf("Bluetooth confirm wiring PASS\n");
}

}  // namespace

int main() {
  testAutoConnectHelperDefaultOn();
  testAutoConnectPersistDefaultOn();
  testKnownNetworkCap();
  testHiddenNetworkRow();
  testConnectedSsidCheckmarkDoesNotStartWebServer();
  testBtConfirmCopyAndPower();
  testOccupancyCallLayer();
  testTransferExistingStaPolicy();
  testScanDisconnectGate();
  testOccupancyTeardownMayLink();
  testOnGoToNetworkIsSystemWifi();
  testWifiSelectionWiring();
  testOccupancyDeniedDoesNotFallToNetworkList();
  testJ2EntersViaHomeTransfer();
  testAutoConnectProductionWiring();
  testTransferOccupancyWiring();
  testBluetoothConfirmWiring();
  printf("ALL Wi-Fi/Transfer Phase 1 contracts PASS\n");
  return 0;
}

#endif
