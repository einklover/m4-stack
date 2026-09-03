#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile

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
service = service_h + "\n" + service_cpp

# P1B's durable invariant is the ownership boundary, not the legacy transport
# or the pre-P1D placement of the service object. P1D moved the service into a
# deferred cleanup context so navigation can detach before concrete teardown.
required_activity = [
    '#include "network/M4FileTransferService.h"',
    "M4FileTransferService service;",
    "std::unique_ptr<DeferredCleanupContext> deferredCleanupContext;",
    "M4FileTransferService& fileTransferService()",
    "cleanup->service.stop(",
]
missing_activity = [needle for needle in required_activity if needle not in activity_h + activity_cpp]
if missing_activity:
    raise SystemExit(
        "P1B Activity delegation contract missing:\n  " + "\n  ".join(missing_activity)
    )

forbidden_activity = [
    "std::unique_ptr<CrossPointWebServer>",
    "httpd_handle_t",
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
    "std::unique_ptr<DNSServer>",
    "MDNS.begin(",
    "MDNS.end()",
    "WiFi.softAPdisconnect(true)",
    "WiFi.mode(WIFI_OFF)",
    "void stop(bool isApMode)",
]
missing_service = [needle for needle in required_service if needle not in service]
if missing_service:
    raise SystemExit(
        "P1B service ownership contract missing:\n  " + "\n  ".join(missing_service)
    )

owned_transport_markers = [
    "std::unique_ptr<CrossPointWebServer>",
    "httpd_handle_t",
]
if not any(marker in service for marker in owned_transport_markers):
    raise SystemExit(
        "P1B service no longer owns a concrete HTTP transport:\n  expected one of:\n  "
        + "\n  ".join(owned_transport_markers)
    )

# Regression: the public service header deliberately forward-declares its
# Arduino-owned resource types. A consumer must still be able to construct an
# owner from that header alone. Keeping the default constructor inline causes
# libstdc++ to instantiate unique_ptr<DNSServer>'s deleter while DNSServer is
# incomplete, which production GCC 14 rejects.
with tempfile.TemporaryDirectory(prefix="m4-file-transfer-header-") as temp_dir:
    temp = Path(temp_dir)
    probe = temp / "service_owner_compile.cpp"
    probe.write_text(
        '#include "network/M4FileTransferService.h"\n'
        'struct HeaderOnlyOwner { M4FileTransferService service; };\n'
        'void construct_header_only_owner() { HeaderOnlyOwner owner; (void)owner; }\n',
        encoding="utf-8",
    )
    compile_result = subprocess.run(
        [
            "g++",
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{ROOT / 'firmware/src'}",
            "-c",
            str(probe),
            "-o",
            str(temp / "service_owner_compile.o"),
        ],
        text=True,
        capture_output=True,
        check=False,
    )
    if compile_result.returncode != 0:
        raise SystemExit(
            "P1B public-header construction compile contract failed:\n"
            + compile_result.stdout
            + compile_result.stderr
        )

print("m4 file-transfer service ownership contract: PASS")
