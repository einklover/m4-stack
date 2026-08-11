#include "apps/providers/M4NativeProviderHttp.h"

#include "apps/M4HttpTransport.h"
#include "apps/providers/M4NativeProviderHeavyGate.h"
#include "apps/providers/M4NativeWifi.h"
#include "apps/providers/M4Psram.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace M4NativeProviderHttp {
namespace {

// Compatibility sink for the few protocol helpers that still need a bounded
// response body after the TLS transaction has completed. Storage prefers
// PSRAM; large discovery/catalog/chapter bodies continue to stream directly
// into their caller-provided sink.
class StringSink final : public M4xJsonStream::Sink {
 public:
  explicit StringSink(size_t cap) : cap_(cap) {
    const size_t initial = std::min<size_t>(cap_, 4096u);
    if (initial == 0) return;
    buf_ = static_cast<char*>(M4Psram::mallocPrefer(initial));
    if (buf_) capacity_ = initial;
  }

  ~StringSink() override { M4Psram::freePrefer(buf_); }

  bool write(const uint8_t* data, size_t len) override {
    if (!data || len == 0) return true;
    if (!buf_ || size_ > cap_ || len > cap_ - size_) return false;
    if (size_ + len > capacity_) {
      size_t next = capacity_ ? capacity_ * 2u : 4096u;
      while (next < size_ + len && next < cap_) next *= 2u;
      next = std::min(next, cap_);
      if (next < size_ + len) return false;
      char* nb = static_cast<char*>(M4Psram::mallocPrefer(next));
      if (!nb) return false;
      if (size_) std::memcpy(nb, buf_, size_);
      M4Psram::freePrefer(buf_);
      buf_ = nb;
      capacity_ = next;
    }
    std::memcpy(buf_ + size_, data, len);
    size_ += len;
    return true;
  }

  std::string take() {
    if (!buf_ || size_ == 0) return {};
    std::string out(buf_, size_);
    size_ = 0;
    return out;
  }

 private:
  size_t cap_ = 0;
  char* buf_ = nullptr;
  size_t capacity_ = 0;
  size_t size_ = 0;
};

struct CallbackCtx {
  const ProgressFn* progress = nullptr;
  const CancelFn* cancelled = nullptr;
};

void progressShim(void* opaque, size_t bytes) {
  const auto* ctx = static_cast<const CallbackCtx*>(opaque);
  if (ctx && ctx->progress && *ctx->progress) (*ctx->progress)(bytes);
}

bool cancelShim(void* opaque) {
  const auto* ctx = static_cast<const CallbackCtx*>(opaque);
  return ctx && ctx->cancelled && *ctx->cancelled && (*ctx->cancelled)();
}

M4HttpTransport::Request toTransportRequest(const Request& req) {
  M4HttpTransport::Request out;
  out.method = req.method.empty() ? "GET" : req.method.c_str();
  out.url = req.url.c_str();
  out.headerCount = std::min<size_t>(req.headers.size(), M4HttpTransport::kMaxHeaders);
  for (size_t i = 0; i < out.headerCount; ++i) {
    out.headers[i].name = req.headers.items[i].name.c_str();
    out.headers[i].value = req.headers.items[i].value.c_str();
  }
  out.body = req.body.empty() ? nullptr : req.body.data();
  out.bodyLen = req.body.size();
  out.maxBytes = req.maxBytes;
  out.timeoutMs = req.timeoutMs;
  out.followRedirects = req.followRedirects;
  out.insecureTls = req.insecureTls;
  return out;
}

Result fromTransportResult(const M4HttpTransport::Result& src) {
  Result out;
  out.ok = src.ok;
  out.status = src.status;
  out.bytes = src.bytes;
  if (src.error[0]) {
    // Preserve the old compatibility-layer spelling consumed by a few UI
    // error maps while the underlying transport uses the more precise name.
    if (std::strcmp(src.error, "sink_write_failed") == 0) out.error = "sink_failed";
    else out.error = src.error;
  }
  return out;
}

bool canRetryZeroByteTransport(const Result& result, const Request& req) {
  if (result.ok || result.bytes != 0 || result.status != 0 || result.error.empty()) return false;
  if (!req.method.empty() && req.method != "GET") return false;

  // ESP-IDF may report a short-lived connect/fetch-header failure while Wi-Fi
  // still says WL_CONNECTED. A fresh one-shot esp_http_client handle is safe to
  // retry only before the sink has received any bytes; after the first body
  // byte the parser/file sink is intentionally not rewindable.
  const std::string& e = result.error;
  if (e.rfind("http_ESP_ERR_HTTP_", 0) == 0) {
    if (e.find("INVALID_ARG") != std::string::npos ||
        e.find("INVALID_STATE") != std::string::npos ||
        e.find("MAX_REDIRECT") != std::string::npos) {
      return false;
    }
    return true;
  }
  return e == "http_ESP_ERR_TCP_TRANSPORT_CONNECTION_FAILED" ||
         e == "http_ESP_ERR_TCP_TRANSPORT_CONNECTION_CLOSED";
}

Result perform(const Request& req, M4xJsonStream::Sink& sink,
               const ProgressFn& progress, const CancelFn& cancelled) {
  Result out;
  if (req.url.empty() || req.maxBytes == 0) {
    out.error = "bad_request";
    return out;
  }
  if (cancelled && cancelled()) {
    out.error = "cancelled";
    return out;
  }

  // Keep the old provider contract responsible for bringing Wi-Fi up. The
  // shared transport intentionally owns only HTTP/TLS and heavy-resource
  // serialization, so this thin adapter is the single compatibility boundary
  // for JJWXC/Fanqie discovery, catalog and chapter callers.
  const auto wifi = M4NativeWifi::ensureConnected(std::min<uint32_t>(req.timeoutMs, 20000u), cancelled);
  if (!wifi.ok) {
    out.error = wifi.error.empty() ? "wifi_not_connected" : wifi.error;
    return out;
  }
  if (cancelled && cancelled()) {
    out.error = "cancelled";
    return out;
  }

  if (!M4NativeProviderHeavyGate::heapHealthy(0x300)) {
    out.error = "heap_corrupt";
    return out;
  }

  const M4HttpTransport::Request tr = toTransportRequest(req);
  CallbackCtx ctx;
  ctx.progress = &progress;
  ctx.cancelled = &cancelled;
  auto net = M4HttpTransport::requestToSink(
      tr, sink, progress ? progressShim : nullptr, &ctx,
      cancelled ? cancelShim : nullptr, &ctx);
  out = fromTransportResult(net);

  if (canRetryZeroByteTransport(out, req) && !(cancelled && cancelled())) {
    // Re-run the cheap Wi-Fi guard because the first failed socket may have
    // coincided with a station state transition. If association is still good
    // this is immediate; if not, the existing saved-network reconnect policy
    // gets one bounded chance before the fresh HTTP handle is created.
    const auto retryWifi = M4NativeWifi::ensureConnected(
        std::min<uint32_t>(req.timeoutMs, 12000u), cancelled);
    if (!retryWifi.ok) {
      out.error = retryWifi.error.empty() ? "wifi_not_connected" : retryWifi.error;
      return out;
    }
    if (cancelled && cancelled()) {
      out.error = "cancelled";
      return out;
    }

    net = M4HttpTransport::requestToSink(
        tr, sink, progress ? progressShim : nullptr, &ctx,
        cancelled ? cancelShim : nullptr, &ctx);
    out = fromTransportResult(net);
  }
  return out;
}

}  // namespace

Result requestToSink(const Request& req, M4xJsonStream::Sink& sink,
                     const ProgressFn& progress, const CancelFn& cancelled) {
  return perform(req, sink, progress, cancelled);
}

bool requestSmall(const Request& req, std::string& bodyOut, Result& resultOut,
                  size_t hardCap, const CancelFn& cancelled) {
  bodyOut.clear();
  Request bounded = req;
  bounded.maxBytes = std::min(req.maxBytes, hardCap);
  if (bounded.maxBytes == 0) {
    resultOut = {};
    resultOut.error = "bad_request";
    return false;
  }
  StringSink sink(bounded.maxBytes);
  resultOut = perform(bounded, sink, {}, cancelled);
  if (!resultOut.ok) return false;
  bodyOut = sink.take();
  return true;
}

}  // namespace M4NativeProviderHttp