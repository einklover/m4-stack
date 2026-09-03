# Murphy M4 System Runtime Optimization Design

## Status

Approved direction for #50. P0 implementation is tracked by #51.

Baseline repository ref: `b7dd293b761a96ea8be44486e82ed99173950bff`.
Pinned upstream audit ref: `Free-Ink/freeink-sdk@68425f8eec1246a0be0c0f311540f60ad733fa76`.

## Problem statement

Murphy M4 is an ESP32-S3 N16R8 e-paper device with 16 MiB flash and 8 MiB PSRAM. The firmware already has several local mitigations for scarce internal SRAM, including a PSRAM framebuffer, provider PSRAM helpers, and a heavy-operation gate that protects contiguous internal memory for TLS. Runtime reports nevertheless include UI/page lag, QR login lag, and a Wi-Fi browser-transfer path where Home/Back can become unavailable until Recovery reboot.

The system needs a bottom-up runtime architecture that:

1. preserves contiguous internal SRAM for ESP-IDF/FreeRTOS/Wi-Fi/LWIP/TLS/DMA/ISR-critical work;
2. moves rebuildable or large application state to PSRAM or explicit cold storage;
3. bounds transient allocations and cache growth;
4. keeps network-service lifecycles independent from UI Activities;
5. guarantees navigation escape even when an Activity-owned service is slow or wedged;
6. measures memory pressure, task/queue pressure, and latency before attributing root causes;
7. reuses mature FreeInk and ESP-IDF primitives rather than duplicating them.

## Reuse audit

The current vendored `firmware/open-m4-sdk` is not equivalent to current public FreeInk upstream. The current tree contains only `display` and six `hardware` libraries, while current public upstream also contains `MemoryManager`, `FreeInkBook`, `FreeInkUI`, network modules, recovery/USB modules, additional boards, and materially newer same-name hardware/display implementations.

The vendored tree also contains Murphy-local QEMU and display/runtime fixes. Therefore the integration rule is:

- import new upstream libraries in isolation at a pinned ref;
- merge same-name hardware/display modules semantically and file-by-file;
- never replace `firmware/open-m4-sdk` wholesale;
- preserve current QEMU, APP1, async display, Browser Bridge ownership, and real-device safety contracts.

### FreeInk MemoryManager

Adopt first. It already provides:

- `MemPool::{Internal,Psram,Default}` reporting;
- free bytes, largest free block, and min-ever-free reporting;
- soft/hard internal-memory watermarks;
- priority-ordered cache sink eviction;
- `ensureFree()` and pressure relief;
- named reusable static task-stack slots;
- fixed bump arenas for phase-scoped scratch;
- cache purge / boost utilities.

Do not use this import as justification to remove current `M4Psram` behavior. Upstream static task-stack slots use task-capable internal memory, while current M4 provider workers may deliberately use PSRAM stacks. P0 keeps both and measures before consolidating policy.

### FreeInkBook

Evaluate side-by-side with the current `firmware/lib/Epub` implementation. FreeInkBook is a self-contained streaming EPUB/layout/pagination library with caller-bounded arenas and persisted compact layout/page cache. It is the preferred model for application-level paging: hot working state in RAM/PSRAM, cold layout/page state on SD/flash.

Do not implement transparent Linux-style swap. ESP32-S3 runtime stability is better served by explicit bounded paging/caching where ownership and eviction are deterministic.

### FreeInkUI

Evaluate incrementally for high-risk bounded pages (Wi-Fi list, provider login, browser transfer) after P0/P1. Do not rewrite the entire firmware UI in one migration.

### ESP-IDF primitives

Prefer native infrastructure over new M4-specific equivalents:

- `esp_event` for Wi-Fi/network state transitions;
- `esp_http_server` for browser-transfer service lifecycle and dedicated server execution;
- `esp_timer` for auth polling cadence;
- heap-capability metrics, heap tracing, and per-task diagnostics for evidence.

## Memory architecture

### Tier 0: internal SRAM

Reserved primarily for:

- Wi-Fi/LWIP/TLS internals;
- DMA-capable buffers and drivers;
- ISR/RTOS-critical state;
- small hot objects that materially benefit from internal RAM;
- allocations that explicitly require internal/task-capable capabilities.

Decisions must consider both total free internal bytes and largest contiguous internal block. A system with ample aggregate free bytes can still fail TLS or a large allocation when fragmented.

### Tier 1: PSRAM

Preferred for:

- large application buffers;
- decoded images/covers;
- page/layout caches where latency allows;
- provider response/model buffers;
- eligible application worker stacks already validated as PSRAM-safe;
- bounded arenas whose users do not require DMA/internal capabilities.

### Tier 2: SD/SPIFFS

Explicit cold backing store for:

- layout/page caches;
- image/cover cache;
- catalog/provider cold state;
- append-only cache journals and periodic compaction;
- data too large or too cold to justify PSRAM residency.

## P0 observability contract

P0 must not claim a specific root cause for the Wi-Fi transfer navigation failure before evidence exists.

A runtime snapshot must distinguish:

- internal free bytes;
- internal largest free block;
- internal min-ever-free;
- PSRAM free/min-ever-free;
- MemoryManager pressure level;
- selected task stack high-water marks where available;
- Activity transition and navigation timestamps;
- Wi-Fi connect, auth poll, HTTP, and service start/stop elapsed time.

The diagnostic path must be low-overhead and safe to leave compiled in development builds. Production logging policy can remain runtime/build gated.

## Service architecture after P0

### Wi-Fi service

Move connection state from polling-oriented Activity ownership toward an event-driven facade backed by ESP-IDF Wi-Fi/IP events. UI subscribes to bounded state snapshots/events and can detach immediately.

### Browser transfer service

Move HTTP/DNS/mDNS/transfer lifecycle out of `CrossPointWebServerActivity`. The Activity becomes a view/controller over a separately owned service. Home/Back must not wait indefinitely for HTTP/client/render cleanup.

Use `esp_http_server` unless measurement proves an integration blocker. Keep upload/download operations streaming and bounded; no whole-file buffering.

### Provider auth

Keep provider HTTP work off the UI thread. Generate a QR bitmap only when the URL changes, cache the 1-bit result, and update only status/timer regions between polls. Prefer `esp_timer` or a bounded scheduler for poll cadence rather than sleep loops in UI logic.

### Navigation supervisor

Introduce a system-level navigation escape path after the blocking/stall evidence is captured. The navigation request must be able to detach the foreground Activity promptly; slow cleanup is cancelled or completed asynchronously with bounded deadlines. Never require `portMAX_DELAY` on a foreground navigation path without an independently guaranteed recovery route.

## Task and queue policy

Prefer a small fixed set of long-lived workers and bounded latest/limited-depth queues over per-Activity transient task proliferation. Preserve existing Browser Bridge invariants: bounded latest-frame-wins behavior, explicit display ownership, ACK semantics independent of panel completion, and stable heap/PSRAM over repeated lifecycles.

## Cache/storage policy

Rebuildable caches register MemoryManager sinks with explicit eviction priority. Cache indexes should be bounded in RAM. Large or growing indexes use append journal + periodic compaction rather than repeatedly materializing the full index into `std::vector<std::string>` and rewriting it.

## Rollout

1. P0: runtime evidence + isolated MemoryManager import (#51).
2. P1: cache-sink registration and M4 memory budgets.
3. P2: allocation policy consolidation; retain PSRAM-safe task stacks where measured beneficial.
4. P3: event-driven Wi-Fi service facade.
5. P4: `esp_http_server` browser-transfer extraction.
6. P5: guaranteed navigation supervisor/escape.
7. P6: provider auth/QR timer and cache cleanup.
8. P7: FreeInkBook side-by-side reader adapter and measured migration.
9. P8: FreeInkUI incremental adoption on high-risk pages.
10. P9: bounded cache journal/cold-state spill.
11. P10: stress/soak/performance budgets and production validation.

Each behavior-changing phase gets a separate issue/PR. Each PR records baseline/after evidence, relevant host/QEMU tests, production build status, and whether any claim requires real-device validation.

## P0 non-goals

- no Wi-Fi behavior rewrite;
- no HTTP-server replacement;
- no navigation supervisor;
- no FreeInkBook/FreeInkUI migration;
- no broad same-name SDK sync;
- no transparent virtual memory/swap;
- no real-device flashing as part of GitHub-only implementation work.

## Acceptance principles

- Measurement precedes root-cause claims.
- New bounded-memory behavior has automated regression coverage.
- Current main behavior remains intact during the observability import.
- No private dependency fetch is reintroduced.
- `murphy_m4` remains the production build contract.
- QEMU profiles remain simulator-only.
- Same-name upstream hardware/display changes require separate semantic review rather than being smuggled into P0.
