#include "apps/providers/M4NativeProvider.h"
#include "apps/providers/M4NativeProviderHttp.h"
#include "apps/providers/M4NativeProviderIo.h"
#include "apps/providers/M4NativeProviderHeavyGate.h"
#include "apps/providers/M4NativeWifi.h"
#include "apps/providers/M4Psram.h"

#include "apps/M4xPsvtsExtract.h"
#include "apps/weread/WereadCrypto.h"

// Single-flight HTTP/TLS substrate (agent A, branch m4-http-transport-core).
// B uses it when present and falls back to the std::function-based native
// bridge so this tree still builds before A merges.
#if __has_include("apps/M4HttpTransport.h")
#include "apps/M4HttpTransport.h"
#define M4_HTTP_TRANSPORT_AVAILABLE 1
#else
#define M4_HTTP_TRANSPORT_AVAILABLE 0
#endif

#include <Arduino.h>
#include <SDCardManager.h>
#include <mbedtls/md5.h>
#include <esp_random.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <time.h>
#include <vector>

namespace M4NativeProviderAdapters {
namespace {

class DirectFileSink final : public M4xJsonStream::Sink {
 public:
  ~DirectFileSink() override { close(); }

  bool open(const std::string& path) {
    close();
    path_ = path;
    M4NativeProviderIo::ensureParentDirs(path_);
    if (SdMan.exists(path_.c_str())) SdMan.remove(path_.c_str());
    open_ = SdMan.openFileForWrite("WR-TMP", path_.c_str(), f_);
    bytes_ = 0;
    return open_;
  }

  bool write(const uint8_t* data, size_t len) override {
    if (!open_ || !data) return false;
    if (len == 0) return true;
    const size_t n = f_.write(data, len);
    if (n != len) return false;
    bytes_ += len;
    return true;
  }

  void close() {
    if (open_) {
      f_.close();
      open_ = false;
    }
  }

  size_t bytes() const { return bytes_; }

 private:
  FsFile f_;
  std::string path_;
  size_t bytes_ = 0;
  bool open_ = false;
};

class PsvtsSink final : public M4xJsonStream::Sink {
 public:
  PsvtsSink() { scanner_.reset(M4xPsvts::kMaxValueLen); }

  bool write(const uint8_t* data, size_t len) override {
    if (!data || len == 0) return true;
    if (scanned_ > M4xPsvts::kMaxScanBytes || len > M4xPsvts::kMaxScanBytes - scanned_) return false;
    scanned_ += len;
    if (!scanner_.found && !scanner_.valueTooLarge) scanner_.feed(data, len);
    return !scanner_.valueTooLarge;
  }

  bool found() const { return scanner_.found && !scanner_.value.empty(); }
  const std::string& value() const { return scanner_.value; }

 private:
  M4xPsvts::Scanner scanner_;
  size_t scanned_ = 0;
};

class PrefixSink final : public M4xJsonStream::Sink {
 public:
  bool write(const uint8_t* data, size_t len) override {
    if (!data || len == 0) return true;
    const size_t room = sizeof(buf_) - 1u - used_;
    const size_t n = std::min(room, len);
    if (n) {
      std::memcpy(buf_ + used_, data, n);
      used_ += n;
      buf_[used_] = 0;
    }
    return true;
  }

  std::string value() const { return std::string(buf_, used_); }

 private:
  char buf_[512] = {};
  size_t used_ = 0;
};

// POD request descriptor shared by both transports: M4HttpTransport needs
// const char* fields (no owning strings in the hot path), M4NativeProviderHttp
// needs std::string. The bridge keeps the chapter fetch logic single-source.
struct NetReq {
  const char* method = "GET";
  const char* url = nullptr;
  const char* hNames[4] = {};
  const char* hValues[4] = {};
  size_t hCount = 0;
  const char* body = nullptr;
  size_t bodyLen = 0;
  size_t maxBytes = 4u * 1024u * 1024u;
  uint32_t timeoutMs = 30000;
};

struct NetResult {
  bool ok = false;
  int status = 0;
  std::string error;
};

bool wrTraceEnabled() {
#if M4_HTTP_TRANSPORT_AVAILABLE
  return M4HttpTransport::debugEnabled();
#else
  return false;
#endif
}

#if M4_HTTP_TRANSPORT_AVAILABLE
// POD progress/cancel shim: the transport only calls back with a context
// pointer, so route into the existing std::function-based M4NativeProvider
// callbacks without allocating while TLS is active.
struct PodCtx {
  const M4NativeProvider::ProgressFn* progress = nullptr;
  const M4NativeProvider::CancelFn* cancelled = nullptr;
  // When set, abort the HTTP body as soon as psvts is extracted so we do not
  // pull the rest of a ~0.5–1MB reader HTML page (major fragmentation source
  // between consecutive chapters).
  const PsvtsSink* stopWhenPsvts = nullptr;
};

void podProgress(void* ctx, size_t bytes) {
  const auto* p = static_cast<const PodCtx*>(ctx);
  if (p && p->progress && *p->progress) {
    (*p->progress)(M4NativeProvider::Phase::Receiving, bytes, 0, 0);
  }
}

bool podCancel(void* ctx) {
  const auto* p = static_cast<const PodCtx*>(ctx);
  if (!p) return false;
  if (p->stopWhenPsvts && p->stopWhenPsvts->found()) return true;
  return p->cancelled && (*p->cancelled)();
}
#endif

NetResult netRequest(const NetReq& r, M4xJsonStream::Sink& sink,
                     const M4NativeProvider::ProgressFn& progress,
                     const M4NativeProvider::CancelFn& cancelled,
                     bool useSession, const PsvtsSink* stopWhenPsvts = nullptr) {
#if M4_HTTP_TRANSPORT_AVAILABLE
  // Same STA bring-up as Fanqie/JJWXC (M4NativeProviderHttp). WeRead used
  // M4HttpTransport directly and skipped this, so a cached-chapter open with
  // Wi-Fi down queued idle prefetch TLS and panicked in lwIP getaddrinfo.
  const auto wifi = M4NativeWifi::ensureConnected(std::min<uint32_t>(r.timeoutMs, 20000u), cancelled);
  if (!wifi.ok) {
    NetResult out;
    out.error = wifi.error.empty() ? "wifi_not_connected" : wifi.error;
    return out;
  }
  M4HttpTransport::Request tr;
  tr.method = r.method;
  tr.url = r.url;
  tr.headerCount = std::min<size_t>(r.hCount, M4HttpTransport::kMaxHeaders);
  for (size_t i = 0; i < tr.headerCount; ++i) {
    tr.headers[i].name = r.hNames[i];
    tr.headers[i].value = r.hValues[i];
  }
  tr.body = r.body;
  tr.bodyLen = r.bodyLen;
  tr.maxBytes = r.maxBytes;
  tr.timeoutMs = r.timeoutMs;
  tr.followRedirects = false;
  tr.insecureTls = false;

  PodCtx ctx;
  ctx.progress = &progress;
  ctx.cancelled = &cancelled;
  ctx.stopWhenPsvts = stopWhenPsvts;
  if (M4HttpTransport::debugEnabled()) {
    const M4HttpTransport::MemSnap m = M4HttpTransport::memSnap();
    Serial.printf("[WRHTTP] enter sess=%d url=%.48s heap=%u int=%u larg=%u tls=%d\n",
                  useSession ? 1 : 0, r.url ? r.url : "", static_cast<unsigned>(m.freeHeap),
                  static_cast<unsigned>(m.freeInternal), static_cast<unsigned>(m.largestInternal),
                  m.tlsGateOk ? 1 : 0);
    Serial.flush();
  }
  const uint32_t t0 = millis();
  const M4HttpTransport::Result net =
      useSession ? M4HttpTransport::sessionRequestToSink(tr, sink, podProgress, &ctx, podCancel, &ctx)
                 : M4HttpTransport::requestToSink(tr, sink, podProgress, &ctx, podCancel, &ctx);
  if (M4HttpTransport::debugEnabled()) {
    const M4HttpTransport::MemSnap m = M4HttpTransport::memSnap();
    Serial.printf(
        "[WRHTTP] leave ok=%d st=%d bytes=%u err=%s ms=%u heap=%u int=%u larg=%u tls=%d\n",
        net.ok ? 1 : 0, net.status, static_cast<unsigned>(net.bytes),
        net.error[0] ? net.error : "-", static_cast<unsigned>(millis() - t0),
        static_cast<unsigned>(m.freeHeap), static_cast<unsigned>(m.freeInternal),
        static_cast<unsigned>(m.largestInternal), m.tlsGateOk ? 1 : 0);
    Serial.flush();
  }
  NetResult out;
  out.ok = net.ok;
  out.status = net.status;
  if (!net.ok && net.error[0]) out.error = net.error;
  // Early psvts cancel is success when the token was captured.
  if (!out.ok && stopWhenPsvts && stopWhenPsvts->found() &&
      (out.error == "cancelled" || out.error == "response_too_large" || net.bytes > 0)) {
    out.ok = true;
    out.error.clear();
  }
  return out;
#else
  (void)useSession;
  (void)stopWhenPsvts;
  M4NativeProviderHttp::Request r2;
  r2.method = r.method;
  r2.url = r.url;
  for (size_t i = 0; i < r.hCount; ++i) r2.headers.push_back({r.hNames[i], r.hValues[i]});
  r2.body.assign(r.body ? r.body : "", r.bodyLen);
  r2.maxBytes = r.maxBytes;
  r2.timeoutMs = r.timeoutMs;
  r2.followRedirects = false;
  r2.insecureTls = false;
  const M4NativeProviderHttp::Result net = M4NativeProviderHttp::requestToSink(
      r2, sink,
      [&progress](size_t n) {
        if (progress) progress(M4NativeProvider::Phase::Receiving, n, 0, 0);
      },
      [&cancelled, stopWhenPsvts]() {
        if (stopWhenPsvts && stopWhenPsvts->found()) return true;
        return cancelled && cancelled();
      });
  NetResult out;
  out.ok = net.ok;
  out.status = net.status;
  out.error = net.error;
  if (!out.ok && stopWhenPsvts && stopWhenPsvts->found()) {
    out.ok = true;
    out.error.clear();
  }
  return out;
#endif
}

#if M4_HTTP_TRANSPORT_AVAILABLE
// RAII so sessionEnd() also runs on every early-return path in fetchChapter.
class TransportSession {
 public:
  bool begin(const char* host) {
    active_ = M4HttpTransport::sessionBegin(host);
    return active_;
  }
  // psvts early-cancel aborts the body mid-stream; rebuild the client so
  // subsequent shard POSTs do not inherit a half-closed connection.
  bool restart(const char* host) {
    if (active_) {
      M4HttpTransport::sessionEnd();
      active_ = false;
    }
    return begin(host);
  }
  bool active() const { return active_; }
  ~TransportSession() {
    if (active_) M4HttpTransport::sessionEnd();
  }

 private:
  bool active_ = false;
};
#else
class TransportSession {
 public:
  bool begin(const char*) { return false; }
  bool restart(const char*) { return false; }
  bool active() const { return false; }
};
#endif

std::string readPrefix(const std::string& path, size_t cap = 512) {
  FsFile f;
  if (!SdMan.openFileForRead("WR-PFX", path.c_str(), f)) return {};
  const size_t n = std::min<size_t>(cap, static_cast<size_t>(f.fileSize()));
  std::string s;
  s.resize(n);
  const size_t got = n ? f.read(reinterpret_cast<uint8_t*>(&s[0]), n) : 0;
  f.close();
  if (got != n) return {};
  return s;
}

bool containsLoginTimeout(const std::string& prefix) {
  return prefix.find("-2012") != std::string::npos ||
         prefix.find("LOGIN_TIMEOUT") != std::string::npos;
}

bool fileSize(const std::string& path, size_t& n) {
  n = 0;
  FsFile f;
  if (!SdMan.openFileForRead("WR-SZ", path.c_str(), f)) return false;
  n = static_cast<size_t>(f.fileSize());
  f.close();
  return true;
}

bool loadPsvtsCache(const std::string& path, std::string& value) {
  value.clear();
  FsFile f;
  if (!SdMan.openFileForRead("WR-PS", path.c_str(), f)) return false;
  char buf[M4xPsvts::kMaxValueLen + 1] = {};
  int n = f.read(reinterpret_cast<uint8_t*>(buf), M4xPsvts::kMaxValueLen);
  f.close();
  if (n <= 0 || n > static_cast<int>(M4xPsvts::kMaxValueLen)) return false;
  while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n' || buf[n - 1] == ' ')) {
    buf[n - 1] = 0;
    --n;
  }
  value.assign(buf);
  return !value.empty();
}

void savePsvtsCache(const std::string& path, const std::string& value) {
  if (value.empty() || value.size() > M4xPsvts::kMaxValueLen) return;
  M4NativeProviderIo::ensureParentDirs(path);
  FsFile f;
  if (!SdMan.openFileForWrite("WR-PS", path.c_str(), f)) return;
  (void)f.write(reinterpret_cast<const uint8_t*>(value.data()), value.size());
  f.close();
}

void clearPsvtsCache(const std::string& path) {
  if (SdMan.exists(path.c_str())) SdMan.remove(path.c_str());
}

bool isHex32(const char* s) {
  for (int i = 0; i < 32; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (!std::isxdigit(c)) return false;
  }
  return true;
}

// WeRead content shards are: 32-byte ASCII MD5 hex of the payload + payload.
// Host probe matches papers3: md5(body).upper() == header (case-insensitive).
// FsFile::read returns int (−1 on error) — never store it in size_t raw.
bool appendCheckedShard(FsFile& combined, const std::string& shardPath, std::string& err) {
  FsFile in;
  if (!SdMan.openFileForRead("WR-SHARD", shardPath.c_str(), in)) {
    err = "shard_open";
    return false;
  }
  const size_t total = static_cast<size_t>(in.fileSize());
  if (total <= 32) {
    in.close();
    err = "shard_short";
    return false;
  }

  char expectedRaw[33] = {};
  if (in.read(reinterpret_cast<uint8_t*>(expectedRaw), 32) != 32) {
    in.close();
    err = "shard_header";
    return false;
  }
  // JSON/error bodies are not MD5-framed — surface a clearer error than shard_md5.
  if (expectedRaw[0] == '{') {
    in.close();
    err = containsLoginTimeout(std::string(expectedRaw, 32)) ? "login_required" : "shard_json";
    return false;
  }
  if (!isHex32(expectedRaw)) {
    in.close();
    err = "shard_bad_header";
    return false;
  }

  const size_t bodyLen = total - 32;
  // Prefer one PSRAM buffer for typical chapter shards (a few–tens of KB). Cap
  // the single-shot path; stream larger ones.
  constexpr size_t kOneShotMax = 256u * 1024u;
  if (bodyLen <= kOneShotMax) {
    uint8_t* body = static_cast<uint8_t*>(M4Psram::mallocPrefer(bodyLen));
    if (!body) {
      in.close();
      err = "shard_oom";
      return false;
    }
    size_t off = 0;
    while (off < bodyLen) {
      const int n = in.read(body + off, bodyLen - off);
      if (n <= 0) {
        M4Psram::freePrefer(body);
        in.close();
        err = "shard_io";
        return false;
      }
      off += static_cast<size_t>(n);
    }
    in.close();
    std::string got = weread_crypto::md5Hex(body, bodyLen);
    std::string exp(expectedRaw, 32);
    for (char& c : got) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (char& c : exp) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (got != exp) {
      M4Psram::freePrefer(body);
      err = "shard_md5";
      return false;
    }
    if (combined.write(body, bodyLen) != bodyLen) {
      M4Psram::freePrefer(body);
      err = "shard_io";
      return false;
    }
    M4Psram::freePrefer(body);
    return true;
  }

  // Large-shard streaming path (same MD5 as weread_crypto via mbedtls one-shot
  // chunks accumulated into a digest using the same hex helper on a temp window
  // is awkward — re-read into rolling mbedtls with correct int read).
  mbedtls_md5_context ctx;
  mbedtls_md5_init(&ctx);
  if (mbedtls_md5_starts(&ctx) != 0) {
    mbedtls_md5_free(&ctx);
    in.close();
    err = "md5_init";
    return false;
  }
  constexpr size_t kShardBuf = 4096;
  uint8_t* buf = static_cast<uint8_t*>(M4Psram::mallocPrefer(kShardBuf));
  if (!buf) {
    mbedtls_md5_free(&ctx);
    in.close();
    err = "shard_oom";
    return false;
  }
  size_t left = bodyLen;
  bool ok = true;
  while (left > 0) {
    const size_t want = std::min<size_t>(left, kShardBuf);
    const int n = in.read(buf, want);
    if (n <= 0) {
      ok = false;
      break;
    }
    const size_t got = static_cast<size_t>(n);
    if (mbedtls_md5_update(&ctx, buf, got) != 0 || combined.write(buf, got) != got) {
      ok = false;
      break;
    }
    left -= got;
  }
  M4Psram::freePrefer(buf);
  uint8_t md[16] = {};
  if (ok && mbedtls_md5_finish(&ctx, md) != 0) ok = false;
  mbedtls_md5_free(&ctx);
  in.close();
  if (!ok) {
    err = "shard_io";
    return false;
  }
  char gotHex[33];
  for (int i = 0; i < 16; ++i) std::snprintf(gotHex + i * 2, 3, "%02X", md[i]);
  gotHex[32] = 0;
  char expHex[33];
  for (int i = 0; i < 32; ++i) {
    expHex[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(expectedRaw[i])));
  }
  expHex[32] = 0;
  if (std::memcmp(gotHex, expHex, 32) != 0) {
    err = "shard_md5";
    return false;
  }
  return true;
}

std::vector<int> swapPositions(const uint8_t* tail, int tailN, int length) {
  std::vector<int> result;
  if (length < 4) return result;
  if (length < 11) return {0, 2};

  std::string tmp;
  tmp.reserve(static_cast<size_t>(tailN) * 5u);
  for (int i = tailN - 1; i >= 0; --i) {
    const uint8_t v = tail[i];
    uint32_t val = 0;
    for (int b = 0; b < 8; ++b) {
      if ((v >> b) & 1u) val += (1u << (2 * b));
    }
    tmp += std::to_string(val);
  }

  const int m = length - tailN - 2;
  if (m <= 0) return result;
  const int step = static_cast<int>(std::to_string(m).size());
  for (int i = 0; static_cast<int>(result.size()) < 10 &&
                  i + step < static_cast<int>(tmp.size()); i += step) {
    auto parse = [&](int at) {
      int v = 0;
      for (int k = 0; k < step && at + k < static_cast<int>(tmp.size()); ++k) {
        const char c = tmp[static_cast<size_t>(at + k)];
        if (c < '0' || c > '9') return 0;
        v = v * 10 + (c - '0');
      }
      return v % m;
    };
    result.push_back(parse(i));
    result.push_back(parse(i + 1));
  }
  return result;
}

bool readByteAt(FsFile& f, size_t off, uint8_t& b) {
  return f.seek(off) && f.read(&b, 1) == 1;
}

bool writeByteAt(FsFile& f, size_t off, uint8_t b) {
  return f.seek(off) && f.write(&b, 1) == 1;
}

bool reverseSwapsOnFile(const std::string& path, size_t payloadBytes, std::string& err) {
  if (payloadBytes < 2) {
    err = "payload_short";
    return false;
  }
  const int length = static_cast<int>(payloadBytes - 1);  // encoded payload starts after payload[0]
  const int n = std::min(4, (length + 9) / 10);
  FsFile f = SdMan.open(path.c_str(), O_RDWR);
  if (!f) {
    err = "swap_open";
    return false;
  }

  uint8_t tail[4] = {};
  for (int i = 0; i < n; ++i) {
    if (!readByteAt(f, 1u + static_cast<size_t>(length - n + i), tail[i])) {
      f.close();
      err = "swap_tail";
      return false;
    }
  }

  const auto pos = swapPositions(tail, n, length);
  for (int i = static_cast<int>(pos.size()) - 1; i > 0; i -= 2) {
    for (int k = 1; k >= 0; --k) {
      const int l = pos[static_cast<size_t>(i)] + k;
      const int r = pos[static_cast<size_t>(i - 1)] + k;
      if (l < 0 || r < 0 || l >= length || r >= length) continue;
      uint8_t a = 0;
      uint8_t b = 0;
      if (!readByteAt(f, 1u + static_cast<size_t>(l), a) ||
          !readByteAt(f, 1u + static_cast<size_t>(r), b) ||
          !writeByteAt(f, 1u + static_cast<size_t>(l), b) ||
          !writeByteAt(f, 1u + static_cast<size_t>(r), a)) {
        f.close();
        err = "swap_io";
        return false;
      }
    }
  }
  f.flush();
  f.close();
  return true;
}

class XhtmlStripSink final : public M4xJsonStream::Sink {
 public:
  explicit XhtmlStripSink(M4xJsonStream::Sink& out) : out_(out) {}

  bool write(const uint8_t* data, size_t len) override {
    for (size_t i = 0; i < len; ++i) {
      const uint8_t b = data[i];
      if (inTag_) {
        if (b == '>') {
          std::string low = tag_;
          std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
          });
          if (low.find("br") == 0 || low.find("/p") == 0 ||
              low.find("/div") == 0 || low.find("/li") == 0) {
            if (!emit('\n')) return false;
          }
          inTag_ = false;
          tag_.clear();
        } else if (b < 0x80 && tag_.size() < 32) {
          tag_.push_back(static_cast<char>(b));
        }
        continue;
      }

      if (inEntity_) {
        if (b == ';') {
          if (entity_ == "nbsp" || entity_ == "#160") {
            if (!emit(' ')) return false;
          } else if (entity_ == "amp") {
            if (!emit('&')) return false;
          } else if (entity_ == "lt") {
            if (!emit('<')) return false;
          } else if (entity_ == "gt") {
            if (!emit('>')) return false;
          } else {
            if (!emit(' ')) return false;
          }
          inEntity_ = false;
          entity_.clear();
          continue;
        }
        if (b < 0x80 && entity_.size() < 14) {
          entity_.push_back(static_cast<char>(b));
          continue;
        }
        if (!emit('&') ||
            !out_.write(reinterpret_cast<const uint8_t*>(entity_.data()), entity_.size())) return false;
        inEntity_ = false;
        entity_.clear();
      }

      if (b == '<') {
        inTag_ = true;
        tag_.clear();
      } else if (b == '&') {
        inEntity_ = true;
        entity_.clear();
      } else if (b != '\r') {
        if (!out_.write(&b, 1)) return false;
      }
    }
    return true;
  }

 private:
  bool emit(char c) {
    const uint8_t b = static_cast<uint8_t>(c);
    return out_.write(&b, 1);
  }

  M4xJsonStream::Sink& out_;
  bool inTag_ = false;
  bool inEntity_ = false;
  std::string tag_;
  std::string entity_;
};

int b64Value(uint8_t c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-' || c == '+') return 62;
  if (c == '_' || c == '/') return 63;
  return -1;
}

bool decodeBase64File(const std::string& combinedPath, bool stripXhtml,
                      M4NativeProviderIo::PartFileSink& finalSink,
                      const M4NativeProvider::ProgressFn& progress,
                      const M4NativeProvider::CancelFn& cancelled,
                      std::string& err) {
  M4NativeProviderHeavyGate::diagnosticStage() = 0x250;
  size_t total = 0;
  if (!fileSize(combinedPath, total) || total <= 1) {
    err = "payload_empty";
    return false;
  }
  if (!reverseSwapsOnFile(combinedPath, total, err)) return false;

  FsFile f;
  if (!SdMan.openFileForRead("WR-B64", combinedPath.c_str(), f) || !f.seek(1)) {
    err = "payload_open";
    if (f.isOpen()) f.close();
    return false;
  }

  // Streaming windows in PSRAM — never on the worker stack / internal heap.
  constexpr size_t kInBytes = 4096;
  constexpr size_t kOutBytes = 3072;
  uint8_t* input = static_cast<uint8_t*>(M4Psram::mallocPrefer(kInBytes));
  uint8_t* output = static_cast<uint8_t*>(M4Psram::mallocPrefer(kOutBytes + 8));
  if (!input || !output) {
    M4Psram::freePrefer(input);
    M4Psram::freePrefer(output);
    f.close();
    err = "decode_oom";
    return false;
  }

  XhtmlStripSink stripped(finalSink);
  M4xJsonStream::Sink& target = stripXhtml ? static_cast<M4xJsonStream::Sink&>(stripped)
                                           : static_cast<M4xJsonStream::Sink&>(finalSink);
  size_t outputLen = 0;
  uint32_t acc = 0;
  int bits = 0;
  size_t done = 1;
  bool sawPadding = false;
  int lastPct = -1;
  uint32_t lastProgressMs = 0;

  auto freeWindows = [&]() {
    M4Psram::freePrefer(input);
    M4Psram::freePrefer(output);
    input = nullptr;
    output = nullptr;
  };

  while (done < total && !sawPadding) {
    if (cancelled && cancelled()) {
      freeWindows();
      f.close();
      err = "cancelled";
      return false;
    }
    const size_t want = std::min<size_t>(kInBytes, total - done);
    const size_t got = f.read(input, want);
    if (got == 0) break;

    for (size_t i = 0; i < got; ++i) {
      const uint8_t c = input[i];
      if (c == '=') {
        sawPadding = true;
        break;
      }
      const int d = b64Value(c);
      if (d < 0) continue;  // tolerate transport whitespace

      // Keep only the live < 8 residual bits after each output byte. This
      // avoids the signed/unbounded shift overflow in the original decoder on
      // long chapters while remaining fully streaming.
      acc = (acc << 6) | static_cast<uint32_t>(d);
      bits += 6;
      while (bits >= 8) {
        bits -= 8;
        output[outputLen++] = static_cast<uint8_t>((acc >> bits) & 0xFFu);
        if (bits == 0) acc = 0;
        else acc &= (1u << bits) - 1u;
        if (outputLen >= kOutBytes) {
          if (!target.write(output, outputLen)) {
            freeWindows();
            f.close();
            err = "decode_write";
            return false;
          }
          outputLen = 0;
        }
      }
    }

    done += got;
    if (progress) {
      const int pct = static_cast<int>((std::min(done, total) * 90u) / total);
      const uint32_t now = millis();
      // Throttle UI/status churn: each progress tick copies several std::string
      // fields on the internal heap while decode is still running.
      if (pct != lastPct && (pct >= lastPct + 5 || now - lastProgressMs >= 400u || pct >= 90)) {
        lastPct = pct;
        lastProgressMs = now;
        progress(M4NativeProvider::Phase::Decoding, total, finalSink.written(), pct);
      }
    }
  }
  f.close();

  if (outputLen && !target.write(output, outputLen)) {
    freeWindows();
    err = "decode_write";
    return false;
  }
  freeWindows();
  return finalSink.written() > 0;
}

bool combineShards(const std::vector<std::string>& shards, const std::string& combinedPath,
                   size_t& payloadBytes, std::string& err) {
  payloadBytes = 0;
  if (SdMan.exists(combinedPath.c_str())) SdMan.remove(combinedPath.c_str());
  M4NativeProviderIo::ensureParentDirs(combinedPath);
  FsFile out;
  if (!SdMan.openFileForWrite("WR-COMB", combinedPath.c_str(), out)) {
    err = "combine_open";
    return false;
  }
  for (const auto& p : shards) {
    if (p.empty()) continue;
    if (!appendCheckedShard(out, p, err)) {
      out.close();
      SdMan.remove(combinedPath.c_str());
      return false;
    }
  }
  out.flush();
  payloadBytes = static_cast<size_t>(out.fileSize());
  out.close();
  if (payloadBytes <= 1) {
    SdMan.remove(combinedPath.c_str());
    err = "payload_empty";
    return false;
  }
  return true;
}

class WereadProvider final : public M4NativeProvider::Adapter {
 public:
  const char* id() const override { return "weread"; }

  M4NativeProvider::FetchResult fetchChapter(const M4NativeProvider::ChapterRequest& req,
                                             const M4NativeProvider::ProgressFn& progress,
                                             const M4NativeProvider::CancelFn& cancelled) override {
    if (!M4NativeProviderHeavyGate::heapHealthy(0x200)) {
      M4NativeProvider::FetchResult bad;
      bad.error = "heap_corrupt";
      return bad;
    }
    M4NativeProvider::FetchResult out;
    size_t cached = 0;
    if (M4NativeProviderIo::cacheComplete(req.cacheAbsPath, &cached)) {
      out.ok = true;
      out.bytes = cached;
      out.cacheRelPath = req.cacheRelPath;
      if (progress) progress(M4NativeProvider::Phase::Ready, 0, cached, 100);
      return out;
    }

    std::string cookie;
    if (!M4NativeProviderIo::loadCookieHeader(req.appDataRoot, "weread", cookie)) {
      out.authRequired = true;
      out.error = "login_required";
      return out;
    }
    if (!M4NativeProviderHeavyGate::heapHealthy(0x210)) {
      out.error = "heap_corrupt";
      return out;
    }

    const std::string readerUrl = std::string("https://weread.qq.com/web/reader/") +
                                  weread_crypto::e(req.book.bookId) + "k" +
                                  weread_crypto::e(req.chapter.uid);
    std::string psvts;
    const std::string psvtsPath = req.appDataRoot + "/cache/" + req.book.bookId + "/psvts.txt";
    bool usedCachedPsvts = loadPsvtsCache(psvtsPath, psvts);
    if (!M4NativeProviderHeavyGate::heapHealthy(0x220)) {
      out.error = "heap_corrupt";
      return out;
    }
    // One transport session covers the whole chapter (psvts + all shards);
    // the RAII guard ends it on every exit path. When the session cannot be
    // established the transport falls back to per-request handles.
#if M4_HTTP_TRANSPORT_AVAILABLE
    // Drop any leaked session from a previous chapter before gate-check so
    // chapter N+1 does not inherit a half-dead client + fragmented peak.
    M4HttpTransport::shutdown();
    if (!M4NativeProviderHeavyGate::tlsBlockAvailable()) {
      // Brief yield: reader/UI may still be freeing the previous chapter.
      for (int i = 0; i < 8 && !M4NativeProviderHeavyGate::tlsBlockAvailable(); ++i) {
        delay(25);
      }
    }
    if (!M4NativeProviderHeavyGate::tlsBlockAvailable()) {
      const auto m = M4HttpTransport::memSnap();
      Serial.printf("[WRHTTP] tls_gate_block ch=%s heap=%u int=%u larg=%u\n",
                    req.chapter.uid.c_str(), static_cast<unsigned>(m.freeHeap),
                    static_cast<unsigned>(m.freeInternal),
                    static_cast<unsigned>(m.largestInternal));
      Serial.flush();
      out.error = "tls_internal_oom";
      return out;
    }
#endif
    TransportSession session;
#if M4_HTTP_TRANSPORT_AVAILABLE
    M4HttpTransport::debugStep("chapter_begin", req.chapter.uid.c_str());
#endif
    session.begin("weread.qq.com");
    if (!usedCachedPsvts) {
      if (wrTraceEnabled()) {
        Serial.printf("[WRHTTP] step=psvts sess=%d ch=%s\n", session.active() ? 1 : 0,
                      req.chapter.uid.c_str());
        Serial.flush();
      }
      if (!fetchPsvts(readerUrl, cookie, psvts, cancelled, out.error, session.active())) {
        if (out.error == "login_required") out.authRequired = true;
        if (wrTraceEnabled()) {
          Serial.printf("[WRHTTP] psvts_fail err=%s\n", out.error.c_str());
          Serial.flush();
        }
        return out;
      }
      savePsvtsCache(psvtsPath, psvts);
      // psvts path cancels the large HTML body early — rebuild before POSTs.
      if (session.active()) session.restart("weread.qq.com");
    } else {
      if (wrTraceEnabled()) {
        Serial.printf("[WRHTTP] step=psvts_cache ch=%s\n", req.chapter.uid.c_str());
        Serial.flush();
      }
    }

    const std::string base = req.cacheAbsPath + ".wr";
    const std::string e0 = base + ".e0";
    const std::string a = base + ".a";
    const std::string b = base + ".b";
    const std::string combined = base + ".combined";
    auto cleanup = [&]() {
      const char* paths[] = {e0.c_str(), a.c_str(), b.c_str(), combined.c_str()};
      for (const char* p : paths) {
        if (SdMan.exists(p)) SdMan.remove(p);
      }
    };
    cleanup();

    if (progress) progress(M4NativeProvider::Phase::Connecting, 0, 0, 0);
    auto fetchE0 = [&]() {
      if (wrTraceEnabled()) {
        Serial.printf("[WRHTTP] step=e_0 sess=%d\n", session.active() ? 1 : 0);
        Serial.flush();
      }
      return downloadShard("/web/book/chapter/e_0", readerUrl, req, psvts, cookie, e0,
                           progress, cancelled, out.error, session.active(), true);
    };
    if (!fetchE0()) {
      if (out.error == "login_required") out.authRequired = true;
      cleanup();
      return out;
    }

    std::string pfx = readPrefix(e0);
    const auto invalidE0 = [&]() {
      return pfx.empty() || pfx == "{}" ||
             (pfx[0] == '{' && pfx.find("\"bookId\"") == std::string::npos);
    };
    // A persisted token makes the common path skip the ~0.9MB reader page.
    // If it expired, refresh once here and keep the failure local to WeRead.
    if (invalidE0()) {
      // reader HTML is public enough to yield psvts even after the account
      // cookie expires. Distinguish that case from a content/protocol error so
      // the UI opens login instead of reporting a misleading empty chapter.
      if (authExpired(cookie, cancelled, session.active())) {
        out.authRequired = true;
        out.error = "login_required";
        cleanup();
        return out;
      }
      usedCachedPsvts = false;
      clearPsvtsCache(psvtsPath);
      cleanup();
      out.error.clear();
      if (session.active()) session.restart("weread.qq.com");
      if (!fetchPsvts(readerUrl, cookie, psvts, cancelled, out.error, session.active())) {
        if (out.error == "login_required") out.authRequired = true;
        return out;
      }
      savePsvtsCache(psvtsPath, psvts);
      if (session.active()) session.restart("weread.qq.com");
      if (!fetchE0()) {
        if (out.error == "login_required") out.authRequired = true;
        cleanup();
        return out;
      }
      pfx = readPrefix(e0);
    }
    if (wrTraceEnabled()) {
      Serial.printf("[WRHTTP] step=e0_pfx len=%u head=%.40s\n", static_cast<unsigned>(pfx.size()),
                    pfx.c_str());
      Serial.flush();
    }
    if (containsLoginTimeout(pfx)) {
      out.authRequired = true;
      out.error = "login_required";
      cleanup();
      return out;
    }
    if (pfx.size() >= 2 && pfx[0] == 'P' && pfx[1] == 'K') {
      out.error = "epub_zip_unsupported";
      cleanup();
      return out;
    }
    if (pfx == "{}" || pfx.empty()) {
      out.error = "empty_content";
      if (wrTraceEnabled()) {
        Serial.printf("[WRHTTP] empty_content\n");
        Serial.flush();
      }
      cleanup();
      return out;
    }

    const bool textMode = pfx[0] == '{' && pfx.find("\"bookId\"") != std::string::npos;
    std::vector<std::string> shards;
    if (textMode) {
      if (wrTraceEnabled()) {
        Serial.printf("[WRHTTP] step=t_0/t_1 text_mode=1\n");
        Serial.flush();
      }
      if (!downloadShard("/web/book/chapter/t_0", readerUrl, req, psvts, cookie, a,
                         progress, cancelled, out.error, session.active()) ||
          !downloadShard("/web/book/chapter/t_1", readerUrl, req, psvts, cookie, b,
                         progress, cancelled, out.error, session.active())) {
        cleanup();
        return out;
      }
      shards = {a, b};
      SdMan.remove(e0.c_str());  // e0 is routing metadata in text mode
    } else {
      if (wrTraceEnabled()) {
        Serial.printf("[WRHTTP] step=e_1/e_3 text_mode=0\n");
        Serial.flush();
      }
      if (!downloadShard("/web/book/chapter/e_1", readerUrl, req, psvts, cookie, a,
                         progress, cancelled, out.error, session.active()) ||
          !downloadShard("/web/book/chapter/e_3", readerUrl, req, psvts, cookie, b,
                         progress, cancelled, out.error, session.active())) {
        cleanup();
        return out;
      }
      shards = {e0, a, b};
    }

    size_t payloadBytes = 0;
    if (progress) progress(M4NativeProvider::Phase::Decoding, 0, 0, 0);
    if (!combineShards(shards, combined, payloadBytes, out.error)) {
      cleanup();
      return out;
    }

    M4NativeProviderIo::PartFileSink finalSink;
    if (!finalSink.open(req.cacheAbsPath)) {
      out.error = "sd_open_failed";
      cleanup();
      return out;
    }
    if (!decodeBase64File(combined, !textMode, finalSink, progress, cancelled, out.error) ||
        !finalSink.flush() || finalSink.written() == 0) {
      finalSink.close();
      M4NativeProviderIo::removeIncomplete(req.cacheAbsPath);
      cleanup();
      if (out.error.empty()) out.error = "decode_failed";
      return out;
    }
    finalSink.close();

    size_t finalBytes = 0;
    if (!M4NativeProviderIo::commitPart(req.cacheAbsPath, &finalBytes)) {
      out.error = "cache_commit_failed";
      cleanup();
      return out;
    }
    cleanup();
    out.ok = true;
    out.bytes = finalBytes;
    out.cacheRelPath = req.cacheRelPath;
    if (wrTraceEnabled()) {
      Serial.printf("[WRHTTP] chapter_ok bytes=%u\n", static_cast<unsigned>(finalBytes));
      Serial.flush();
    }
    if (progress) progress(M4NativeProvider::Phase::Ready, payloadBytes, finalBytes, 100);
    return out;
  }

 private:
  bool authExpired(const std::string& cookie,
                   const M4NativeProvider::CancelFn& cancelled,
                   bool useSession) {
    PrefixSink sink;
    NetReq r;
    r.url = "https://weread.qq.com/web/shelf/sync";
    r.hNames[0] = "Cookie";
    r.hValues[0] = cookie.c_str();
    r.hNames[1] = "Referer";
    r.hValues[1] = "https://weread.qq.com/";
    r.hCount = 2;
    // Expired-login JSON is tiny. A valid shelf may exceed this cap; its
    // prefix is enough to prove it is not the -2012 response.
    r.maxBytes = 2048;
    r.timeoutMs = 10000;
    const NetResult net = netRequest(r, sink, {}, cancelled, useSession);
    if (net.status == 401 || net.status == 403) return true;
    return containsLoginTimeout(sink.value());
  }

  bool fetchPsvts(const std::string& readerUrl, const std::string& cookie, std::string& psvts,
                  const M4NativeProvider::CancelFn& cancelled, std::string& err,
                  bool useSession) {
    PsvtsSink sink;
    NetReq r;
    r.url = readerUrl.c_str();
    r.hNames[0] = "Cookie";
    r.hValues[0] = cookie.c_str();
    r.hNames[1] = "Referer";
    r.hValues[1] = "https://weread.qq.com/";
    r.hCount = 2;
    // Keep the proven scan ceiling: some reader pages place psvts near 0.9MB.
    // The cancel callback still stops immediately when the token is found.
    r.maxBytes = M4xPsvts::kMaxScanBytes;
    r.timeoutMs = 30000;
    const NetResult net = netRequest(r, sink, {}, cancelled, useSession, &sink);
    if (sink.found()) {
      psvts = sink.value();
      if (wrTraceEnabled()) {
        Serial.printf("[WRHTTP] psvts_ok len=%u\n", static_cast<unsigned>(psvts.size()));
        Serial.flush();
      }
      return true;
    }
    if (!net.ok) {
      err = (net.status == 401 || net.status == 403) ? "login_required" : net.error;
      return false;
    }
    err = "psvts_not_found";
    return false;
  }

  bool downloadShard(const std::string& endpoint, const std::string& readerUrl,
                     const M4NativeProvider::ChapterRequest& req, const std::string& psvts,
                     const std::string& cookie, const std::string& outPath,
                     const M4NativeProvider::ProgressFn& progress,
                     const M4NativeProvider::CancelFn& cancelled, std::string& err,
                     bool useSession, bool allowJson = false) {
    DirectFileSink sink;
    if (!sink.open(outPath)) {
      err = "sd_open_failed";
      return false;
    }

    const std::string url = std::string("https://weread.qq.com") + endpoint;
    const long now = static_cast<long>(time(nullptr));
    const long rnd = static_cast<long>(esp_random() % 10000u);
    const std::string contentBody = weread_crypto::makeContentParamsJson(
        req.book.bookId, req.chapter.uid, psvts, false, 1, now, rnd);
    NetReq r;
    r.method = "POST";
    r.url = url.c_str();
    r.hNames[0] = "Cookie";
    r.hValues[0] = cookie.c_str();
    r.hNames[1] = "Referer";
    r.hValues[1] = readerUrl.c_str();
    r.hNames[2] = "Content-Type";
    r.hValues[2] = "application/json";
    r.hCount = 3;
    r.body = contentBody.data();
    r.bodyLen = contentBody.size();
    r.maxBytes = 2u * 1024u * 1024u;
    r.timeoutMs = 30000;
    const NetResult net = netRequest(r, sink, progress, cancelled, useSession);
    const size_t shardBytes = sink.bytes();
    sink.close();
    if (!net.ok || shardBytes == 0) {
      err = (net.status == 401 || net.status == 403) ? "login_required" : net.error;
      if (err.empty()) err = "shard_download";
      return false;
    }

    const std::string prefix = readPrefix(outPath, 256);
    if (containsLoginTimeout(prefix)) {
      err = "login_required";
      return false;
    }
    if (!allowJson && !prefix.empty() && prefix[0] == '{') {
      err = "shard_json";
      return false;
    }
    if (prefix.size() >= 32 && !isHex32(prefix.c_str())) {
      err = "shard_bad_header";
      return false;
    }
    return true;
  }
};

}  // namespace

std::unique_ptr<M4NativeProvider::Adapter> createWereadProvider() {
  return std::make_unique<WereadProvider>();
}

}  // namespace M4NativeProviderAdapters
