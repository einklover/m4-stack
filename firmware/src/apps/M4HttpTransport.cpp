#include "apps/M4HttpTransport.h"

#include "apps/providers/M4NativeProviderHeavyGate.h"

#include <Arduino.h>
#include <SDCardManager.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_heap_caps.h>

#include <cstdio>
#include <cstring>

namespace M4HttpTransport {
namespace {

constexpr const char* kDefaultUa = "Mozilla/5.0 Murphy-M4 M4HttpTransport/1";
constexpr size_t kProgressBytes = 8u * 1024u;
constexpr uint32_t kProgressMs = 250u;

bool gDebug = false;
char gDebugLogPath[96] = {};

bool debugActive() { return gDebug || gDebugLogPath[0]; }

struct RxCtx {
  M4xJsonStream::Sink* sink = nullptr;
  size_t maxBytes = 0;
  size_t bytes = 0;
  ProgressFn progress = nullptr;
  void* progressCtx = nullptr;
  CancelFn cancel = nullptr;
  void* cancelCtx = nullptr;
  size_t lastProgressBytes = 0;
  uint32_t lastProgressMs = 0;
  bool overflow = false;
  bool sinkFailed = false;
  bool cancelled = false;
};

// Streams body chunks straight into the sink; never accumulates. Returning
// ESP_FAIL aborts the transfer so perform() can map the failure to Result.
esp_err_t httpEvent(esp_http_client_event_t* evt) {
  auto* ctx = static_cast<RxCtx*>(evt->user_data);
  if (!ctx || evt->event_id != HTTP_EVENT_ON_DATA || !evt->data || evt->data_len <= 0) {
    return ESP_OK;
  }
  if (ctx->cancel && ctx->cancel(ctx->cancelCtx)) {
    ctx->cancelled = true;
    return ESP_FAIL;
  }
  const size_t n = static_cast<size_t>(evt->data_len);
  if (ctx->bytes > ctx->maxBytes || n > ctx->maxBytes - ctx->bytes) {
    ctx->overflow = true;
    return ESP_FAIL;
  }
  if (!ctx->sink->write(static_cast<const uint8_t*>(evt->data), n)) {
    ctx->sinkFailed = true;
    return ESP_FAIL;
  }
  ctx->bytes += n;
  // A bounded extractor may have enough records after this chunk. Check again
  // after the sink consumed it so chunked/no-Content-Length peers cannot keep
  // the request alive waiting for an EOF that never arrives.
  if (ctx->cancel && ctx->cancel(ctx->cancelCtx)) {
    ctx->cancelled = true;
    return ESP_FAIL;
  }
  if (ctx->progress) {
    const uint32_t now = millis();
    if (ctx->lastProgressBytes == 0 || ctx->bytes - ctx->lastProgressBytes >= kProgressBytes ||
        now - ctx->lastProgressMs >= kProgressMs) {
      ctx->lastProgressBytes = ctx->bytes;
      ctx->lastProgressMs = now;
      ctx->progress(ctx->progressCtx, ctx->bytes);
    }
  }
  return ESP_OK;
}

void setError(Result& r, const char* msg) {
  if (msg) std::snprintf(r.error, sizeof(r.error), "%s", msg);
}

bool isHttps(const char* url) { return url && std::strncmp(url, "https://", 8) == 0; }

void appendDebugLog(const char* line) {
  if (!gDebugLogPath[0] || !line || !SdMan.ready()) return;
  // Prefer append so step runs accumulate; fall back to truncate write.
  FsFile f = SdMan.open(gDebugLogPath, O_WRONLY | O_CREAT | O_APPEND);
  if (!f) {
    if (!SdMan.openFileForWrite("M4HttpLog", gDebugLogPath, f)) return;
  }
  const size_t n = std::strlen(line);
  if (n) f.write(reinterpret_cast<const uint8_t*>(line), n);
  f.write(reinterpret_cast<const uint8_t*>("\n"), 1);
  f.sync();
  f.close();
}

void logStep(const char* stage, const char* detail, const MemSnap* mem) {
  if (!gDebug && !gDebugLogPath[0]) return;
  char line[240];
  if (mem) {
    std::snprintf(line, sizeof(line),
                  "[M4Http] %s %s heap=%u min=%u psram=%u int=%u larg=%u tls=%d",
                  stage ? stage : "?", detail ? detail : "",
                  static_cast<unsigned>(mem->freeHeap), static_cast<unsigned>(mem->minFreeHeap),
                  static_cast<unsigned>(mem->freePsram), static_cast<unsigned>(mem->freeInternal),
                  static_cast<unsigned>(mem->largestInternal), mem->tlsGateOk ? 1 : 0);
  } else {
    std::snprintf(line, sizeof(line), "[M4Http] %s %s", stage ? stage : "?",
                  detail ? detail : "");
  }
  if (gDebug) {
    Serial.println(line);
    Serial.flush();
  }
  if (gDebugLogPath[0]) appendDebugLog(line);
}

esp_http_client_config_t makeConfig(const Request& req) {
  esp_http_client_config_t cfg{};
  cfg.url = req.url;
  cfg.event_handler = httpEvent;
  cfg.timeout_ms = req.timeoutMs;
  cfg.disable_auto_redirect = !req.followRedirects;
  if (req.insecureTls) {
    cfg.skip_cert_common_name_check = true;
  } else {
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
  }
  return cfg;
}

// Persistent session handle plus fixed header-name slots, so a later request
// does not inherit stale headers. Header bookkeeping must not malloc/free in
// the TLS hot path.
esp_http_client_handle_t gSession = nullptr;
constexpr size_t kHeaderNameBytes = 48;
char gSessionHeaders[kMaxHeaders][kHeaderNameBytes] = {};
size_t gSessionHeaderCount = 0;

void sessionFreeHeaders() {
  for (size_t i = 0; i < gSessionHeaderCount; ++i) {
    if (gSessionHeaders[i][0]) {
      esp_http_client_delete_header(gSession, gSessionHeaders[i]);
      gSessionHeaders[i][0] = 0;
    }
  }
  gSessionHeaderCount = 0;
}

void sessionRecordHeaders(const Request& req) {
  sessionFreeHeaders();
  for (size_t i = 0; i < req.headerCount && i < kMaxHeaders; ++i) {
    if (req.headers[i].name) {
      std::snprintf(gSessionHeaders[gSessionHeaderCount], kHeaderNameBytes, "%.*s",
                    static_cast<int>(kHeaderNameBytes - 1), req.headers[i].name);
      ++gSessionHeaderCount;
    }
  }
}

Result perform(esp_http_client_handle_t h, const Request& req, RxCtx& ctx, const char* tag) {
  Result out;
  M4NativeProviderHeavyGate::diagnosticStage() = 0x411;
  esp_http_client_set_user_data(h, &ctx);
  esp_http_client_set_method(h, std::strcmp(req.method, "POST") == 0 ? HTTP_METHOD_POST : HTTP_METHOD_GET);

  char urlBrief[64] = {};
  if (req.url) {
    std::snprintf(urlBrief, sizeof(urlBrief), "%.60s", req.url);
  }
  if (debugActive()) {
    MemSnap m = memSnap();
    char det[96];
    std::snprintf(det, sizeof(det), "%s %s", req.method ? req.method : "?", urlBrief);
    logStep(tag ? tag : "perform_pre", det, &m);
  }

  M4NativeProviderHeavyGate::diagnosticStage() = 0x412;
  esp_http_client_set_url(h, req.url);
  if (req.body && req.bodyLen) {
    esp_http_client_set_post_field(h, req.body, static_cast<int>(req.bodyLen));
  } else {
    esp_http_client_set_post_field(h, nullptr, 0);
  }
  esp_http_client_set_header(h, "User-Agent", kDefaultUa);
  esp_http_client_set_header(h, "Accept-Encoding", "identity");
  for (size_t i = 0; i < req.headerCount && i < kMaxHeaders; ++i) {
    if (req.headers[i].name && req.headers[i].value) {
      esp_http_client_set_header(h, req.headers[i].name, req.headers[i].value);
    }
  }

  M4NativeProviderHeavyGate::diagnosticStage() = 0x413;
  const uint32_t t0 = millis();
  const esp_err_t err = esp_http_client_perform(h);
  const uint32_t dt = millis() - t0;
  M4NativeProviderHeavyGate::diagnosticStage() = 0x414;

  out.status = esp_http_client_get_status_code(h);
  out.bytes = ctx.bytes;
  if (ctx.cancelled) {
    setError(out, "cancelled");
  } else if (ctx.overflow) {
    setError(out, "response_too_large");
  } else if (ctx.sinkFailed) {
    setError(out, "sink_write_failed");
  } else if (err != ESP_OK) {
    // Keep POD error short; include esp err name for step debug.
    char buf[48];
    std::snprintf(buf, sizeof(buf), "http_%s", esp_err_to_name(err));
    setError(out, buf);
  } else if (out.status < 200 || out.status >= 300) {
    std::snprintf(out.error, sizeof(out.error), "http_%d", out.status);
  } else {
    out.ok = true;
  }

  if (debugActive()) {
    MemSnap m = memSnap();
    char det[120];
    std::snprintf(det, sizeof(det), "ok=%d st=%d bytes=%u err=%s ms=%u esp=%s",
                  out.ok ? 1 : 0, out.status, static_cast<unsigned>(out.bytes),
                  out.error[0] ? out.error : "-", static_cast<unsigned>(dt),
                  esp_err_to_name(err));
    logStep(tag ? tag : "perform_post", det, &m);
  }
  return out;
}

}  // namespace

MemSnap memSnap() {
  MemSnap m;
#if defined(ARDUINO_ARCH_ESP32)
  m.freeHeap = static_cast<uint32_t>(ESP.getFreeHeap());
  m.minFreeHeap = static_cast<uint32_t>(ESP.getMinFreeHeap());
  m.freePsram = static_cast<uint32_t>(ESP.getFreePsram());
  m.freeInternal =
      static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  m.largestInternal = static_cast<uint32_t>(
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  m.tlsGateOk = M4NativeProviderHeavyGate::tlsBlockAvailable();
#endif
  return m;
}

void setDebug(bool on) { gDebug = on; }
bool debugEnabled() { return gDebug; }

void setDebugLogPath(const char* absPath) {
  if (!absPath || !absPath[0]) {
    gDebugLogPath[0] = 0;
    return;
  }
  std::snprintf(gDebugLogPath, sizeof(gDebugLogPath), "%.*s",
                static_cast<int>(sizeof(gDebugLogPath) - 1), absPath);
}

void debugStep(const char* stage, const char* detail) {
  if (!debugActive()) return;
  MemSnap m = memSnap();
  // Force a line even if gDebug is off when SD path is set.
  const bool prev = gDebug;
  if (gDebugLogPath[0]) gDebug = true;
  logStep(stage, detail, &m);
  gDebug = prev;
}

bool sessionOpen() { return gSession != nullptr; }

Result requestToSink(const Request& req, M4xJsonStream::Sink& sink, ProgressFn progress,
                     void* progressCtx, CancelFn cancel, void* cancelCtx) {
  Result out;
  M4NativeProviderHeavyGate::Lock lock(M4NativeProviderHeavyGate::mutex());
  M4NativeProviderHeavyGate::diagnosticStage() = 0x420;
  if (debugActive()) {
    MemSnap m = memSnap();
    logStep("oneshot_enter", req.url ? req.url : "", &m);
  }
  if (isHttps(req.url) && !M4NativeProviderHeavyGate::tlsBlockAvailable()) {
    // Drop a stale session (if any) and re-check once — consecutive chapters
    // often free TLS buffers only after cleanup.
    if (gSession) {
      sessionFreeHeaders();
      esp_http_client_cleanup(gSession);
      gSession = nullptr;
    }
    if (!M4NativeProviderHeavyGate::tlsBlockAvailable()) {
      setError(out, "tls_internal_oom");
      logStep("oneshot_tls_gate", "tls_internal_oom", nullptr);
      return out;
    }
  }

  RxCtx ctx;
  ctx.sink = &sink;
  ctx.maxBytes = req.maxBytes;
  ctx.progress = progress;
  ctx.progressCtx = progressCtx;
  ctx.cancel = cancel;
  ctx.cancelCtx = cancelCtx;

  M4NativeProviderHeavyGate::diagnosticStage() = 0x421;
  esp_http_client_config_t cfg = makeConfig(req);
  esp_http_client_handle_t h = esp_http_client_init(&cfg);
  if (!h) {
    setError(out, "http_init_failed");
    logStep("oneshot_init", "http_init_failed", nullptr);
    return out;
  }
  out = perform(h, req, ctx, "oneshot");
  M4NativeProviderHeavyGate::diagnosticStage() = 0x422;
  esp_http_client_cleanup(h);
  if (debugActive()) {
    MemSnap m = memSnap();
    logStep("oneshot_exit", out.ok ? "ok" : out.error, &m);
  }
  return out;
}

bool sessionBegin(const char* hostHint) {
  M4NativeProviderHeavyGate::Lock lock(M4NativeProviderHeavyGate::mutex());
  M4NativeProviderHeavyGate::diagnosticStage() = 0x400;
  if (gSession) {
    logStep("session_begin", "already_open", nullptr);
    return true;
  }

  char base[160];
  const char* host = (hostHint && hostHint[0]) ? hostHint : "weread.qq.com";
  std::snprintf(base, sizeof(base), "https://%.*s/", 120, host);
  if (debugActive()) {
    MemSnap m = memSnap();
    logStep("session_begin_pre", base, &m);
  }
  if (!M4NativeProviderHeavyGate::tlsBlockAvailable()) {
    logStep("session_begin", "tls_internal_oom_precheck", nullptr);
    // Still allow init (client alloc only); handshake later may fail — surface precheck.
  }

  esp_http_client_config_t cfg{};
  cfg.url = base;
  cfg.event_handler = httpEvent;
  cfg.timeout_ms = 30000;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.disable_auto_redirect = false;
  M4NativeProviderHeavyGate::diagnosticStage() = 0x401;
  gSession = esp_http_client_init(&cfg);
  M4NativeProviderHeavyGate::diagnosticStage() = 0x402;
  if (debugActive()) {
    MemSnap m = memSnap();
    logStep("session_begin_post", gSession ? "ok" : "init_failed", &m);
  }
  return gSession != nullptr;
}

Result sessionRequestToSink(const Request& req, M4xJsonStream::Sink& sink, ProgressFn progress,
                            void* progressCtx, CancelFn cancel, void* cancelCtx) {
  Result out;
  M4NativeProviderHeavyGate::Lock lock(M4NativeProviderHeavyGate::mutex());
  M4NativeProviderHeavyGate::diagnosticStage() = 0x410;
  if (!gSession) {
    setError(out, "session_not_open");
    logStep("session_req", "session_not_open", nullptr);
    return out;
  }
  // Do not re-run the pre-handshake gate for every request in an open session.
  // A live TLS connection has already allocated its buffers, so largest-block
  // naturally falls below the cold-handshake threshold. Blocking here broke
  // the second/third shard and defeated connection reuse. If the peer closed
  // the socket, esp_http_client_perform() reconnects and reports the real error.

  sessionRecordHeaders(req);
  RxCtx ctx;
  ctx.sink = &sink;
  ctx.maxBytes = req.maxBytes;
  ctx.progress = progress;
  ctx.progressCtx = progressCtx;
  ctx.cancel = cancel;
  ctx.cancelCtx = cancelCtx;
  return perform(gSession, req, ctx, "session");
}

void sessionEnd() {
  M4NativeProviderHeavyGate::Lock lock(M4NativeProviderHeavyGate::mutex());
  M4NativeProviderHeavyGate::diagnosticStage() = 0x430;
  if (gSession) {
    if (debugActive()) {
      MemSnap m = memSnap();
      logStep("session_end_pre", "", &m);
    }
    sessionFreeHeaders();
    esp_http_client_cleanup(gSession);
    gSession = nullptr;
    if (debugActive()) {
      MemSnap m = memSnap();
      logStep("session_end_post", "closed", &m);
    }
  }
}

void shutdown() { sessionEnd(); }

}  // namespace M4HttpTransport
