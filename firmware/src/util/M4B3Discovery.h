#pragma once

// Host-testable Browser Bridge LAN discovery contract.
// Firmware advertises this record over existing ESP32 mDNS only while the
// M4B3 TCP listener has a valid STA IPv4 endpoint. Android resolves the same
// type via NsdManager/DNS-SD. No BLE, no extra libraries.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace M4B3Discovery {

constexpr const char* kService = "m4b3";
constexpr const char* kProto = "tcp";
constexpr const char* kServiceType = "_m4b3._tcp";
constexpr const char* kServiceTypeDot = "_m4b3._tcp.";
constexpr const char* kInstanceName = "murphy-m4-browser";
constexpr const char* kHostname = "murphy-m4";
constexpr const char* kTxtProtoKey = "proto";
constexpr const char* kTxtProtoVal = "m4b3";
constexpr const char* kTxtRoleKey = "role";
constexpr const char* kTxtRoleVal = "browser-bridge";
constexpr uint16_t kPort = 48624;
constexpr size_t kMaxHostLen = 253;
constexpr size_t kMaxNameLen = 63;

enum class Source : uint8_t { Manual = 1, Discovered = 2, Cached = 3, None = 4, Loopback = 5 };

struct Record {
  char name[kMaxNameLen + 1];
  char type[32];
  char host[kMaxHostLen + 1];
  char protoTxt[16];
  uint16_t port;
  bool ok;
};

inline void clearRecord(Record& r) {
  std::memset(&r, 0, sizeof(r));
}

inline bool ieq(const char* a, const char* b) {
  if (!a || !b) return a == b;
  while (*a && *b) {
    char ca = *a >= 'A' && *a <= 'Z' ? static_cast<char>(*a - 'A' + 'a') : *a;
    char cb = *b >= 'A' && *b <= 'Z' ? static_cast<char>(*b - 'A' + 'a') : *b;
    if (ca != cb) return false;
    ++a;
    ++b;
  }
  return *a == 0 && *b == 0;
}

inline void copyCapped(char* dst, size_t cap, const char* src) {
  if (!dst || cap == 0) return;
  size_t n = 0;
  if (src) {
    while (src[n] && n + 1 < cap) {
      dst[n] = src[n];
      ++n;
    }
  }
  dst[n] = 0;
}

inline const char* skipSpaces(const char* s) {
  if (!s) return "";
  while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') ++s;
  return s;
}

inline void rtrimInPlace(char* s) {
  if (!s) return;
  size_t n = std::strlen(s);
  while (n > 0) {
    char c = s[n - 1];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
    s[--n] = 0;
  }
}

inline bool validPort(int port) { return port >= 1 && port <= 65535; }

inline bool validHost(const char* raw) {
  const char* s = skipSpaces(raw);
  if (!s || !*s) return false;
  char buf[kMaxHostLen + 1];
  copyCapped(buf, sizeof(buf), s);
  rtrimInPlace(buf);
  const size_t n = std::strlen(buf);
  if (n == 0 || n > kMaxHostLen) return false;
  if (ieq(buf, "0.0.0.0") || ieq(buf, "255.255.255.255")) return false;
  if (ieq(buf, "loopback") || ieq(buf, "local")) return false;
  if (std::strstr(buf, "://") != nullptr) return false;
  for (size_t i = 0; i < n; ++i) {
    const char c = buf[i];
    if (c == ' ' || c == '\t' || c == '/' || c == '\\' || c == ':' || c == '@') return false;
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '-' || c == '_';
    if (!ok) return false;
  }
  if (buf[0] == '.' || buf[n - 1] == '.') return false;
  return true;
}

inline bool normalizeType(const char* raw, char* out, size_t cap) {
  if (!out || cap < 12) return false;
  const char* s = skipSpaces(raw);
  char buf[32];
  copyCapped(buf, sizeof(buf), s);
  rtrimInPlace(buf);
  size_t n = std::strlen(buf);
  while (n > 0 && buf[n - 1] == '.') buf[--n] = 0;
  for (size_t i = 0; i < n; ++i) {
    if (buf[i] >= 'A' && buf[i] <= 'Z') buf[i] = static_cast<char>(buf[i] - 'A' + 'a');
  }
  if (!ieq(buf, kServiceType)) return false;
  copyCapped(out, cap, kServiceType);
  return true;
}

inline bool protoTxtOk(const char* protoTxt) {
  const char* s = skipSpaces(protoTxt);
  if (!s || !*s) return true;  // missing TXT is allowed; type already scopes the service
  char buf[16];
  copyCapped(buf, sizeof(buf), s);
  rtrimInPlace(buf);
  return ieq(buf, kTxtProtoVal);
}

inline bool parseRecord(const char* name, const char* type, const char* host, int port, const char* protoTxt,
                        Record& out) {
  clearRecord(out);
  if (!normalizeType(type, out.type, sizeof(out.type))) return false;
  if (!validHost(host) || !validPort(port) || !protoTxtOk(protoTxt)) return false;
  const char* n = skipSpaces(name);
  if (!n || !*n) n = kInstanceName;
  copyCapped(out.name, sizeof(out.name), n);
  rtrimInPlace(out.name);
  if (!out.name[0] || std::strlen(out.name) > kMaxNameLen) return false;
  copyCapped(out.host, sizeof(out.host), skipSpaces(host));
  rtrimInPlace(out.host);
  copyCapped(out.protoTxt, sizeof(out.protoTxt), protoTxt ? skipSpaces(protoTxt) : "");
  out.port = static_cast<uint16_t>(port);
  out.ok = true;
  return true;
}

// Advertise only when the M4B3 TCP listener is bound on a real STA IPv4.
inline bool advertiseAllowed(bool listening, bool staReady, const char* bindIp) {
  if (!listening || !staReady) return false;
  return validHost(bindIp);
}

inline int sourceRank(Source s) {
  switch (s) {
    case Source::Manual:
      return 0;
    case Source::Discovered:
      return 1;
    case Source::Cached:
      return 2;
    case Source::Loopback:
      return 3;
    case Source::None:
    default:
      return 4;
  }
}

}  // namespace M4B3Discovery
