#!/usr/bin/env python3
"""S1 J2 route contract: Join Network must always enter WifiSelection.

RED basis (baseline with the connected shortcut): connected + JOIN_NETWORK
routes to PendingParentAction::StartWebServer, so WifiSelection is unreachable.
GREEN: the JOIN_NETWORK branch routes unconditionally to EnterWifiSelection;
the current link may only surface as status/default highlight inside
WifiSelection, never as an entry hijack.

Independent Web Server capability is locked separately and must stay GREEN on
both baseline and fix: distinct StartWebServer pending action, beginWebServer
transport, post-selection server start, USB autoStart, Hotspot/Calibre routes.

Fix round 1 (runtime): with the QEMU compat link up, entering WifiSelection
must skip every radio touch (STA mode / disconnect / scan) and land in the
existing empty NETWORK_LIST state; otherwise the guest starves (frozen picker,
bridge dead). The normal path keeps the full radio sequence, production builds
are unchanged, and the empty-list pixels are pinned (J2 screenshot contract).
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ACTIVITY_H = ROOT / "firmware/src/activities/network/CrossPointWebServerActivity.h"
ACTIVITY_CPP = ROOT / "firmware/src/activities/network/CrossPointWebServerActivity.cpp"
SERVICE_H = ROOT / "firmware/src/network/M4FileTransferService.h"

activity_h = ACTIVITY_H.read_text(encoding="utf-8")
activity_cpp = ACTIVITY_CPP.read_text(encoding="utf-8")
service_h = SERVICE_H.read_text(encoding="utf-8")


def _function(source: str, signature: str) -> str:
    start = source.index(signature)
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


def _join_branch(route_fn: str) -> str:
    join_at = route_fn.index("if (mode == NetworkMode::JOIN_NETWORK)")
    wifi_at = route_fn.index("PendingParentAction::EnterWifiSelection", join_at)
    end_at = route_fn.index("return;", wifi_at) + len("return;")
    return route_fn[join_at:end_at]


failures = []

# --- Route behavior: model (JOIN_NETWORK, connected) -> pending action ------
route_fn = _function(
    activity_cpp,
    "void CrossPointWebServerActivity::onNetworkModeSelected(const NetworkMode mode)",
)
join_branch = _join_branch(route_fn)

# Behavioral model of the JOIN branch: a connected-conditional that selects
# StartWebServer inside the JOIN branch is the hijack; otherwise the branch
# falls through to EnterWifiSelection for every link state.
if "PendingParentAction::StartWebServer" in join_branch:
    routed = "StartWebServer"
elif "PendingParentAction::EnterWifiSelection" in join_branch:
    routed = "EnterWifiSelection"
else:
    routed = "unknown"
if routed != "EnterWifiSelection":
    failures.append(
        f"JOIN_NETWORK+connected routes to {routed} (WifiSelection unreachable); "
        "expected EnterWifiSelection for every link state"
    )
if "m4QemuNetWifiCompatConnected" in join_branch:
    failures.append(
        "JOIN_NETWORK branch still keys on m4QemuNetWifiCompatConnected(); "
        "the current link must not hijack the Join entry"
    )

# --- Locks: independent Web Server + sibling routes must survive -----------
locks = [
    ("StartWebServer" in activity_h and "EnterWifiSelection" in activity_h
     and "StartAccessPoint" in activity_h,
     "distinct pending actions (StartAccessPoint/EnterWifiSelection/StartWebServer) missing from header"),
    ("case PendingParentAction::StartWebServer:" in activity_cpp
     and "beginWebServer()" in activity_cpp,
     "runPendingParentAction no longer serves StartWebServer via beginWebServer()"),
    ("case PendingParentAction::EnterWifiSelection:" in activity_cpp
     and "new WifiSelectionActivity" in activity_cpp,
     "runPendingParentAction no longer enters WifiSelectionActivity"),
    ("beginWebServer" in service_h,
     "M4FileTransferService::beginWebServer capability missing"),
    ("new CalibreConnectActivity" in route_fn,
     "CONNECT_CALIBRE route missing"),
    ("PendingParentAction::StartAccessPoint" in route_fn,
     "CREATE_HOTSPOT StartAccessPoint route missing"),
    ("autoStartSavedSta" in activity_cpp and "startWebServer()" in activity_cpp,
     "USB file-transfer autoStart (autoStartSavedSta -> startWebServer) missing"),
]
for ok, message in locks:
    if not ok:
        failures.append("LOCK: " + message)

# onWifiSelectionComplete must keep the post-selection server start.
wifi_done = _function(
    activity_cpp,
    "void CrossPointWebServerActivity::onWifiSelectionComplete(const bool connected)",
)
if "PendingParentAction::StartWebServer" not in wifi_done:
    failures.append("LOCK: onWifiSelectionComplete no longer starts the Web Server")

# --- Runtime behavior: connected Join must not wedge the QEMU guest ---------
# RED basis (1b20313): entering WifiSelection with the compat link up forces
# the radio STA mode + disconnect + scan on unmodelled hardware; the guest
# starves (frozen picker, bridge dead, J2 step 9 activity=None). GREEN: the
# compat-up path skips every radio touch and lands in the existing empty
# NETWORK_LIST state (pixel-identical to a failed scan); the normal path keeps
# the full radio sequence. Production (non-QEMU) behavior is unchanged.
WIFI_CPP = ROOT / "firmware/src/activities/network/WifiSelectionActivity.cpp"
wifi_cpp = WIFI_CPP.read_text(encoding="utf-8")

pump_fn = _function(activity_cpp, "void CrossPointWebServerActivity::runPendingParentAction()")
enter_case = pump_fn.split("case PendingParentAction::EnterWifiSelection:")[1].split(
    "case PendingParentAction::StartWebServer:")[0]
if "WiFi.mode(WIFI_STA)" not in enter_case:
    failures.append(
        "RUNTIME: EnterWifiSelection no longer inits radio STA mode on the normal path"
    )
if "!m4QemuNetWifiCompatConnected" not in enter_case:
    failures.append(
        "RUNTIME: EnterWifiSelection forces radio STA mode even with the compat link up "
        "(guest wedge: frozen picker, bridge dead)"
    )

scan_fn = _function(wifi_cpp, "void WifiSelectionActivity::startWifiScan()")
for needle in ("WiFi.mode(WIFI_STA)", "WiFi.scanNetworks(true)"):
    if needle not in scan_fn:
        failures.append(f"RUNTIME: startWifiScan lost its normal-path radio step {needle}")
if "m4WifiScanShouldDisconnectExistingSta" not in scan_fn:
    failures.append(
        "RUNTIME: startWifiScan must consult m4WifiScanShouldDisconnectExistingSta "
        "(unconditional WiFi.disconnect() drops the current STA/SSID checkmark)"
    )
elif "WiFi.disconnect()" in scan_fn and scan_fn.index("WiFi.disconnect()") < scan_fn.index(
        "m4WifiScanShouldDisconnectExistingSta"):
    failures.append("RUNTIME: startWifiScan disconnects before consulting scan-disconnect policy")
if "m4QemuNetWifiCompatConnected" not in scan_fn or scan_fn.index(
        "m4QemuNetWifiCompatConnected") > scan_fn.index("WiFi.scanNetworks(true)"):
    failures.append(
        "RUNTIME: startWifiScan touches the radio with the compat link up "
        "(guest wedge: frozen picker, bridge dead)"
    )
else:
    compat_head = scan_fn[: scan_fn.index("WiFi.scanNetworks(true)")]
    if "NETWORK_LIST" not in compat_head or "return;" not in compat_head:
        failures.append(
            "RUNTIME: compat-up scan bypass does not land in NETWORK_LIST "
            "(the list loop the journey asserts on)"
        )

# Empty-list pixels are the J2 screenshot contract: pin them against drift.
for needle in ('"\\u672a\\u627e\\u5230 Wi-Fi"', '"%d \\u4e2a\\u7f51\\u7edc"',
               '"\\u70b9\\u6309\\u53f3\\u4e0a\\u89d2\\u91cd\\u65b0\\u626b\\u63cf"',
               '"\\u5de6\\u7f18\\u6ed1\\u52a8\\u8fd4\\u56de"'):
    if needle.encode().decode("unicode_escape") not in wifi_cpp:
        failures.append(f"LOCK: WifiSelection empty-list render drifted ({needle})")

if failures:
    raise SystemExit(
        "m4 join-network route contract FAILED:\n  - " + "\n  - ".join(failures)
    )

print("m4 join-network route contract: PASS")
