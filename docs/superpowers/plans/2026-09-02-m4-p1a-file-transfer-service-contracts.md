# Murphy M4 P1A — file-transfer lifecycle characterization and service contracts

Issue: #55  
Parent: #54  
Stacked base: PR #52 head `b63d97863ac28908c1d05d4f66c2c10b1d29571e`

## Goal

Create the smallest host-testable service-state contract needed to decouple file-transfer service lifetime from UI Activity lifetime later, while adding measurement-only lifecycle timing to the current implementation. P1A does not move HTTP, Wi-Fi, DNS, mDNS, WebSocket, UDP, or navigation behavior.

## Current critical path to preserve/measure

Current Home path is synchronous at Activity exit:

`onGoHome()` -> `onGoHomeAnimated(false, 0)` -> global `exitActivity()` -> `CrossPointWebServerActivity::onExit()` -> new `HomeActivity`.

Current transfer Activity exit includes web/server stop, mDNS/DNS cleanup, fixed delays, Wi-Fi shutdown, and an unbounded render-mutex take. The current implementation therefore has a structural blocking risk. P1A measures it; P1B/P1D will change ownership/teardown.

## Contract design

Add a freestanding C++ header under `firmware/src/network/` with no Arduino, FreeRTOS, ESP-IDF, STL heap containers, or Activity dependencies.

### Types

- `M4FileTransferServiceState`: `Stopped`, `StartingNetwork`, `StartingServer`, `Ready`, `Stopping`, `Failed`.
- `M4FileTransferMode`: `None`, `JoinNetwork`, `AccessPoint`.
- `M4FileTransferError`: bounded enum for contract-level failure classification.
- `M4FileTransferServiceSnapshot`: state, mode, generation, error, `uiMayDetach`, `cleanupActive`.
- `M4FileTransferServiceContract`: tiny state machine.

### Semantics

- Starting from `Stopped` begins a new non-zero generation and enters `StartingNetwork`.
- Duplicate start during an active generation is idempotent: no new generation/resource ownership.
- Network-ready advances only the current generation to `StartingServer`.
- Server-ready advances only the current generation to `Ready`.
- Failure events mutate only the current generation.
- Stop from any active/failed state enters `Stopping`; repeated Stop is idempotent.
- A snapshot in `Stopping` reports `uiMayDetach=true` and `cleanupActive=true`.
- Stop completion for the current generation moves to `Stopped`, clears mode/error, and reports no cleanup active.
- Events from stale generations are ignored.
- Reset from `Failed` is explicit/deterministic; no implicit restart.

## TDD execution

### Task 1 — RED: host contract

Create `firmware/tests/native_app/test_m4_file_transfer_service_contract.cpp` first. Tests must require the new production header and cover:

1. initial stopped snapshot;
2. start -> `StartingNetwork` with generation > 0;
3. duplicate start does not increment generation;
4. current generation network-ready -> `StartingServer`;
5. current generation server-ready -> `Ready`;
6. stale generation events do nothing;
7. Stop -> `Stopping`, immediate `uiMayDetach`, cleanup active;
8. repeated Stop stays same generation/state;
9. stop-complete -> `Stopped`;
10. failure -> `Failed`, explicit reset/stop behavior;
11. generation rollover skips zero.

Wire this test into a new `P1A host contracts` step in `.github/workflows/m4-fast.yml`. Commit the test/workflow before production implementation so CI records a genuine missing-header RED.

### Task 2 — GREEN: minimal pure state model

Add only the production header/model required by Task 1. No transport ownership, queues, tasks, sockets, or Activity pointers.

Expected result: P1A host contract GREEN while existing P0 contracts remain GREEN.

### Task 3 — characterization telemetry

Instrument current `CrossPointWebServerActivity` and `CrossPointWebServer::stop()` without changing order or behavior.

Capture/log:
- Activity enter;
- before/after server start;
- navigation request;
- exit begin/end elapsed;
- server stop elapsed;
- render-mutex wait elapsed;
- before/after network teardown;
- P0 memory snapshots at stable boundaries.

Use compact logs suitable for serial/QEMU evidence. Do not add dynamic log buffers.

### Task 4 — source/static guard

Add a lightweight source contract if useful to ensure P1A does not accidentally remove the existing synchronous behavior before measurement is captured. Do not make brittle line-number tests.

### Task 5 — verification

Required before claiming P1A implementation complete:
- P0 host contracts PASS;
- P1A host contracts PASS;
- Murphy `murphy_m4` production build PASS;
- plugin-debug build PASS;
- minimal QEMU boot/smoke PASS;
- report #53 NetworkManager result independently if that workflow reaches it;
- no real-device flashing;
- PR diff review confirms no behavioral migration.

## Explicit non-goals

- no `esp_http_server` migration;
- no service worker/task ownership yet;
- no Wi-Fi event-model rewrite;
- no Home/navigation supervisor rewrite;
- no timeout inflation for #53;
- no Reader/provider/auth changes;
- no automatic FreeInk pressure eviction;
- no real-device fix claim.

## Follow-up boundary

P1B may consume this contract to move ownership out of `CrossPointWebServerActivity`; P1C may then migrate HTTP handling to `esp_http_server`; P1D establishes bounded teardown. Each remains independently reviewable.