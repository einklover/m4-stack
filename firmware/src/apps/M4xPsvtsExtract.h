#pragma once

// Bounded streaming extraction of WeRead `"psvts":"..."` from large HTTPS bodies.
// Host-testable: no full-page buffer; only the short value is retained.

#include "apps/M4xHttpBodyReader.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace M4xPsvts {

// Scan enough for observed ~900 KB reader pages with headroom; never allocate body.
inline constexpr size_t kMaxScanBytes = 2 * 1024 * 1024;
// Real psvts values are short hex-ish tokens; keep a hard upper bound.
inline constexpr size_t kMaxValueLen = 512;
inline constexpr size_t kReadChunk = 512;

inline constexpr char kMarker[] = "\"psvts\"";
inline constexpr size_t kMarkerLen = 7;  // strlen("\"psvts\"")

struct Scanner {
  enum class Phase : uint8_t { Marker, Colon, OpenQuote, Value, Done };

  Phase phase = Phase::Marker;
  size_t markerMatch = 0;
  size_t maxValueLen = kMaxValueLen;
  std::string value;
  bool found = false;
  bool valueTooLarge = false;

  void reset(size_t maxVal = kMaxValueLen) {
    phase = Phase::Marker;
    markerMatch = 0;
    maxValueLen = maxVal;
    value.clear();
    found = false;
    valueTooLarge = false;
  }

  // Feed one byte. Returns true when terminal (found or value too large).
  bool feed(char c) {
    if (phase == Phase::Done) return true;
    switch (phase) {
      case Phase::Marker: {
        const char expect = kMarker[markerMatch];
        if (c == expect) {
          ++markerMatch;
          if (markerMatch >= kMarkerLen) {
            phase = Phase::Colon;
            markerMatch = 0;
          }
        } else if (c == kMarker[0]) {
          markerMatch = 1;
        } else {
          markerMatch = 0;
        }
        break;
      }
      case Phase::Colon:
        // Match production weread_crypto::extractPsvts: first ':' after marker.
        if (c == ':') phase = Phase::OpenQuote;
        break;
      case Phase::OpenQuote:
        // First '"' after colon opens the value (whitespace/other chars skipped).
        if (c == '"') {
          phase = Phase::Value;
          value.clear();
        }
        break;
      case Phase::Value:
        if (c == '"') {
          found = true;
          phase = Phase::Done;
          return true;
        }
        if (value.size() >= maxValueLen) {
          valueTooLarge = true;
          phase = Phase::Done;
          return true;
        }
        value.push_back(c);
        break;
      case Phase::Done:
        return true;
    }
    return false;
  }

  // Feed a buffer. Returns true when terminal.
  bool feed(const char* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      if (feed(p[i])) return true;
    }
    return false;
  }

  bool feed(const uint8_t* p, size_t n) { return feed(reinterpret_cast<const char*>(p), n); }
};

struct Result {
  bool ok = false;     // true only when value extracted
  bool found = false;
  std::string value;
  std::string error;
  size_t scanned = 0;
};

// Scan HTTP body stream for psvts without retaining the page.
// maxScan: total decoded body bytes examined (default 2 MiB).
// maxValue: max extracted value length.
inline Result scanBody(M4xHttp::Stream& s, M4xHttp::BodyKind kind, size_t contentLength,
                       const M4xHttp::Limits& lim, size_t maxScan = kMaxScanBytes,
                       size_t maxValue = kMaxValueLen) {
  Result r;
  Scanner sc;
  sc.reset(maxValue);

  const uint32_t start = M4xHttp::nowOr0(lim);
  const uint32_t deadline = lim.nowMs ? (start + lim.totalTimeoutMs) : 0;
  uint32_t lastDataMs = start;
  std::string err;

  auto finishFound = [&]() {
    r.ok = true;
    r.found = true;
    r.value = sc.value;
    r.error.clear();
  };

  auto fail = [&](const char* e) {
    r.ok = false;
    r.found = false;
    r.value.clear();
    r.error = e ? e : "scan_failed";
  };

  auto feedBytes = [&](const uint8_t* p, size_t n) -> bool {
    // Cap total scanned bytes.
    if (r.scanned >= maxScan) {
      fail("scan_too_large");
      return false;
    }
    size_t take = n;
    if (r.scanned + take > maxScan) take = maxScan - r.scanned;
    if (sc.feed(p, take)) {
      r.scanned += take;
      if (sc.found) {
        finishFound();
        return false;  // stop scanning (success)
      }
      if (sc.valueTooLarge) {
        fail("psvts_value_too_large");
        return false;
      }
    } else {
      r.scanned += take;
      if (r.scanned >= maxScan && !sc.found) {
        fail("scan_too_large");
        return false;
      }
    }
    return true;  // keep going
  };

  if (kind == M4xHttp::BodyKind::ContentLength) {
    // Known oversized Content-Length is still scannable up to maxScan.
    size_t remaining = contentLength;
    uint8_t tmp[kReadChunk];
    while (remaining > 0) {
      if (M4xHttp::cancelled(lim)) {
        fail("cancelled");
        return r;
      }
      if (M4xHttp::timedOut(lim, deadline)) {
        fail("timeout");
        return r;
      }
      if (M4xHttp::idleTimedOut(lim, lastDataMs)) {
        fail("idle_timeout");
        return r;
      }
      const size_t want = remaining < sizeof(tmp) ? remaining : sizeof(tmp);
      if (!M4xHttp::readExact(s, tmp, want, lim, deadline, lastDataMs, err)) {
        fail(err.empty() ? "read_failed" : err.c_str());
        return r;
      }
      if (!feedBytes(tmp, want)) return r;
      remaining -= want;
    }
    if (!r.ok) fail(sc.phase == Scanner::Phase::Value ? "psvts_unclosed" : "psvts_not_found");
    return r;
  }

  if (kind == M4xHttp::BodyKind::Chunked) {
    while (true) {
      std::string sizeLine;
      if (!M4xHttp::readLine(s, sizeLine, lim, deadline, lastDataMs, err)) {
        fail(err.empty() ? "chunk_size_line" : err.c_str());
        return r;
      }
      size_t chunkSize = 0;
      if (!M4xHttp::parseHexSize(sizeLine, chunkSize)) {
        fail("chunk_size_parse");
        return r;
      }
      if (chunkSize == 0) {
        // Drain trailers.
        while (true) {
          std::string trailer;
          if (!M4xHttp::readLine(s, trailer, lim, deadline, lastDataMs, err)) {
            fail(err.empty() ? "chunk_trailer" : err.c_str());
            return r;
          }
          if (trailer.empty()) break;
        }
        if (!r.ok) fail(sc.phase == Scanner::Phase::Value ? "psvts_unclosed" : "psvts_not_found");
        return r;
      }
      size_t left = chunkSize;
      uint8_t tmp[kReadChunk];
      while (left > 0) {
        const size_t want = left < sizeof(tmp) ? left : sizeof(tmp);
        if (!M4xHttp::readExact(s, tmp, want, lim, deadline, lastDataMs, err)) {
          fail(err.empty() ? "chunk_data" : err.c_str());
          return r;
        }
        if (!feedBytes(tmp, want)) {
          // Found or hard error: drain remaining chunk + CRLF best-effort is optional;
          // caller will close the connection. Still consume remaining of this chunk if found
          // so keep-alive peers stay clean when possible.
          if (r.ok) {
            size_t rest = left - want;
            uint8_t sink[kReadChunk];
            while (rest > 0) {
              const size_t w = rest < sizeof(sink) ? rest : sizeof(sink);
              std::string e2;
              if (!M4xHttp::readExact(s, sink, w, lim, deadline, lastDataMs, e2)) break;
              rest -= w;
            }
            uint8_t crlf[2];
            std::string e3;
            (void)M4xHttp::readExact(s, crlf, 2, lim, deadline, lastDataMs, e3);
          }
          return r;
        }
        left -= want;
      }
      uint8_t crlf[2];
      if (!M4xHttp::readExact(s, crlf, 2, lim, deadline, lastDataMs, err)) {
        fail(err.empty() ? "chunk_crlf" : err.c_str());
        return r;
      }
      if (crlf[0] != '\r' || crlf[1] != '\n') {
        fail("chunk_crlf_bad");
        return r;
      }
    }
  }

  // UntilClose
  uint8_t tmp[kReadChunk];
  while (true) {
    if (M4xHttp::cancelled(lim)) {
      fail("cancelled");
      return r;
    }
    if (M4xHttp::timedOut(lim, deadline)) {
      fail("timeout");
      return r;
    }
    if (M4xHttp::idleTimedOut(lim, lastDataMs)) {
      if (!s.connected() && s.available() <= 0) {
        if (!r.ok) fail(sc.phase == Scanner::Phase::Value ? "psvts_unclosed" : "psvts_not_found");
        return r;
      }
      fail("idle_timeout");
      return r;
    }
    const int avail = s.available();
    if (avail <= 0) {
      if (!s.connected()) {
        if (!r.ok) fail(sc.phase == Scanner::Phase::Value ? "psvts_unclosed" : "psvts_not_found");
        return r;
      }
      if (lim.onWait) lim.onWait();
      continue;
    }
    const int rdb = s.read(tmp, sizeof(tmp));
    if (rdb < 0) {
      if (!r.ok) fail(sc.phase == Scanner::Phase::Value ? "psvts_unclosed" : "psvts_not_found");
      return r;
    }
    if (rdb == 0) {
      if (lim.onWait) lim.onWait();
      continue;
    }
    lastDataMs = M4xHttp::nowOr0(lim);
    if (!feedBytes(tmp, static_cast<size_t>(rdb))) return r;
  }
}

// Convenience: scan a contiguous buffer (unit tests / simulator mock path).
inline Result scanBuffer(const char* data, size_t len, size_t maxScan = kMaxScanBytes,
                         size_t maxValue = kMaxValueLen) {
  struct BufStream : M4xHttp::Stream {
    const uint8_t* p = nullptr;
    size_t n = 0;
    size_t off = 0;
    int available() override {
      if (off >= n) return 0;
      return static_cast<int>(n - off);
    }
    int read(uint8_t* buf, size_t want) override {
      if (off >= n || want == 0) return 0;
      size_t take = want;
      if (take > n - off) take = n - off;
      std::memcpy(buf, p + off, take);
      off += take;
      return static_cast<int>(take);
    }
    bool connected() override { return off < n; }
  } stream;
  stream.p = reinterpret_cast<const uint8_t*>(data);
  stream.n = len;
  M4xHttp::Limits lim;
  lim.maxBody = maxScan;
  lim.totalTimeoutMs = 0;
  lim.idleTimeoutMs = 0;
  lim.nowMs = nullptr;
  return scanBody(stream, M4xHttp::BodyKind::UntilClose, 0, lim, maxScan, maxValue);
}

}  // namespace M4xPsvts
