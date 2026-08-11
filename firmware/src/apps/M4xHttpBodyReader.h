#pragma once

// Production HTTP response body reader for M4x net.request (host-testable).
// Content-Length: read exactly N bytes then finish (keep-alive safe).
// Chunked: decode chunks with size limits.
// UntilClose: stop on disconnect; idle + total deadlines.

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace M4xHttp {

struct Stream {
  virtual int available() = 0;
  virtual int read(uint8_t* buf, size_t n) = 0;
  virtual bool connected() = 0;
  virtual ~Stream() = default;
};

enum class BodyKind { ContentLength, Chunked, UntilClose };

struct Limits {
  size_t maxBody = 512 * 1024;
  uint32_t totalTimeoutMs = 20000;
  uint32_t idleTimeoutMs = 5000;
  uint32_t (*nowMs)() = nullptr;
  void (*onWait)() = nullptr;
  // Cooperative cancel (optional).
  bool (*isCancelled)() = nullptr;
};

struct Buffer {
  char* data = nullptr;
  size_t len = 0;
  size_t cap = 0;
  // Opaque owner (e.g. NetBodyBuf*); grow must preserve self->len.
  void* growCtx = nullptr;
  bool (*grow)(Buffer* self, size_t need) = nullptr;

  bool append(const char* p, size_t n, size_t maxTotal) {
    if (len + n > maxTotal) return false;
    if (len + n > cap) {
      if (!grow || !grow(this, len + n)) return false;
    }
    if (n && data) std::memcpy(data + len, p, n);
    len += n;
    return true;
  }
};

struct Result {
  bool ok = false;
  std::string error;
  size_t length = 0;
};

inline uint32_t nowOr0(const Limits& lim) { return lim.nowMs ? lim.nowMs() : 0; }

inline bool cancelled(const Limits& lim) {
  return lim.isCancelled && lim.isCancelled();
}

inline bool timedOut(const Limits& lim, uint32_t deadline) {
  if (!lim.nowMs || deadline == 0) return false;
  return static_cast<int32_t>(deadline - nowOr0(lim)) <= 0;
}

inline bool idleTimedOut(const Limits& lim, uint32_t lastDataMs) {
  if (!lim.nowMs || lim.idleTimeoutMs == 0 || lastDataMs == 0) return false;
  return static_cast<int32_t>(nowOr0(lim) - lastDataMs) > static_cast<int32_t>(lim.idleTimeoutMs);
}

inline bool readExact(Stream& s, uint8_t* dest, size_t n, const Limits& lim, uint32_t deadline,
                      uint32_t& lastDataMs, std::string& err) {
  size_t off = 0;
  while (off < n) {
    if (cancelled(lim)) {
      err = "cancelled";
      return false;
    }
    if (timedOut(lim, deadline)) {
      err = "timeout";
      return false;
    }
    if (idleTimedOut(lim, lastDataMs)) {
      err = "idle_timeout";
      return false;
    }
    const int avail = s.available();
    if (avail <= 0) {
      if (!s.connected()) {
        err = "connection_closed";
        return false;
      }
      if (lim.onWait) lim.onWait();
      continue;
    }
    const int r = s.read(dest + off, n - off);
    if (r < 0) {
      err = "read_error";
      return false;
    }
    if (r == 0) {
      if (lim.onWait) lim.onWait();
      continue;
    }
    off += static_cast<size_t>(r);
    lastDataMs = nowOr0(lim);
  }
  return true;
}

inline bool readLine(Stream& s, std::string& line, const Limits& lim, uint32_t deadline, uint32_t& lastDataMs,
                     std::string& err, size_t maxLine = 64) {
  line.clear();
  while (line.size() < maxLine) {
    uint8_t c = 0;
    if (!readExact(s, &c, 1, lim, deadline, lastDataMs, err)) return false;
    if (c == '\n') {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      return true;
    }
    line.push_back(static_cast<char>(c));
  }
  err = "line_too_long";
  return false;
}

inline bool parseHexSize(const std::string& s, size_t& out) {
  out = 0;
  if (s.empty()) return false;
  size_t i = 0;
  while (i < s.size() && s[i] != ';') {
    char c = s[i++];
    size_t v;
    if (c >= '0' && c <= '9')
      v = static_cast<size_t>(c - '0');
    else if (c >= 'a' && c <= 'f')
      v = static_cast<size_t>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      v = static_cast<size_t>(c - 'A' + 10);
    else
      return false;
    if (out > (SIZE_MAX - v) / 16) return false;
    out = out * 16 + v;
  }
  return true;
}

inline Result readBody(Stream& s, BodyKind kind, size_t contentLength, Buffer& out, const Limits& lim) {
  Result r;
  const uint32_t start = nowOr0(lim);
  const uint32_t deadline = lim.nowMs ? (start + lim.totalTimeoutMs) : 0;
  uint32_t lastDataMs = start;
  std::string err;

  if (kind == BodyKind::ContentLength) {
    if (contentLength > lim.maxBody) {
      r.error = "response_too_large";
      return r;
    }
    if (contentLength == 0) {
      r.ok = true;
      return r;
    }
    if (out.cap < contentLength) {
      if (!out.grow || !out.grow(&out, contentLength)) {
        r.error = "oom";
        return r;
      }
    }
    if (!readExact(s, reinterpret_cast<uint8_t*>(out.data), contentLength, lim, deadline, lastDataMs, err)) {
      r.error = err.empty() ? "read_failed" : err;
      return r;
    }
    out.len = contentLength;
    r.ok = true;
    r.length = contentLength;
    return r;
  }

  if (kind == BodyKind::Chunked) {
    while (true) {
      std::string sizeLine;
      if (!readLine(s, sizeLine, lim, deadline, lastDataMs, err)) {
        r.error = err.empty() ? "chunk_size_line" : err;
        return r;
      }
      size_t chunkSize = 0;
      if (!parseHexSize(sizeLine, chunkSize)) {
        r.error = "chunk_size_parse";
        return r;
      }
      if (chunkSize == 0) {
        while (true) {
          std::string trailer;
          if (!readLine(s, trailer, lim, deadline, lastDataMs, err)) {
            r.error = err.empty() ? "chunk_trailer" : err;
            return r;
          }
          if (trailer.empty()) break;
        }
        r.ok = true;
        r.length = out.len;
        return r;
      }
      if (out.len + chunkSize > lim.maxBody) {
        r.error = "response_too_large";
        return r;
      }
      if (out.cap < out.len + chunkSize) {
        if (!out.grow || !out.grow(&out, out.len + chunkSize)) {
          r.error = "oom";
          return r;
        }
      }
      if (!readExact(s, reinterpret_cast<uint8_t*>(out.data + out.len), chunkSize, lim, deadline, lastDataMs, err)) {
        r.error = err.empty() ? "chunk_data" : err;
        return r;
      }
      out.len += chunkSize;
      uint8_t crlf[2];
      if (!readExact(s, crlf, 2, lim, deadline, lastDataMs, err)) {
        r.error = err.empty() ? "chunk_crlf" : err;
        return r;
      }
      if (crlf[0] != '\r' || crlf[1] != '\n') {
        r.error = "chunk_crlf_bad";
        return r;
      }
    }
  }

  // UntilClose
  uint8_t tmp[512];
  while (true) {
    if (cancelled(lim)) {
      r.error = "cancelled";
      return r;
    }
    if (timedOut(lim, deadline)) {
      r.error = "timeout";
      return r;
    }
    if (idleTimedOut(lim, lastDataMs)) {
      if (!s.connected() && s.available() <= 0) {
        r.ok = true;
        r.length = out.len;
        return r;
      }
      r.error = "idle_timeout";
      return r;
    }
    const int avail = s.available();
    if (avail <= 0) {
      if (!s.connected()) {
        r.ok = true;
        r.length = out.len;
        return r;
      }
      if (lim.onWait) lim.onWait();
      continue;
    }
    const int rdb = s.read(tmp, sizeof(tmp));
    if (rdb < 0) {
      r.ok = true;
      r.length = out.len;
      return r;
    }
    if (rdb == 0) {
      if (lim.onWait) lim.onWait();
      continue;
    }
    lastDataMs = nowOr0(lim);
    if (!out.append(reinterpret_cast<const char*>(tmp), static_cast<size_t>(rdb), lim.maxBody)) {
      r.error = "response_too_large";
      return r;
    }
  }
}

// scheme://host:port (default ports 443/80)
inline std::string effectiveOrigin(const std::string& url) {
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) return {};
  std::string scheme = url.substr(0, schemeEnd);
  for (char& c : scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  size_t hostStart = schemeEnd + 3;
  const size_t path = url.find('/', hostStart);
  const size_t limit = path == std::string::npos ? url.size() : path;
  const size_t at = url.find('@', hostStart);
  if (at != std::string::npos && at < limit) hostStart = at + 1;
  size_t hostEnd = hostStart;
  while (hostEnd < limit && url[hostEnd] != ':' && url[hostEnd] != '/' && url[hostEnd] != '?' &&
         url[hostEnd] != '#')
    ++hostEnd;
  std::string host = url.substr(hostStart, hostEnd - hostStart);
  for (char& c : host) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  int port = -1;
  if (hostEnd < limit && url[hostEnd] == ':') {
    size_t p = hostEnd + 1;
    port = 0;
    while (p < limit && url[p] >= '0' && url[p] <= '9') {
      port = port * 10 + (url[p] - '0');
      ++p;
    }
  }
  if (port < 0) port = (scheme == "https") ? 443 : 80;
  return scheme + "://" + host + ":" + std::to_string(port);
}

inline bool sameOrigin(const std::string& a, const std::string& b) {
  const std::string oa = effectiveOrigin(a);
  const std::string ob = effectiveOrigin(b);
  return !oa.empty() && oa == ob;
}

}  // namespace M4xHttp
