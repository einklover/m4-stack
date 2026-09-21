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
    'm4LogRuntimeMemory("file-transfer-exit-end")',
    'render_mutex_wait_ms=',
    'cleanup_deferred=',
    'deferred_cleanup_ms=',
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

# P1D moved concrete network teardown off the navigation-critical Activity exit
# path. Preserve telemetry around that deferred cleanup while requiring the
# bounded render-mutex wait and task notification introduced by P1D.
behavior_guards = [
    "cleanup->service.stop(cleanupApMode)",
    "xSemaphoreTake(renderingMutex, pdMS_TO_TICKS(25))",
    "xTaskNotifyGive(cleanupTask)",
]
missing_activity_guards = [needle for needle in behavior_guards if needle not in activity]
if missing_activity_guards:
    raise SystemExit(
        "P1D file-transfer lifecycle telemetry contract missing deferred teardown behavior:\n  "
        + "\n  ".join(missing_activity_guards)
    )

if "fileTransferService.stop(isApMode)" in activity:
    raise SystemExit(
        "P1D file-transfer lifecycle telemetry contract: Activity exit must not synchronously stop networking"
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
        "P1D service did not preserve concrete deferred network teardown:\n  "
        + "\n  ".join(missing_service_guards)
    )

print("m4 file-transfer lifecycle telemetry contract: PASS")
