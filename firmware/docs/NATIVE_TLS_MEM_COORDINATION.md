# Native TLS / Memory model — multi-agent coordination

## Goal
Align Native HTTPS internal-heap behavior with mature Lua Host path:
- Prefer PSRAM for non-TLS allocations
- Request-scoped arenas
- prepareForTls / afterTls around handshakes
- Keep PR #10 WeRead connection reuse (4→2 handshakes)
- Do NOT change CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL yet

## Repo
- Git root: this directory (m4-firmware)
- Remote: origin = https://github.com/einklover/m4-firmware.git
- Base branch: `feat/native-tls-mem-stack` @ 590e395 (includes HTTPS pair reuse)

## Branches (one agent each)
| Branch | Owner | Scope (non-overlapping) |
|--------|-------|-------------------------|
| `agent/m4-mem-core` | A | NEW only: `src/apps/M4Mem.h` (+ optional tiny `.cpp` if needed). May thin-wrap `src/apps/providers/M4Psram.h` without breaking callers. |
| `agent/m4-http-mem-wire` | B | `M4NativeProviderHttp.cpp/.h`, `M4NativeProviderHeavyGate.h` only — wire prepareForTls/afterTls + M4Mem for buffers; **preserve** WeRead pair reuse & stages 0x321-0x324 |
| `agent/weread-mem-hygiene` | C | `WereadProvider.cpp` (+ `.h` if needed) only — psvts cache, less temporary std::string, use M4Mem where easy |

## Merge order (integrator / parent)
1. A → stack
2. B → stack (may need small fixups if A API names differ)
3. C → stack
4. Push stack, open PR

## Hard rules
- No APP0 / partition / full-chip erase
- No changing platformio SPIRAM ALWAYSINTERNAL knobs
- No drive-by refactors outside file scope
- Compile: `pio run -e murphy_m4` with existing PLATFORMIO_BUILD_FLAGS include if needed
- Commit with complete sentences; push your branch to origin
- If blocked on A API: define the expected API in a short comment and implement against it; parent will fix include paths

## Upstream / deps
- Arduino-ESP32 HTTPClient / NetworkClientSecure: PlatformIO package `framework-arduinoespressif32` (prebuilt mbedTLS; no DYNAMIC_BUFFER)
- ESP-IDF heap: `heap_caps_malloc(MALLOC_CAP_SPIRAM|INTERNAL)`
- Existing helpers: `M4Psram.h`, `M4NativeProviderHeavyGate.h` (tlsBlockAvailable 40KB, diagnosticStage)

## See also
- **`docs/M4_HTTP_TRANSPORT.md`** — authoritative root-cause design for native HTTP/TLS
  transport (M4HttpTransport): provider → M4HttpTransport → esp_http_client → memory
  domains, phase separation, NetworkTask future vs phase1 single-flight, deferred
  ALWAYSINTERNAL / RESERVE_INTERNAL / MBEDTLS_DYNAMIC_BUFFER / MEM_ALLOC_MODE
  experiments, WeRead one-session-per-chapter, Lua migration path, 40KB-gate-as-telemetry,
  and phased rollout. This coordination doc is the branch-level planning layer; the
  transport doc is the design layer.

## Success criteria
- Builds murphy_m4
- Native non-WeRead paths unchanged in behavior (JJ still single-flight HTTP)
- WeRead still uses pair reuse
- TLS path calls prepareForTls before https and afterTls after release
