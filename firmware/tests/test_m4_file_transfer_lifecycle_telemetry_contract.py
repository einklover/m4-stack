#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ACTIVITY = ROOT / "firmware/src/activities/network/CrossPointWebServerActivity.cpp"

text = ACTIVITY.read_text(encoding="utf-8")

required = [
    '#include "util/M4RuntimeMemory.h"',
    'm4LogRuntimeMemory("file-transfer-enter")',
    'm4LogRuntimeMemory("file-transfer-server-start-before")',
    'm4LogRuntimeMemory("file-transfer-server-start-after")',
    'm4LogRuntimeMemory("file-transfer-nav-request")',
    'm4LogRuntimeMemory("file-transfer-exit-begin")',
    'm4LogRuntimeMemory("file-transfer-after-network-stop")',
    'm4LogRuntimeMemory("file-transfer-exit-end")',
    'server_stop_ms=',
    'render_mutex_wait_ms=',
    'exit_ms=',
]

missing = [needle for needle in required if needle not in text]
if missing:
    raise SystemExit(
        "m4 file-transfer lifecycle telemetry contract missing:\n  "
        + "\n  ".join(missing)
    )

# Characterization only: the existing synchronous teardown behavior must still
# be present in P1A. P1B/P1D will deliberately change these ownership details.
behavior_guards = [
    "stopWebServer();",
    "MDNS.end();",
    "WiFi.mode(WIFI_OFF);",
    "xSemaphoreTake(renderingMutex, portMAX_DELAY)",
]
missing_guards = [needle for needle in behavior_guards if needle not in text]
if missing_guards:
    raise SystemExit(
        "P1A unexpectedly changed the characterized teardown path:\n  "
        + "\n  ".join(missing_guards)
    )

print("m4 file-transfer lifecycle telemetry contract: PASS")
