#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ACTIVITY_H = ROOT / "firmware/src/activities/network/CrossPointWebServerActivity.h"
ACTIVITY_CPP = ROOT / "firmware/src/activities/network/CrossPointWebServerActivity.cpp"
SERVICE_H = ROOT / "firmware/src/network/M4FileTransferService.h"
SERVICE_CPP = ROOT / "firmware/src/network/M4FileTransferService.cpp"
ROUTES_H = ROOT / "firmware/src/network/M4FileTransferHttpRoutes.h"
ROUTES_CPP = ROOT / "firmware/src/network/M4FileTransferHttpRoutes.cpp"
AUX_H = ROOT / "firmware/src/network/M4FileTransferAuxiliaryServer.h"
AUX_CPP = ROOT / "firmware/src/network/M4FileTransferAuxiliaryServer.cpp"

paths = [ACTIVITY_H, ACTIVITY_CPP, SERVICE_H, SERVICE_CPP, ROUTES_H, ROUTES_CPP, AUX_H, AUX_CPP]
missing_files = [str(path.relative_to(ROOT)) for path in paths if not path.exists()]
if missing_files:
    raise SystemExit("P1C transport files missing:\n  " + "\n  ".join(missing_files))

activity = ACTIVITY_H.read_text(encoding="utf-8") + "\n" + ACTIVITY_CPP.read_text(encoding="utf-8")
service_h = SERVICE_H.read_text(encoding="utf-8")
service_cpp = SERVICE_CPP.read_text(encoding="utf-8")
service = service_h + "\n" + service_cpp
routes = ROUTES_H.read_text(encoding="utf-8") + "\n" + ROUTES_CPP.read_text(encoding="utf-8")
aux = AUX_H.read_text(encoding="utf-8") + "\n" + AUX_CPP.read_text(encoding="utf-8")
transport = service + "\n" + routes + "\n" + aux

# Keep the public service header host-compilable. ESP-IDF implementation types
# belong behind the private HttpRuntime boundary in the .cpp.
forbidden_public_esp = [
    "esp_http_server.h",
    "httpd_handle_t",
    "httpd_req_t",
]
public_leaks = [needle for needle in forbidden_public_esp if needle in service_h]
if public_leaks:
    raise SystemExit(
        "P1C leaked ESP-IDF HTTP types into M4FileTransferService public header:\n  "
        + "\n  ".join(public_leaks)
    )

required_server_contract = [
    "esp_http_server.h",
    "httpd_handle_t httpServer",
    "httpd_start(",
    "httpd_register_uri_handler(",
    "httpd_stop(",
    "httpd_req_recv(",
    "descriptor.user_ctx = context",
    "std::unique_ptr<HttpRuntime>",
]
missing_server_contract = [needle for needle in required_server_contract if needle not in transport]
if missing_server_contract:
    raise SystemExit(
        "P1C esp_http_server ownership contract missing:\n  "
        + "\n  ".join(missing_server_contract)
    )

forbidden_legacy_ownership = [
    "std::unique_ptr<CrossPointWebServer>",
    "webServer->handleClient()",
]
legacy = [needle for needle in forbidden_legacy_ownership if needle in service + "\n" + activity]
if legacy:
    raise SystemExit(
        "P1C still owns/services browser HTTP through legacy WebServer polling:\n  "
        + "\n  ".join(legacy)
    )

# P1C may temporarily retain the P1B Activity-facing method signature so the
# transport switch is one reversible commit, but its implementation must be an
# auxiliary-only pump. HTTP itself is serviced by the httpd task.
if "handleWebClients" in service:
    required_compat = [
        "browser HTTP is serviced by esp_http_server",
        "pollAuxiliary();",
    ]
    missing_compat = [needle for needle in required_compat if needle not in service]
    if missing_compat:
        raise SystemExit(
            "P1C transitional Activity pump is not explicitly auxiliary-only:\n  "
            + "\n  ".join(missing_compat)
        )

required_routes = [
    '"/", HTTP_GET',
    '"/files", HTTP_GET',
    '"/api/status", HTTP_GET',
    '"/api/files", HTTP_GET',
    '"/download", HTTP_GET',
    '"/upload", HTTP_POST',
    '"/mkdir", HTTP_POST',
    '"/rename", HTTP_POST',
    '"/move", HTTP_POST',
    '"/delete", HTTP_POST',
    '"/settings", HTTP_GET',
    '"/api/settings", HTTP_GET',
    '"/api/settings", HTTP_POST',
]
missing_routes = [needle for needle in required_routes if needle not in service_cpp]
if missing_routes:
    raise SystemExit(
        "P1C esp_http_server route registration contract missing:\n  "
        + "\n  ".join(missing_routes)
    )

# user_ctx must point to an object owned by the service runtime, not Activity
# state or a stack temporary.
required_context = [
    "runtime.httpRoutes.get()",
    "req->user_ctx",
]
missing_context = [needle for needle in required_context if needle not in service_cpp]
if missing_context:
    raise SystemExit(
        "P1C service-owned handler context contract missing:\n  " + "\n  ".join(missing_context)
    )

required_bounds = [
    "HTTP_BODY_CHUNK_SIZE",
    "HTTP_MAX_BODY_SIZE",
    "HTTP_CONTROL_BODY_SIZE",
]
missing_bounds = [needle for needle in required_bounds if needle not in routes]
if missing_bounds:
    raise SystemExit(
        "P1C bounded request/body contract missing:\n  " + "\n  ".join(missing_bounds)
    )

unbounded_patterns = [
    "malloc(req->content_len)",
    "malloc(request->content_len)",
    "new char[req->content_len]",
    "new uint8_t[req->content_len]",
]
found_unbounded = [needle for needle in unbounded_patterns if needle in routes]
if found_unbounded:
    raise SystemExit(
        "P1C request handling allocates from unbounded content_len:\n  "
        + "\n  ".join(found_unbounded)
    )

serialization_contract = [
    "xSemaphoreCreateMutex()",
    "StorageGuard",
    "storageMutex",
]
missing_serialization = [needle for needle in serialization_contract if needle not in service + routes]
if missing_serialization:
    raise SystemExit(
        "P1C storage serialization contract missing:\n  " + "\n  ".join(missing_serialization)
    )

auxiliary_contract = [
    "WebSocketsServer",
    "WS_PORT = 81",
    "udp_.begin(LOCAL_UDP_PORT)",
    "pollAuxiliary()",
]
missing_aux = [needle for needle in auxiliary_contract if needle not in service + aux]
if missing_aux:
    raise SystemExit(
        "P1C auxiliary WebSocket/UDP compatibility contract missing:\n  " + "\n  ".join(missing_aux)
    )

if "CrossPointWebServerActivity" in service:
    raise SystemExit("P1C HTTP service must not target CrossPointWebServerActivity state directly")

required_memory_breadcrumbs = [
    'm4LogRuntimeMemory("file-transfer-server-start-before")',
    'm4LogRuntimeMemory("file-transfer-server-start-after")',
    'm4LogRuntimeMemory("file-transfer-after-network-stop")',
]
combined = activity + "\n" + service
missing_memory = [needle for needle in required_memory_breadcrumbs if needle not in combined]
if missing_memory:
    raise SystemExit(
        "P1C lost required file-transfer memory breadcrumbs:\n  " + "\n  ".join(missing_memory)
    )

print("m4 esp_http_server contract: PASS")
