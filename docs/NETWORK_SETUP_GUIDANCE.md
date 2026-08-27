# Murphy M4 Network Setup — Operational Guidance (P3)

This note addresses the Xiaohongshu user reports around “网络管理 1/3 跳回” and
“开源阅读怎么配网 / wifi 怎么连” without changing the already-validated
mode-selection flow.

## Current state (main@b5e2b4c + repair font/refresh on `agent/m4-xhs-repair-font`)

- `NetworkModeSelectionActivity` (3 items: 1 手机→设备传书 / 2 设备→WiFi传书 / 3 Calibre无线) uses a deferred `pendingParentAction` pattern in `CrossPointWebServerActivity::onNetworkModeSelected`. The historical “mode 1/3 bounce” was a child-callback race where the mode picker’s 4 KB display task was still alive while the parent allocated WiFi/LWIP/HTTP. It was fixed by deferring `enterNewActivity`/`startAccessPoint`/`startWebServer` until `pumpSubActivityFrame` has destroyed the child. The existing `simulator/tests/network_manager_e2e.py` proves stability: after `Confirm` on each mode, the parent stays `CrossPointWebServer` and the expected child (`""` for 1/2, `CalibreConnect` for 3) remains stable for 3 s (`_assert_mode_stable`). That journey is the correct regression gate (`m4sim test network-manager --skip-build`).

- Evidence on this branch: `firmware/src/activities/network/CrossPointWebServerActivity.cpp:135-222` (reopenModeSelection → pendingParentAction → runPendingParentAction), `NetworkModeSelectionActivity.cpp:82-101` (Select vs Activate, two-tap list pattern), and the same E2E file above. No reproducible navigation bug remains on current `main`; the remaining reports are UX ambiguity.

- Explicit loading/failure feedback already exists and is preserved:
  - Mode 1 (Create Hotspot) → `AP_STARTING` → render “Starting Hotspot…” then `renderServerRunning` shows SSID (`MERCURY_C165_5G` example), QR `WIFI:S:…;;`, `http://192.168.4.1/` + `http://crosspoint.local/`.
  - Mode 2 (Join Network) → `WifiSelectionActivity` → `SCANNING` “Scanning…”, `NETWORK_LIST` list with RSSI / `*` encrypted / `+` saved, `CONNECTING` “Connecting… to <SSID>”. A successful user-entered password is persisted automatically; only an explicit authentication failure clears the saved credential. Other failures preserve it and show `CONNECTION_FAILED` with `connectionError`.
  - Mode 3 (Calibre) → `CalibreConnectActivity` → `WIFI_SELECTION` child or `SERVER_STARTING` → `SERVER_RUNNING` / `ERROR` with heap logs.
  - Failures call `showSetupError` → state `ERROR` → “Network setup failed” + `setupError` + “Back: choose another mode”.

## What users actually saw

- Group-chat “传书页面记IP” is the existing `renderServerRunning` IP/QR — not a hidden debug path. Users who expected a separate “Wi-Fi settings” entry missed that File Transfer *is* the network settings entry point on Murphy M4 (same as Via/Calibre flow).
- “1/3 反跳” on old `diy/crosspoint` firmware was the pre-fix child-race; on `main` it does not reproduce under `m4sim` and has not been reproduced on a real device with `main@b5e2b4c` plus the font/refresh repair.

## No firmware state-machine change in this phase

Because the state bug is already fixed and the E2E is the correct oracle, this phase makes **no** navigation change. The only change is documentation and a small diagnostic aid: a `Serial` line when a mode is selected (already present via `pendingParentAction` logs; no new log needed) and this guidance file so the next agent and QA can point users to the right screen.

## How to verify

```bash
# From a clean main-based checkout with this branch’s two font/refresh commits:
export PATH="$HOME/.local/bin:$HOME/.platformio/penv/bin:$PATH"
export PLATFORMIO_BUILD_CACHE_DIR="$HOME/.cache/murphy-m4/platformio-build-cache"
cd firmware && pio run -e murphy_m4_qemu_plugin   # once
cd .. && ./m4sim test network-manager --plugin-debug --skip-build --ready-seconds 90
# Expected: NETWORK MODE PASS: 1-phone-to-device / 2-device-to-phone / 3-calibre-wifi
#           each with stability_samples ~17 (3 s @ 0.18 s)
```

If that journey is GREEN, the bounce is not a product bug. If a future real-device trace shows a different bounce (e.g. `showSetupError` immediately after mode 2 on a specific router), add that exact `setupError` string and `wifi_connected`/`wifi_ip` to the issue and make a targeted fix — do not rework the whole mode picker.

## Operational tip for users (to be surfaced in the next docs/UI pass)

- File Transfer (Home → File transfer) **is** Wi-Fi setup. Pick mode 2 to join a router, or mode 1 to create a `CROSSPOINT` hotspot when no router is available. After “Connected!” the same screen shows the `http://<IP>/` or `http://crosspoint.local/` URL and QR — that is the address to open in the phone’s browser or in Legado/阅读’s “远程导入”.

No Jinjiang (P2) or Browser Bridge/M5/M6 code was touched in this phase.
