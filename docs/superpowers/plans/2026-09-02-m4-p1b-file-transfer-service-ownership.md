# M4 P1B File-Transfer Service Ownership Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move file-transfer network/server resource ownership out of `CrossPointWebServerActivity` into `M4FileTransferService` without changing user-visible behavior or asynchronous navigation semantics.

**Architecture:** Keep the existing Arduino/ESP32 networking implementation, HTTP routes, synchronous teardown delays, QEMU compatibility, and Activity UI state. Introduce a concrete service that owns `CrossPointWebServer`, captive DNS, mDNS lifecycle, and file-transfer AP/STA teardown; the Activity delegates lifecycle/polling and retains presentation/navigation decisions.

**Tech Stack:** C++17, Arduino-ESP32/PioArduino, FreeRTOS, `WiFi`, `DNSServer`, `ESPmDNS`, existing `CrossPointWebServer`, Python source-contract tests, GitHub Actions/QEMU.

**Spec:** `docs/superpowers/specs/2026-09-02-m4-p1b-file-transfer-service-ownership-design.md`

## Global Constraints

- Base exactly on P1A head `1590ec27a96dedae1e3d54d13d3b507b7f850a37`.
- Do not migrate to `esp_http_server`.
- Do not make teardown asynchronous.
- Do not change HTTP routes, QR payloads, UI strings, or file-transfer protocol behavior.
- Preserve current teardown delays/order as closely as ownership allows.
- Preserve QEMU plugin-debug branches.
- Keep P1A telemetry.
- Do not flash real hardware.
- Track unrelated NetworkManager failure under #53.

## Closeout evidence snapshot

Exact head evaluated before this documentation update: `652851ee32e6f9145d75007627d7e8f2e837c599`.

- P0, P1A, and P1B host contracts: **PASS** in `m4 fast firmware loop` run `33585631954`.
- Plugin-debug build and fast boot/protocol smoke: **PASS** in the same run.
- Network Manager regression: **FAIL**, matching the separately tracked #53 gate; Reader touch UI is skipped after that fail-fast point.
- Production boot: **PASS** in run `33585631916` at the same exact head.
- P1A → P1B comparison is 11 commits ahead with the ownership extraction plus focused plan/test/workflow/diagnostic changes; no P1C `esp_http_server`, async teardown, route/UI/protocol migration, or #53 fix is included.
- PR #58 still requires GitHub-side stacked-base/body/issue evidence cleanup before it is ready to leave draft status.

---

### Task 1: Add RED ownership contract

**Files:**
- Create: `firmware/tests/test_m4_file_transfer_service_ownership_contract.py`
- Modify: `.github/workflows/m4-fast.yml`

**Interfaces:**
- Consumes: existing P1A branch source layout.
- Produces: static contract requiring `M4FileTransferService` and forbidding direct Activity ownership of `CrossPointWebServer`, global `DNSServer*`, and direct mDNS teardown.

- [x] **Step 1: Write the failing test**

The Python test reads `firmware/src/activities/network/CrossPointWebServerActivity.h/.cpp` and the expected service header/source. Assert:

```python
assert service_h.exists()
assert service_cpp.exists()
assert '#include "network/M4FileTransferService.h"' in activity_h
assert 'M4FileTransferService fileTransferService;' in activity_h
assert 'std::unique_ptr<CrossPointWebServer>' not in activity_h
assert 'DNSServer* dnsServer' not in activity_cpp
assert 'MDNS.end()' not in activity_cpp
assert 'fileTransferService.stop(' in activity_cpp
assert 'fileTransferService.handleWebClients(' in activity_cpp
```

Also assert the service owns the concrete server/DNS types.

- [x] **Step 2: Wire contract into `m4-fast.yml`**

Add the test beside P0/P1A host contracts so PR CI evaluates it before expensive QEMU stages.

- [x] **Step 3: Push RED and verify it fails**

Expected failure: missing `firmware/src/network/M4FileTransferService.h/.cpp` and/or Activity still owns the current resources. The RED contract commit is `31fcb3e56d425ec4602e7254589863cdc9736280`; implementation followed in `14755da5f620191a818596ed5fcf65d984c1670d`.

### Task 2: Implement service ownership boundary

**Files:**
- Create: `firmware/src/network/M4FileTransferService.h`
- Create: `firmware/src/network/M4FileTransferService.cpp`
- Modify: `firmware/src/activities/network/CrossPointWebServerActivity.h`
- Modify: `firmware/src/activities/network/CrossPointWebServerActivity.cpp`

**Interfaces:**
- Produces:

```cpp
class M4FileTransferService final {
 public:
  ~M4FileTransferService();
  bool beginAccessPoint(const char* ssid, const char* password, int channel,
                        int maxConnections, const char* hostname,
                        std::string& connectedIp);
  bool beginStationMdns(const char* hostname);
  bool beginWebServer();
  void processDns();
  void handleWebClients(int maxIters, unsigned long budgetMs);
  bool webServerRunning() const;
  void stopWebServer();
  void stopNetwork(bool isApMode);
  void stop(bool isApMode);
};
```

- [x] **Step 1: Add the service implementation**

Move concrete ownership of `CrossPointWebServer` and `DNSServer` into the service. Put mDNS begin/end and AP/STA shutdown behind service methods. Make `stop()` idempotent and destructor-safe.

- [x] **Step 2: Convert Activity to delegation**

Remove direct server/DNS ownership and lifecycle calls. Keep UI state, SSID/IP strings, child-activity flow, error messages, telemetry, HTTP iteration limits, and navigation decisions in the Activity. Delegate DNS processing, web-client polling, server state, start and stop operations.

- [x] **Step 3: Preserve failure behavior**

If AP/server start fails, return `false` to Activity and reuse the existing `showSetupError(...)` strings. Cleanup partial service resources before returning failure.

- [x] **Step 4: Run host contracts**

P0, P1A, and P1B ownership contracts PASS at `652851ee32e6f9145d75007627d7e8f2e837c599` in run `33585631954`.

### Task 3: Verify exact head and document evidence

**Files:**
- No production source changes unless verification exposes a P1B-caused defect.
- Update GitHub Issue #57 and stacked PR body/comments with exact evidence.

**Interfaces:**
- Consumes: final P1B commit SHA.
- Produces: reviewable GitHub evidence and explicit known-failure classification.

- [x] **Step 1: Run/inspect GitHub Actions**

Exact-head evaluation at `652851ee32e6f9145d75007627d7e8f2e837c599`: host contracts, plugin-debug build, fast boot/protocol smoke, and production boot pass. Network Manager fails at the existing #53 gate and is classified as known/separate rather than evidence of a P1B ownership regression.

- [x] **Step 2: Compare P1A → P1B diff**

Compared `1590ec27a96dedae1e3d54d13d3b507b7f850a37...652851ee32e6f9145d75007627d7e8f2e837c599`: 11 commits ahead. The delta contains P1B design/plan/test/workflow, service ownership and Activity delegation, plus production-diagnostic fixes; it does not introduce P1C/P1D behavior changes.

- [ ] **Step 3: Create/update stacked PR**

Head: `runtime/p1b-file-transfer-service-ownership`  
Base: `runtime/p1a-file-transfer-service-contracts`  
Draft until exact-head verification is evaluated and GitHub-side evidence is synchronized.

- [ ] **Step 4: Update #57**

Record RED run, GREEN run(s), exact commit SHA, build/QEMU results, NetworkManager #53 status, and explicit non-goals.
