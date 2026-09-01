#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ACTIVITY_H = ROOT / "firmware/src/activities/network/CrossPointWebServerActivity.h"
ACTIVITY_CPP = ROOT / "firmware/src/activities/network/CrossPointWebServerActivity.cpp"
SERVICE_H = ROOT / "firmware/src/network/M4FileTransferService.h"
SERVICE_CPP = ROOT / "firmware/src/network/M4FileTransferService.cpp"

missing_files = [str(path.relative_to(ROOT)) for path in (SERVICE_H, SERVICE_CPP) if not path.exists()]
if missing_files:
    raise SystemExit("P1B service ownership files missing:\n  " + "\n  ".join(missing_files))

activity_h = ACTIVITY_H.read_text(encoding="utf-8")
activity_cpp = ACTIVITY_CPP.read_text(encoding="utf-8")
service_h = SERVICE_H.read_text(encoding="utf-8")
service_cpp = SERVICE_CPP.read_text(encoding="utf-8")

required_activity = [
    '#include "network/M4FileTransferService.h"',
    "M4FileTransferService fileTransferService;",
    "fileTransferService.stop(",
    "fileTransferService.handleWebClients(",
]
missing_activity = [needle for needle in required_activity if needle not in activity_h + activity_cpp]
if missing_activity:
    raise SystemExit(
        "P1B Activity delegation contract missing:\n  " + "\n  ".join(missing_activity)
    )

forbidden_activity = [
    "std::unique_ptr<CrossPointWebServer>",
    "DNSServer* dnsServer",
    "MDNS.end()",
]
leaked = [needle for needle in forbidden_activity if needle in activity_h + activity_cpp]
if leaked:
    raise SystemExit(
        "P1B Activity still owns file-transfer network resources:\n  " + "\n  ".join(leaked)
    )

required_service = [
    "class M4FileTransferService final",
    "std::unique_ptr<CrossPointWebServer>",
    "std::unique_ptr<DNSServer>",
    "MDNS.begin(",
    "MDNS.end()",
    "WiFi.softAPdisconnect(true)",
    "WiFi.mode(WIFI_OFF)",
    "void stop(bool isApMode)",
]
missing_service = [needle for needle in required_service if needle not in service_h + service_cpp]
if missing_service:
    raise SystemExit(
        "P1B service ownership contract missing:\n  " + "\n  ".join(missing_service)
    )

print("m4 file-transfer service ownership contract: PASS")
