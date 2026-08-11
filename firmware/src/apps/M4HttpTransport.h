#pragma once

// Memory-predictable HTTP/TLS substrate for M4 native providers. Bodies are
// streamed chunk-by-chunk from esp_http_client straight into an
// M4xJsonStream::Sink; nothing is accumulated while TLS is active. All
// entry points are single-flight under the shared M4NativeProviderHeavyGate
// lock so two handshakes never overlap their internal-RAM peaks.

#include "apps/M4xJsonStream.h"

#include <cstddef>
#include <cstdint>

namespace M4HttpTransport {

struct Header {
  const char* name;   // no owning strings in hot path
  const char* value;
};

static constexpr size_t kMaxHeaders = 8;

struct Request {
  const char* method = "GET";   // "GET"/"POST"
  const char* url = nullptr;
  Header headers[kMaxHeaders];
  size_t headerCount = 0;
  const char* body = nullptr;
  size_t bodyLen = 0;
  size_t maxBytes = 4u * 1024u * 1024u;
  uint32_t timeoutMs = 30000;
  bool followRedirects = false;
  bool insecureTls = false;   // opt out of CA validation; keep false for credential bearers
};

struct Result {
  bool ok = false;
  int status = 0;
  size_t bytes = 0;
  char error[48] = {};   // POD, no std::string
};

// Heap / TLS-gate snapshot for step debugging (POD, safe to log).
struct MemSnap {
  uint32_t freeHeap = 0;
  uint32_t minFreeHeap = 0;
  uint32_t freePsram = 0;
  uint32_t freeInternal = 0;
  uint32_t largestInternal = 0;
  bool tlsGateOk = false;
};

using ProgressFn = void (*)(void* ctx, size_t bytes);
using CancelFn = bool (*)(void* ctx);

// Single-flight. Stream body to sink; never accumulate. Creates and tears down
// its own esp_http_client handle.
Result requestToSink(const Request& req, M4xJsonStream::Sink& sink,
                     ProgressFn progress, void* progressCtx,
                     CancelFn cancel, void* cancelCtx);

// Optional session: one persistent esp_http_client_handle_t reused across a
// multi-request chapter so the TLS connection stays warm. Not thread-safe with
// concurrent sessions; single-flight lock still applies per call.
bool sessionBegin(const char* hostHint /* e.g. weread.qq.com */);
Result sessionRequestToSink(const Request& req, M4xJsonStream::Sink& sink,
                            ProgressFn progress, void* progressCtx,
                            CancelFn cancel, void* cancelCtx);
void sessionEnd();

// Cleanup any session handle and release TLS memory. Safe to call at any time.
void shutdown();

// --- Step / base debugging (safe to leave on; off by default for noise) ---
MemSnap memSnap();
bool sessionOpen();
void setDebug(bool on);
bool debugEnabled();
// When set, append step lines to this SD path (apps_data only recommended).
// Empty/null disables SD logging. Serial [M4Http] lines still follow setDebug.
void setDebugLogPath(const char* absPath);
// Manual breadcrumb: stage tag + optional detail (also writes when debug on).
void debugStep(const char* stage, const char* detail = nullptr);

}  // namespace M4HttpTransport
