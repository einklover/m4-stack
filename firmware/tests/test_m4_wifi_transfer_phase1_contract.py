#!/usr/bin/env python3
"""Phase 1 Wi-Fi / Transfer: system networking ≠ 传书.

RED on the Foundation-only tree: onGoToNetwork still aliases
onGoToFileTransfer, WifiSelection silently disables Bluetooth, there is
no hidden-network row / n/8 copy / occupancy call-layer, and a connected
SSID still completes (Transfer then starts the Web server).

GREEN: Settings/system Wi-Fi always opens the network list; connected
SSID is checkmark-only; JOIN_NETWORK contract is unchanged; occupancy
calls wrap Transfer / WifiSelection / NTP-on-connect; BT exclusivity has
explicit confirm copy; known-network cap 8 is user-visible; QEMU compat
radio skip is preserved.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "firmware" / "src"
POLICY = SRC / "network" / "M4WifiTransferPolicy.h"
MAIN = SRC / "main.cpp"
WIFI_CPP = SRC / "activities" / "network" / "WifiSelectionActivity.cpp"
WIFI_H = SRC / "activities" / "network" / "WifiSelectionActivity.h"
XFER_CPP = SRC / "activities" / "network" / "CrossPointWebServerActivity.cpp"
XFER_H = SRC / "activities" / "network" / "CrossPointWebServerActivity.h"
SVC_CPP = SRC / "network" / "M4FileTransferService.cpp"
BT_CPP = SRC / "activities" / "settings" / "SimpleBluetoothActivity.cpp"
OCC = SRC / "network" / "M4NetworkOccupancy.h"
CATALOG = SRC / "activities" / "settings" / "M4SettingsCatalog.h"

failures = []


def fail(msg):
    failures.append(msg)


def _function(source: str, signature: str) -> str:
    start = source.rindex(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


def main():
    if not POLICY.exists():
        fail("network/M4WifiTransferPolicy.h missing (call-layer policy not present)")
        raise SystemExit(
            "m4 wifi-transfer phase1 contract FAILED:\n  - " + "\n  - ".join(failures)
        )

    policy = POLICY.read_text(encoding="utf-8")
    main_cpp = MAIN.read_text(encoding="utf-8")
    wifi_cpp = WIFI_CPP.read_text(encoding="utf-8")
    wifi_h = WIFI_H.read_text(encoding="utf-8")
    xfer_cpp = XFER_CPP.read_text(encoding="utf-8")
    xfer_h = XFER_H.read_text(encoding="utf-8")
    svc_cpp = SVC_CPP.read_text(encoding="utf-8")
    bt_cpp = BT_CPP.read_text(encoding="utf-8")
    occ = OCC.read_text(encoding="utf-8")
    catalog = CATALOG.read_text(encoding="utf-8")

    # Frozen Foundation occupancy API must not be rewritten by this module.
    for token in (
        "enum class M4NetworkOwner",
        "m4NeedNetwork",
        "m4ReleaseNetwork",
        "m4NetworkTryDeinit",
        "m4NetworkRadioMayOff",
    ):
        if token not in occ:
            fail(f"Foundation occupancy API drifted ({token})")

    if "wifi" not in catalog or "m4SettingsRootCatalog" not in catalog:
        fail("must not replace Settings root catalog; wifi key missing")

    net = _function(main_cpp, "void onGoToNetwork()")
    xfer = _function(main_cpp, "void onGoToFileTransfer()")
    if "onGoToFileTransfer();" in net:
        fail("onGoToNetwork still aliases onGoToFileTransfer (Settings Wi-Fi hijack)")
    if "WifiSelectionActivity" not in net:
        fail("onGoToNetwork does not open WifiSelectionActivity")
    if "CrossPointWebServerActivity" in net:
        fail("onGoToNetwork still enters the Web server activity")
    if "new CrossPointWebServerActivity" not in xfer:
        fail("LOCK: onGoToFileTransfer no longer enters CrossPointWebServerActivity")

    if "m4WifiSelectActionForSsid" not in wifi_cpp:
        fail("WifiSelection does not consult connected-SSID checkmark policy")
    select = _function(wifi_cpp, "void WifiSelectionActivity::selectNetwork")
    if "purpose" not in select:
        fail("selectNetwork checkmark policy is purpose-blind")
    if "StayOnListCheckmark" not in select:
        fail("SystemNetworking same-SSID StayOnListCheckmark missing")
    if "onComplete(true)" not in select:
        fail("SessionJoin same-SSID must onComplete(true) rather than stay on the list")
    if "attemptConnection()" not in select:
        fail("selectNetwork saved/open path must keep attemptConnection()")
    if "occupancyDenied" not in wifi_h:
        fail("WifiSelection has no occupancyDenied member for deny-exit")
    loop = _function(wifi_cpp, "void WifiSelectionActivity::loop")
    failed_at = loop.find("CONNECTION_FAILED")
    if failed_at < 0:
        fail("loop() lost CONNECTION_FAILED handling")
    else:
        next_at = loop.find("if (state !=", failed_at)
        failed = loop[failed_at: next_at if next_at >= 0 else None]
        if "occupancyDenied" not in failed or "onComplete(false)" not in failed:
            fail("occupancy deny still falls through to NETWORK_LIST without onComplete(false)")
    scan = _function(wifi_cpp, "void WifiSelectionActivity::startWifiScan")
    if "m4WifiScanShouldDisconnectExistingSta" not in scan:
        fail("startWifiScan does not consult scan-disconnect policy")
    if "m4WifiSettingsExclusiveWhileTransfer" not in scan:
        fail("startWifiScan does not re-check exclusive occupancy before radio")
    if "CrossPointSettings.h" not in wifi_cpp or "SETTINGS.wifiAlwaysReselect" not in wifi_cpp:
        fail("auto-connect known is helper-only; production does not read SETTINGS")
    results = _function(wifi_cpp, "void WifiSelectionActivity::processWifiScanResults")
    if "m4WifiShouldAutoConnectKnown" not in results and "maybeAutoConnectKnown" not in wifi_cpp:
        fail("processWifiScanResults never invokes auto-connect")
    if "加入隐藏网络" not in wifi_cpp:
        fail("hidden-network row copy missing")
    if "已保存网络已满 8/8" not in wifi_cpp and "m4WifiKnownNetworkFullCopy" not in wifi_cpp:
        fail("known-network cap 8 has no user-visible failure copy")
    if "连接 Wi-Fi 会断开蓝牙翻页" not in wifi_cpp:
        fail("Wi-Fi→BT exclusivity confirm copy missing")
    if "m4QemuNetWifiCompatConnected" not in wifi_cpp:
        fail("QEMU compat radio skip missing from WifiSelection")
    if "SystemNetworking" not in wifi_h:
        fail("WifiSelection has no system-networking purpose distinct from session join")

    ntp_fn = _function(wifi_cpp, "void WifiSelectionActivity::checkConnectionStatus")
    if "configTime" in ntp_fn and "Ntp" not in ntp_fn:
        fail("NTP-on-connect is not occupancy-wired")

    if "M4NetworkOwner::Transfer" not in xfer_cpp:
        fail("Transfer activity does not acquire occupancy")
    if "m4ReleaseNetwork" not in xfer_cpp and "m4WifiReleaseNetwork" not in xfer_cpp:
        fail("Transfer activity does not release occupancy")
    if "m4NetworkTryDeinit" not in svc_cpp and "m4WifiTryRadioOff" not in svc_cpp:
        fail("file-transfer stop still force-deinits without occupancy TryDeinit")
    stop = _function(svc_cpp, "void M4FileTransferService::stop(")
    stop_err = _function(svc_cpp, "void M4FileTransferService::stopForSetupError")
    stop_prod = stop[stop.find("#else") :] if "#else" in stop else stop
    err_prod = stop_err[stop_err.find("#else") :] if "#else" in stop_err else stop_err
    if "m4WifiMayTeardownLink" not in stop_prod:
        fail("stop() disconnect/softAP is not occupancy-gated (WIFI_OFF-only gate)")
    if "m4WifiMayTeardownLink" not in err_prod:
        fail("stopForSetupError AP teardown is not occupancy-gated")

    j2_path = ROOT / "simulator" / "journeys" / "j2_wifi_list_no_crash.json"
    j2 = j2_path.read_text(encoding="utf-8")
    if "AppList" in j2:
        fail("J2 still enters via AppList; Home 传书 is the transfer doorway")
    if "网络管理" in j2:
        fail("J2 still claims 网络管理 opens CrossPointWebServer")
    if "0.53125" not in j2 or "0.4979" not in j2:
        fail("JOIN option-2 tap nx=0.53125 ny=0.4979 drifted")
    if "0.2667" not in j2 or "0.57875" not in j2:
        fail("J2 Home 传书 tap (nx=0.2667, ny=0.57875) missing")
    if "onGoToFileTransfer" not in j2 and "文件传输" not in j2:
        fail("J2 must name Home 传书 / onGoToFileTransfer")

    if "m4WifiScanShouldDisconnectExistingSta" not in policy:
        fail("scan-disconnect policy helper missing")
    if "m4WifiMayTeardownLink" not in policy:
        fail("link-teardown occupancy helper missing")
    if "m4WifiShouldAutoConnectKnownMigrated" in policy:
        fail("fake missing-key helper m4WifiShouldAutoConnectKnownMigrated must not exist")
    if "kM4WifiAlwaysReselectProductDefault" not in policy:
        fail("product default for wifiAlwaysReselect (auto-connect ON) missing")

    cps_h = (SRC / "CrossPointSettings.h").read_text(encoding="utf-8")
    cps_cpp = (SRC / "CrossPointSettings.cpp").read_text(encoding="utf-8")
    if "uint8_t wifiAlwaysReselect = 0;" not in cps_h:
        fail("CrossPointSettings field default is not 0 (auto-connect ON)")
    if "uint8_t wifiAlwaysReselect = 1;" in cps_h:
        fail("CrossPointSettings field default still 1 (auto-connect OFF)")
    reset = _function(cps_cpp, "void CrossPointSettings::resetToDefaults(")
    reset_at = reset.find("wifiAlwaysReselect")
    if reset_at < 0:
        fail("resetToDefaults does not assign wifiAlwaysReselect")
    else:
        reset_stmt = reset[reset_at : reset.find(";", reset_at) + 1]
        if "= 0" not in reset_stmt:
            fail("resetToDefaults does not restore wifiAlwaysReselect = 0")
        if "= 1" in reset_stmt:
            fail("resetToDefaults still restores wifiAlwaysReselect = 1")
    load = _function(cps_cpp, "bool CrossPointSettings::loadFromFile(")
    doc_at = load.find('doc["wifiAlwaysReselect"]')
    if doc_at < 0:
        fail("JSON load does not read wifiAlwaysReselect")
    else:
        stmt_start = load.rfind("wifiAlwaysReselect", 0, doc_at)
        load_stmt = load[stmt_start : load.find(";", doc_at) + 1]
        if "| (uint8_t)0" not in load_stmt:
            fail("JSON missing-key fallback is not 0 (auto-connect ON)")
        if "| (uint8_t)1" in load_stmt:
            fail("JSON missing-key fallback still 1 (auto-connect OFF)")
        if 'doc["wifiAlwaysReselect"]' not in load_stmt:
            fail("JSON load must keep a present explicit 1 via ArduinoJson |")
    save = _function(cps_cpp, "bool CrossPointSettings::saveToFile(")
    if 'doc["wifiAlwaysReselect"]' not in save:
        fail("saveToFile must persist wifiAlwaysReselect so explicit 1 stays OFF")
    binary = _function(cps_cpp, "bool CrossPointSettings::loadFromBinaryFile(")
    if "wifiAlwaysReselect" in binary:
        fail("binary load must not invent wifiAlwaysReselect; missing uses struct default 0")

    join = _function(
        xfer_cpp,
        "void CrossPointWebServerActivity::onNetworkModeSelected(const NetworkMode mode)",
    )
    join_at = join.index("if (mode == NetworkMode::JOIN_NETWORK)")
    join_branch = join[join_at : join.index("return;", join.index("EnterWifiSelection", join_at)) + 7]
    if "StartWebServer" in join_branch:
        fail("JOIN_NETWORK contract broken: connected Join hijacks to Web server")
    if "EnterWifiSelection" not in join_branch:
        fail("JOIN_NETWORK contract broken: does not enter WifiSelection")

    if "打开蓝牙会断开 Wi-Fi" not in bt_cpp:
        fail("BT→Wi-Fi exclusivity confirm copy missing")

    # Frozen files this module must not own.
    if "kMaxRepeatItems" in policy:
        fail("policy must not touch kMaxRepeatItems")

    if failures:
        raise SystemExit(
            "m4 wifi-transfer phase1 contract FAILED:\n  - " + "\n  - ".join(failures)
        )
    print("m4 wifi-transfer phase1 contract: PASS")


if __name__ == "__main__":
    main()
