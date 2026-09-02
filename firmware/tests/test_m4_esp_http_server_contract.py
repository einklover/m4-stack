#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ACTIVITY_H = ROOT / "firmware/src/activities/network/CrossPointWebServerActivity.h"
ACTIVITY_CPP = ROOT / "firmware/src/activities/network/CrossPointWebServerActivity.cpp"
SERVICE_H = ROOT / "firmware/src/network/M4FileTransferService.h"
SERVICE_CPP = ROOT / "firmware/src/network/M4FileTransferService.cpp"

activity_h = ACTIVITY_H.read_text(encoding="utf-8")
activity_cpp = ACTIVITY_CPP.read_text(encoding="utf-8")
service_h = SERVICE_H.read_text(encoding="utf-8")
service_cpp = SERVICE_CPP.read_text(encoding="utf-8")
service = service_h + "\n" + service_cpp
activity = activity_h + "\n" + activity_cpp

# P1C ownership: M4FileTransferService owns the ESP-IDF server handle and
# registration lifecycle. The Activity must no longer drive synchronous HTTP
# servicing through CrossPointWebServer::handleClient().
required_server_contract = [
    "esp_http_server.h",
    "httpd_handle_t",
    "httpd_start(",
    "httpd_register_uri_handler(",
    "httpd_stop(",
    "httpd_req_recv(",
    "user_ctx",
]
missing_server_contract = [needle for needle in required_server_contract if needle not in service]
if missing_server_contract:
    raise SystemExit(
        "P1C esp_http_server ownership contract missing:\n  "
        + "\n  ".join(missing_server_contract)
    )

forbidden_polling = [
    "webServer->handleClient()",
    "fileTransferService.handleWebClients(",
]
leaked_polling = [needle for needle in forbidden_polling if needle in service + "\n" + activity]
if leaked_polling:
    raise SystemExit(
        "P1C still depends on synchronous Activity/service HTTP polling:\n  "
        + "\n  ".join(leaked_polling)
    )

# Existing browser/file-manager routes are compatibility surface. P1C changes
# transport ownership, not route vocabulary.
required_routes = [
    '"/"',
    '"/files"',
    '"/api/status"',
    '"/api/files"',
    '"/download"',
    '"/upload"',
    '"/mkdir"',
    '"/rename"',
    '"/move"',
    '"/delete"',
    '"/settings"',
    '"/api/settings"',
]
missing_routes = [route for route in required_routes if route not in service_cpp]
if missing_routes:
    raise SystemExit(
        "P1C esp_http_server route registration contract missing:\n  "
        + "\n  ".join(missing_routes)
    )

# Handler callbacks must resolve through service-owned context, never Activity
# state. Keep the direct ownership boundary source-visible.
if "CrossPointWebServerActivity" in service:
    raise SystemExit("P1C HTTP service must not target CrossPointWebServerActivity state directly")

# Request/body processing must remain explicitly bounded. The migration uses a
# fixed chunk size rather than allocating a request-sized body buffer.
required_bounds = [
    "HTTP_BODY_CHUNK_SIZE",
    "HTTP_MAX_BODY_SIZE",
]
missing_bounds = [needle for needle in required_bounds if needle not in service]
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
found_unbounded = [needle for needle in unbounded_patterns if needle in service_cpp]
if found_unbounded:
    raise SystemExit(
        "P1C request handling allocates from unbounded content_len:\n  "
        + "\n  ".join(found_unbounded)
    )

# P0/P1A observability must surround the new server lifecycle. Existing labels
# are intentionally retained so RED/GREEN evidence remains comparable.
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
