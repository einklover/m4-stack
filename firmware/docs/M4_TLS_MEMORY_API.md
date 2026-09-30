# System TLS memory policy

`util/M4TlsMemory.h` owns the TLS allocator and admission policy. Call
`install()` once in M4 `setup()` before networking. Never replace the global
mbedTLS callbacks around an individual request.

`resourcesAvailable()` checks the heap that the installed allocator actually
uses. With the PSRAM hook active it requires internal free/largest of 32/8 KiB
for HTTP/lwIP objects and external free/largest of 128/32 KiB for TLS. Without
the hook it preserves the SDK-default internal-only policy. Allocation failure
still returns null; PSRAM failure never falls back to internal RAM.

The native provider HeavyGate delegates to this API, so App Store and native
providers through `M4HttpTransport` share it. Lua `net.request`,
`net.extractPsvts`, `dl.jsonGet`, file download, JSON-to-file, and progressive
HTTPS connection paths use the same policy. Existing Lua return shapes and
`oom` errors remain compatible. A live `dl.jsonGet` keep-alive skips a fresh
handshake check because its TLS buffers are already allocated.

The startup allocator applies to mbedTLS used by both ESP HTTP client and
Arduino secure clients. Transport/session/certificate behavior remains in the
existing HTTP APIs; this change does not replace their stream parsers or cache
semantics. Native HeavyGate serialization and Lua session ownership remain
unchanged.

This check does not reserve memory. Concurrent allocation, PSRAM exhaustion,
or an oversized response can still fail after admission; callers must handle
their normal error returns. Do not promise that a shared API eliminates every
OOM, and do not disable certificate validation as an OOM workaround.

Regression test: `python3 firmware/tests/test_m4_tls_memory.py`. It compiles the
real allocator/policy with capability shims and verifies the observed
96676-byte internal free / 18420-byte contiguous block case, both allocator
modes, reserve failures, allocation overflow and paired free. Binding checks
are source contracts, not Lua HTTPS runtime evidence.
