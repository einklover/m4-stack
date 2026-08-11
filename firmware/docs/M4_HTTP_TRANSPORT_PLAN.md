# M4HttpTransport — root-cause architecture (binding plan)

## Goal (not “make WeRead not OOM”)

Build one **memory-predictable HTTP/TLS substrate** for M4:

```text
Lua Host / Native WeRead / Native JJWXC / Fanqie
                    │
                    ▼
             M4HttpTransport
        (single-flight, persistent client handle,
         TLS lifecycle, timeout, stream sink,
         heap budget hooks)
                    │
             esp_http_client
                    │
             esp-tls / mbedTLS
```

PR #10 TLS pair-reuse is a **short-term A/B**. Final state: **one transport**, providers never own `HTTPClient`/`WiFiClientSecure` lifecycles.

## Memory domains

| Domain | Allowed |
|--------|---------|
| Internal DRAM | Wi-Fi, lwIP, TLS control, NetworkTask stack (fixed), DMA, RTOS |
| PSRAM | body RX/TX, POST, JSON fragments, cookie, catalog, decode, Lua heap, FB |
| SD | shards, combined, chapter text, large raw responses |

## Phase-separated model

**While TLS active:** no std::string growth, no vector growth, no JsonDocument, no second TLS, no font/FB alloc. Only fixed buffers + sink.write + POD progress.

**After TLS inactive:** decode, JSON, UI.

## Multi-agent branches (non-overlapping files)

| Branch | Agent | Files |
|--------|-------|-------|
| `agent/m4-http-transport-core` | A | NEW `src/apps/M4HttpTransport.h`, `src/apps/M4HttpTransport.cpp` only |
| `agent/m4-http-transport-weread` | B | `src/apps/providers/WereadProvider.cpp` only — route fetchChapter HTTP through M4HttpTransport |
| `agent/m4-http-transport-docs` | C | `docs/M4_HTTP_TRANSPORT.md` full design + update `docs/NATIVE_TLS_MEM_COORDINATION.md` |

Base: `feat/native-tls-mem-stack` (includes PR#10 reuse in M4NativeProviderHttp — leave JJ on old path).

## M4HttpTransport API contract (Agent A must implement)

```cpp
namespace M4HttpTransport {
  struct Header { const char* name; const char* value; }; // no owning strings in hot path
  static constexpr size_t kMaxHeaders = 8;
  struct Request {
    const char* method; // "GET"/"POST"
    const char* url;
    Header headers[kMaxHeaders];
    size_t headerCount;
    const char* body; size_t bodyLen;
    size_t maxBytes;
    uint32_t timeoutMs;
    bool followRedirects;
    bool insecureTls;
  };
  struct Result {
    bool ok; int status; size_t bytes;
    char error[48]; // POD, no std::string
  };
  using ProgressFn = void (*)(void* ctx, size_t bytes);
  using CancelFn = bool (*)(void* ctx);

  // Single-flight. Stream body to sink; never accumulate.
  Result requestToSink(const Request& req, M4xJsonStream::Sink& sink,
                       ProgressFn progress, void* progressCtx,
                       CancelFn cancel, void* cancelCtx);

  // Optional session: keep esp_http_client handle for multi-request chapter.
  bool sessionBegin(const char* hostHint /* e.g. weread.qq.com */);
  Result sessionRequestToSink(...); // same as request but reuses handle
  void sessionEnd();

  void shutdown(); // cleanup handle
}
```

Implementation notes for A:
- Use `esp_http_client` + `esp_crt_bundle_attach` (see M4NativeProviderLogin.cpp)
- Single global mutex (or reuse M4NativeProviderHeavyGate)
- PSRAM buffer for RX chunks if needed (8–16KB)
- On HTTPS: call HeavyGate tlsBlockAvailable; error key `tls_internal_oom`
- Progress: throttle ~8KB / 250ms, callback POD only
- Do not use Arduino HTTPClient / WiFiClientSecure

## Agent B (WeRead)
- Keep crypto unchanged
- fetchPsvts + downloadShard use M4HttpTransport instead of M4NativeProviderHttp where possible
- Prefer sessionBegin for whole chapter; if too hard, session per pair is OK for first PR
- Fall back to M4NativeProviderHttp only if transport compile-ifdef off
- Push branch agent/m4-http-transport-weread

## Agent C (docs)
- **DONE**: `docs/M4_HTTP_TRANSPORT.md` is now the authoritative full design. See:
  - Layer diagram + memory domains (§2)
  - Phase separation / zero-malloc hot path (§3, §8)
  - Persistent NetworkTask future vs phase1 sync single-flight (§4)
  - API contract for Agent A (§5)
  - ALWAYSINTERNAL / RESERVE_INTERNAL deferred experiments, do NOT change now (§6)
  - MBEDTLS_DYNAMIC_BUFFER / MEM_ALLOC_MODE deferred experiments, prebuilt framework has neither (§7)
  - WeRead one-session-per-chapter (§9)
  - Lua later migration path (§10)
  - 40KB gate → telemetry not architecture (§11)
  - Phased rollout: core → WeRead → TLS knobs → POD progress → Lua (§12)
- Explicit: do NOT change sdkconfig ALWAYSINTERNAL / RESERVE_INTERNAL in this PR

## Merge order
1. A core → feat/m4-http-transport
2. C docs → same
3. B weread → same
4. pio build -e murphy_m4

## Hard rules
- No APP0 flash / partition erase
- No drive-by refactors outside scope
- Commit + push own branch to origin
- Complete sentences in commits
