#pragma once

// Platform-independent policy for the M4 serial debug bridge.
// Header-only, no Arduino/FreeRTOS deps — unit-tested on host and used by
// the firmware implementation when CROSSPOINT_MURPHY_M4 (single M4 firmware;
// runtime-authorized via Developer Options).

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace M4SerialDebugPolicy {

inline constexpr size_t kMaxLineLen = 1600;
inline constexpr size_t kMaxRawChunk = 768;
inline constexpr size_t kMaxReqIdLen = 24;
inline constexpr size_t kMaxPackageBytes = 4u * 1024u * 1024u;
inline constexpr size_t kMaxInboxNameLen = 64;
inline constexpr unsigned long kHostActivityWindowMs = 120000;  // 2 min after last host frame

// ---- Line intake: overflow discards entire line until newline ----

struct LineIntake {
  static constexpr size_t kCap = kMaxLineLen;
  char buf[kCap + 1] = {};
  size_t len = 0;
  bool discardUntilNewline = false;

  void reset() {
    len = 0;
    buf[0] = 0;
  }

  // Returns true when a complete non-empty line is ready in buf (NUL-terminated).
  // Overlong lines set discardUntilNewline and never surface as complete.
  bool feed(char c, bool& lineReady) {
    lineReady = false;
    if (c == '\r') return true;
    if (c == '\n') {
      if (discardUntilNewline) {
        discardUntilNewline = false;
        reset();
        return true;
      }
      buf[len] = 0;
      if (len > 0) {
        lineReady = true;
      }
      // Caller must read buf before next feed; we leave content until reset after handle.
      return true;
    }
    if (discardUntilNewline) {
      return true;
    }
    if (len >= kCap) {
      discardUntilNewline = true;
      reset();
      return true;
    }
    buf[len++] = c;
    return true;
  }

  void clearAfterHandle() { reset(); }
};

// ---- Strict Base64 (RFC 4648 alphabet; optional ASCII whitespace only) ----

inline int b64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

inline bool isAsciiWs(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// Strict decode: only Base64 alphabet + '=' padding; ASCII whitespace may be skipped.
// Rejects embedded garbage, invalid padding, and truncated groups.
inline bool b64DecodeStrict(const char* in, size_t inLen, uint8_t* out, size_t outCap, size_t& outLen) {
  outLen = 0;
  if (!in || !out) return false;

  // Decode one quartet at a time. Keeping only four 6-bit values avoids
  // unbounded signed left shifts (undefined behaviour on long frames).
  uint8_t q[4] = {};
  size_t qLen = 0;
  bool finished = false;
  for (size_t i = 0; i < inLen; ++i) {
    const char c = in[i];
    if (isAsciiWs(c)) continue;
    if (finished) return false;
    if (c == '=') {
      q[qLen++] = 64;
    } else {
      const int d = b64Value(c);
      if (d < 0) return false;
      q[qLen++] = static_cast<uint8_t>(d);
    }
    if (qLen != 4) continue;

    if (q[0] == 64 || q[1] == 64) return false;
    if (outLen >= outCap) return false;
    out[outLen++] = static_cast<uint8_t>((q[0] << 2) | (q[1] >> 4));

    if (q[2] == 64) {
      if (q[3] != 64 || (q[1] & 0x0F) != 0) return false;
      finished = true;
    } else {
      if (outLen >= outCap) return false;
      out[outLen++] = static_cast<uint8_t>((q[1] << 4) | (q[2] >> 2));
      if (q[3] == 64) {
        if ((q[2] & 0x03) != 0) return false;
        finished = true;
      } else {
        if (outLen >= outCap) return false;
        out[outLen++] = static_cast<uint8_t>((q[2] << 6) | q[3]);
      }
    }
    qLen = 0;
  }
  return qLen == 0;
}

inline size_t b64Encode(const uint8_t* in, size_t inLen, char* out, size_t outCap) {
  static const char kTab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  if (!out || outCap < 4) return 0;
  size_t o = 0;
  for (size_t i = 0; i < inLen && o + 4 < outCap; i += 3) {
    const uint32_t n = static_cast<uint32_t>(in[i]) << 16 |
                       (i + 1 < inLen ? static_cast<uint32_t>(in[i + 1]) << 8 : 0) |
                       (i + 2 < inLen ? static_cast<uint32_t>(in[i + 2]) : 0);
    out[o++] = kTab[(n >> 18) & 63];
    out[o++] = kTab[(n >> 12) & 63];
    out[o++] = (i + 1 < inLen) ? kTab[(n >> 6) & 63] : '=';
    out[o++] = (i + 2 < inLen) ? kTab[n & 63] : '=';
  }
  if (o < outCap) out[o] = 0;
  return o;
}

// ---- Request ID / filename validation ----

inline bool isValidReqId(const char* id) {
  if (!id || !*id) return false;
  size_t n = 0;
  for (const char* p = id; *p; ++p, ++n) {
    if (n >= kMaxReqIdLen) return false;
    const unsigned char c = static_cast<unsigned char>(*p);
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                    c == '-' || c == '.';
    if (!ok) return false;
  }
  return n > 0 && n <= kMaxReqIdLen;
}

// Returns machine error key or nullptr if OK.
inline const char* validateInboxFilename(const char* name) {
  if (!name || !*name) return "bad_name";
  const size_t n = std::strlen(name);
  if (n < 5 || n > kMaxInboxNameLen) return "bad_name";
  if (std::strchr(name, '/') || std::strchr(name, '\\') || std::strstr(name, "..")) return "path_traversal";
  if (name[0] == '.' || name[0] == '/') return "bad_name";
  if (n < 4) return "bad_ext";
  // case-insensitive .m4x
  const char* ext = name + n - 4;
  auto lower = [](char c) -> char {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  };
  if (lower(ext[0]) != '.' || lower(ext[1]) != 'm' || lower(ext[2]) != '4' || lower(ext[3]) != 'x') {
    return "bad_ext";
  }
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(name[i]);
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                    c == '-' || c == '.';
    if (!ok) return "bad_name";
  }
  return nullptr;
}

// ---- Chunk sequence policy ----

enum class ChunkAccept { Ok, Reject, ReplayLast };

struct ChunkSession {
  bool active = false;
  uint32_t byteTotal = 0;       // declared package size
  uint32_t bytesReceived = 0;
  uint32_t nextSeq = 0;
  uint32_t declaredTotal = 0;   // chunk count; 0 = not yet set
  bool hasDeclaredTotal = false;
  uint32_t lastAcceptedSeq = 0;
  bool hasLastAccepted = false;
  char lastChunkReqId[kMaxReqIdLen + 1] = {};

  void reset() {
    active = false;
    byteTotal = 0;
    bytesReceived = 0;
    nextSeq = 0;
    declaredTotal = 0;
    hasDeclaredTotal = false;
    lastAcceptedSeq = 0;
    hasLastAccepted = false;
    lastChunkReqId[0] = 0;
  }

  void begin(uint32_t size) {
    reset();
    active = true;
    byteTotal = size;
  }

  // Validate and accept one chunk. Does not write data; only policy.
  // On Ok: caller must write data then call commitAccept(reqId, seq, len).
  ChunkAccept evaluate(const char* reqId, uint32_t seq, uint32_t total, size_t len, const char** errKey) const {
    *errKey = nullptr;
    if (!active) {
      *errKey = "no_upload";
      return ChunkAccept::Reject;
    }
    if (total == 0) {
      *errKey = "bad_chunk_total";
      return ChunkAccept::Reject;
    }
    if (seq >= total) {
      *errKey = "bad_chunk_seq";
      return ChunkAccept::Reject;
    }
    if (hasDeclaredTotal && total != declaredTotal) {
      *errKey = "total_mismatch";
      return ChunkAccept::Reject;
    }
    if (len == 0 || len > kMaxRawChunk) {
      *errKey = "bad_chunk";
      return ChunkAccept::Reject;
    }
    // Exact same request ID for last accepted chunk → replay ack (lost response).
    if (hasLastAccepted && reqId && lastChunkReqId[0] && std::strcmp(reqId, lastChunkReqId) == 0 &&
        seq == lastAcceptedSeq) {
      return ChunkAccept::ReplayLast;
    }
    if (seq != nextSeq) {
      *errKey = "seq_mismatch";
      return ChunkAccept::Reject;
    }
    if (bytesReceived + static_cast<uint32_t>(len) > byteTotal) {
      *errKey = "size_overflow";
      return ChunkAccept::Reject;
    }
    return ChunkAccept::Ok;
  }

  void commitAccept(const char* reqId, uint32_t seq, uint32_t total, size_t len) {
    if (!hasDeclaredTotal) {
      hasDeclaredTotal = true;
      declaredTotal = total;
    }
    bytesReceived += static_cast<uint32_t>(len);
    lastAcceptedSeq = seq;
    hasLastAccepted = true;
    nextSeq = seq + 1;
    if (reqId) {
      std::strncpy(lastChunkReqId, reqId, kMaxReqIdLen);
      lastChunkReqId[kMaxReqIdLen] = 0;
    }
  }

  // Ready for install_commit: all declared chunks arrived and byte size matches.
  bool readyToCommit(const char** errKey) const {
    *errKey = nullptr;
    if (!active) {
      *errKey = "no_upload";
      return false;
    }
    if (!hasDeclaredTotal) {
      *errKey = "incomplete_chunks";
      return false;
    }
    if (nextSeq != declaredTotal) {
      *errKey = "incomplete_chunks";
      return false;
    }
    if (bytesReceived != byteTotal) {
      *errKey = "size_mismatch";
      return false;
    }
    return true;
  }
};

// No-op only when content SHA matches previously staged package of same name.
inline bool isContentNoop(const char* uploadedShaHex64, const char* existingShaHex64) {
  if (!uploadedShaHex64 || !existingShaHex64) return false;
  if (std::strlen(uploadedShaHex64) != 64 || std::strlen(existingShaHex64) != 64) return false;
  for (int i = 0; i < 64; ++i) {
    char a = uploadedShaHex64[i];
    char b = existingShaHex64[i];
    if (a >= 'A' && a <= 'F') a = static_cast<char>(a - 'A' + 'a');
    if (b >= 'A' && b <= 'F') b = static_cast<char>(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

inline bool isHex64(const char* s) {
  if (!s || std::strlen(s) != 64) return false;
  for (int i = 0; i < 64; ++i) {
    const char c = s[i];
    const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!ok) return false;
  }
  return true;
}

// Wi-Fi bulk install: device is HTTP *client* only (no bridge listener).
// Host serves .m4x on LAN; serial only carries this small control request.
// Allow only plain http:// to RFC1918 / loopback IPv4 hosts (no creds, no HTTPS).
inline constexpr size_t kMaxInstallHttpUrlLen = 256;

inline bool isDecimalOctet(const char* s, size_t n, int& out) {
  if (n == 0 || n > 3) return false;
  int v = 0;
  for (size_t i = 0; i < n; ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + (s[i] - '0');
  }
  if (v > 255) return false;
  // Reject leading zeros except "0" itself (avoid octal surprises).
  if (n > 1 && s[0] == '0') return false;
  out = v;
  return true;
}

inline bool isPrivateOrLoopbackIpv4(int a, int b, int c, int d) {
  (void)c;
  (void)d;
  if (a == 127) return true;                       // loopback
  if (a == 10) return true;                        // 10/8
  if (a == 192 && b == 168) return true;           // 192.168/16
  if (a == 172 && b >= 16 && b <= 31) return true; // 172.16/12
  return false;
}

// Returns error key or nullptr if URL is acceptable for install_http.
inline const char* validateInstallHttpUrl(const char* url) {
  if (!url || !*url) return "url_invalid";
  const size_t n = std::strlen(url);
  if (n < 12 || n > kMaxInstallHttpUrlLen) return "url_invalid";
  if (std::strncmp(url, "http://", 7) != 0) return "url_scheme";
  const char* host = url + 7;
  // Reject userinfo (http://user:pass@host).
  for (const char* p = host; *p && *p != '/'; ++p) {
    if (*p == '@') return "url_userinfo";
  }
  // Host must be IPv4 dotted-quad (no DNS — avoids SSRF via public resolve).
  int o[4] = {};
  const char* p = host;
  for (int i = 0; i < 4; ++i) {
    const char* start = p;
    while (*p >= '0' && *p <= '9') ++p;
    if (!isDecimalOctet(start, static_cast<size_t>(p - start), o[i])) return "url_host";
    if (i < 3) {
      if (*p != '.') return "url_host";
      ++p;
    }
  }
  if (!isPrivateOrLoopbackIpv4(o[0], o[1], o[2], o[3])) return "url_not_lan";
  // Optional :port
  if (*p == ':') {
    ++p;
    const char* ps = p;
    if (*p < '0' || *p > '9') return "url_port";
    int port = 0;
    while (*p >= '0' && *p <= '9') {
      port = port * 10 + (*p - '0');
      if (port > 65535) return "url_port";
      ++p;
    }
    if (p == ps || port == 0) return "url_port";
  }
  // Path required (at least "/x")
  if (*p != '/') return "url_path";
  if (p[1] == 0) return "url_path";
  // Reject traversal / control chars in remainder.
  for (const char* q = p; *q; ++q) {
    const unsigned char c = static_cast<unsigned char>(*q);
    if (c < 0x20 || c == 0x7f) return "url_invalid";
    if (c == ' ' || c == '\\') return "url_invalid";
  }
  if (std::strstr(p, "..") != nullptr) return "url_path";
  return nullptr;
}

// PBM P4 row packing: black=true → set bit (MSB first).
inline uint8_t packPbmByte(const bool bits[8], int validBits) {
  uint8_t byte = 0;
  for (int b = 0; b < validBits && b < 8; ++b) {
    if (bits[b]) byte |= static_cast<uint8_t>(0x80 >> b);
  }
  return byte;
}

inline size_t pbmRowBytes(int width) { return static_cast<size_t>((width + 7) / 8); }
inline size_t pbmTotalBytes(int width, int height) {
  return pbmRowBytes(width) * static_cast<size_t>(height > 0 ? height : 0);
}

// Host activity window: keep device awake only while host is actively talking.
// When unauthorized, host activity must never keep the device awake.
inline bool hostActivityKeepsAwake(unsigned long nowMs, unsigned long lastHostMs, unsigned long windowMs,
                                   bool authorized = true) {
  if (!authorized) return false;
  if (lastHostMs == 0) return false;
  return (nowMs - lastHostMs) < windowMs;
}

// ---- Runtime authorization (Developer Options switch) ----
// Desired setting comes only from local UI-persisted settings (never serial/web).

enum class AuthTransition : uint8_t { None = 0, Enable = 1, Disable = 2 };

// Clamp persisted JSON / legacy values: only exact 1 is on.
inline uint8_t clampDeveloperSerialDebug(int raw) { return (raw == 1) ? 1 : 0; }

struct AuthorizationState {
  bool authorized = false;

  AuthTransition applyDesired(bool desired) {
    if (desired == authorized) return AuthTransition::None;
    authorized = desired;
    return desired ? AuthTransition::Enable : AuthTransition::Disable;
  }

  // While disabled, no complete line may be delivered for execution.
  // RX bytes are discarded (optionally still advance overflow discard state).
  bool shouldExecuteFrames() const { return authorized; }

  // Sleep keep-alive only when authorized and recent host traffic.
  bool shouldKeepAwake(unsigned long nowMs, unsigned long lastHostMs, unsigned long windowMs) const {
    return hostActivityKeepsAwake(nowMs, lastHostMs, windowMs, authorized);
  }
};

// Serial ops that must never change authorization (defense in depth for tests).
inline bool opCanEnableAuthorization(const char* op) {
  (void)op;
  return false;  // no protocol op may enable; only physical UI
}

// Rate limit: first inject never blocked by zero epoch.
inline bool rateLimitBusy(unsigned long nowMs, unsigned long lastInjectMs, bool everInjected,
                          unsigned long minIntervalMs) {
  if (!everInjected) return false;
  return (nowMs - lastInjectMs) < minIntervalMs;
}

}  // namespace M4SerialDebugPolicy
