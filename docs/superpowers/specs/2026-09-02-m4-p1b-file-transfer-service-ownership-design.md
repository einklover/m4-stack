# M4 P1B File-Transfer Service Ownership Design

## Context

`CrossPointWebServerActivity` currently combines UI/navigation, AP/STA lifecycle, captive DNS, mDNS, HTTP server ownership, HTTP polling, and teardown. P1A added lifecycle telemetry and host contracts without changing behavior. P1B changes only the ownership boundary so later phases can move networking off the Activity lifecycle safely.

Parent: #54  
Tracking: #57  
Base: `runtime/p1a-file-transfer-service-contracts@1590ec27a96dedae1e3d54d13d3b507b7f850a37`

## Goal

Introduce a dedicated `M4FileTransferService` that owns the file-transfer network resources and their synchronous lifecycle. `CrossPointWebServerActivity` remains responsible for UI state, navigation, child activities, and rendering, but delegates network/server ownership and polling.

## Non-goals

- Do not migrate to ESP-IDF `esp_http_server`.
- Do not make teardown asynchronous.
- Do not introduce a global navigation supervisor.
- Do not change HTTP routes, browser behavior, file-transfer semantics, or QR contents.
- Do not change existing teardown delays in this phase.
- Do not flash real hardware.
- Do not treat NetworkManager regression #53 as caused by P1B unless new evidence demonstrates causality.

## Ownership boundary

Before:

```text
CrossPointWebServerActivity
 ├─ UI / rendering / input / child Activity
 ├─ CrossPointWebServer ownership
 ├─ DNSServer ownership
 ├─ mDNS lifecycle
 ├─ Wi-Fi/AP shutdown
 └─ HTTP/DNS polling
```

After P1B:

```text
CrossPointWebServerActivity
 ├─ UI / rendering / input / child Activity
 ├─ UI-visible network metadata (SSID/IP/mode)
 └─ M4FileTransferService
      ├─ CrossPointWebServer ownership
      ├─ DNSServer ownership
      ├─ mDNS lifecycle
      ├─ AP/STA file-transfer network lifecycle
      └─ bounded HTTP/DNS polling helpers
```

The Activity may still decide *when* an AP or server should start and how failures are displayed. The service owns the concrete resources and performs the corresponding lifecycle operations.

## Service API

The service is a concrete M4 firmware class. It intentionally uses the current Arduino/ESP32 networking stack so P1B remains a mechanical ownership refactor.

```cpp
class M4FileTransferService final {
 public:
  M4FileTransferService() = default;
  ~M4FileTransferService();

  M4FileTransferService(const M4FileTransferService&) = delete;
  M4FileTransferService& operator=(const M4FileTransferService&) = delete;

  bool beginAccessPoint(const char* ssid, const char* password, int channel,
                        int maxConnections, const char* hostname,
                        std::string& connectedIp);
  bool beginStationServer(const char* hostname);
  bool beginWebServer();

  void processDns();
  void handleWebClients(int maxIters, unsigned long budgetMs);

  bool webServerRunning() const;
  bool hasDnsServer() const;

  void stopWebServer();
  void stopNetwork(bool isApMode);
  void stop(bool isApMode);
};
```

Implementation may use a smaller equivalent API if it preserves the ownership invariants below. `stop()` must be idempotent.

## Required invariants

1. `CrossPointWebServerActivity.h` does not own `std::unique_ptr<CrossPointWebServer>`.
2. There is no Activity translation-unit global `DNSServer*`.
3. `M4FileTransferService` owns `CrossPointWebServer` and `DNSServer`.
4. mDNS begin/end used by file transfer live behind the service boundary.
5. File-transfer AP/STA teardown used by this Activity lives behind the service boundary.
6. Repeated `stop()` or destruction after `stop()` is safe.
7. Existing HTTP handling iteration/budget remains bounded exactly as before at the Activity call site or equivalently inside the service.
8. Existing QEMU plugin-debug behavior is preserved.
9. UI state names and visible strings remain unchanged.
10. P1B does not solve asynchronous Home/Back escape; it only creates the ownership boundary required for that later phase.

## Error handling

Service start methods return failure rather than mutating Activity UI state. The Activity converts failures into the existing `showSetupError(...)` paths. Resource cleanup on a failed start is service-owned and leaves the service safe to stop again.

## Telemetry

Keep P1A telemetry. `server_stop_ms`, file-transfer memory snapshots, navigation request logging, and `exit_ms` remain observable. P1B must not remove evidence needed to compare before/after behavior.

## Testing

P1B uses three layers:

1. Host/source ownership contract: verifies Activity no longer contains direct server/DNS ownership/lifecycle tokens and delegates to `M4FileTransferService`.
2. Native service-state/compile contract where feasible without Arduino runtime; ownership API should remain narrow enough to test structure on host.
3. Existing CI: P0/P1A contracts, Murphy M4 production build, flash composition, plugin-debug/production QEMU boot and smoke.

A RED commit must exist before production implementation. GREEN evidence must correspond to the exact final head.

## Rollback

P1B is stacked on P1A and isolated in `runtime/p1b-file-transfer-service-ownership`. If behavior or resource regressions appear, the P1B PR can be dropped without reverting P0/P1A.