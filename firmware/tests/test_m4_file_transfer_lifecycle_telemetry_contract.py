#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ACTIVITY = ROOT / "firmware/src/activities/network/CrossPointWebServerActivity.cpp"
SERVICE = ROOT / "firmware/src/network/M4FileTransferService.cpp"

activity = ACTIVITY.read_text(encoding="utf-8")
service = SERVICE.read_text(encoding="utf-8") if SERVICE.exists() else ""
combined = activity + "\n" + service

required_activity = [
    '#include "util/M4RuntimeMemory.h"',
    'm4LogRuntimeMemory("file-transfer-enter")',
    'm4LogRuntimeMemory("file-transfer-server-start-before")',
    'm4LogRuntimeMemory("file-transfer-server-start-after")',
    'm4LogRuntimeMemory("file-transfer-nav-request")',
    'm4LogRuntimeMemory("file-transfer-exit-begin")',
    'm4LogRuntimeMemory("file-transfer-after-network-stop")',
    'm4LogRuntimeMemory("file-transfer-exit-end")',
    'render_mutex_wait_ms=',
    'exit_ms=',
]

missing = [needle for needle in required_activity if needle not in activity]
if missing:
    raise SystemExit(
        "m4 file-transfer lifecycle telemetry contract missing:\n  "
        + "\n  ".join(missing)
    )

if "server_stop_ms=" not in combined:
    raise SystemExit("m4 file-transfer lifecycle telemetry contract missing: server_stop_ms=")

# P1B deliberately moves concrete network teardown into M4FileTransferService,
# while preserving the characterized synchronous teardown and render wait.
behavior_guards = [
    "fileTransferService.stop(isApMode)",
    "xSemaphoreTake(renderingMutex, portMAX_DELAY)",
]
missing_activity_guards = [needle for needle in behavior_guards if needle not in activity]
if missing_activity_guards:
    raise SystemExit(
        "P1B unexpectedly changed characterized Activity teardown behavior:\n  "
        + "\n  ".join(missing_activity_guards)
    )

service_guards = [
    "MDNS.end();",
    "WiFi.mode(WIFI_OFF);",
    "delay(200);",
    "delay(300);",
]
missing_service_guards = [needle for needle in service_guards if needle not in service]
if missing_service_guards:
    raise SystemExit(
        "P1B service did not preserve characterized synchronous network teardown:\n  "
        + "\n  ".join(missing_service_guards)
    )

print("m4 file-transfer lifecycle telemetry contract: PASS")
