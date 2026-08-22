#pragma once

// Shared Legado bridge utilities: stable short book IDs derived from the
// full bookUrl locator, a sidecar mapping file, and the HTTP endpoint base.
//
// The M4 host only accepts short, filesystem-safe book IDs (kMaxBookIdLen=64,
// idOk() rejects '/', '?', '#', spaces). Legado bookUrl values are long
// locators (often content://... or https URLs), so we never pass them through
// the host ID path. Instead:
//   bookId = short FNV-1a hex of the bookUrl (16 chars, safe)
//   sidecar row maps bookId -> full bookUrl for catalog/content requests.
//
// Endpoint base (phone web service) is no longer a compile-time constant:
//   1. Last working base is persisted under the app data root.
//   2. On miss/stale, ensureEndpoint() probes recent Wi-Fi-transfer visitor IPs
//      on the current SSID against known Legado-family web ports.
//
// ID/path helpers stay SD-free for host tests. Endpoint probe/load live in the
// .cpp (SD + HTTP).

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>

namespace M4LegadoBridge {

// Compile-time fallback only used before any discovery succeeds (dev default).
inline constexpr const char* kDefaultBase = "http://192.168.0.118:1122";

// Ports used by 开源阅读 / Legado app web service and common forks/server
// editions. Order = probe priority (most common first).
//   1122  — official Legado Android "Web 服务" default
//   8080  — frequent custom / docker / reverse-proxy
//   4396  — common 阅读3 multi-user / community server builds
//   2060  — hectorqin/reader jar default in many deploy guides
//   8081  — alternate custom
//   9080  — some container maps
//   80    — rare bare HTTP
//   1234  — occasional third-party plugin docs
inline constexpr uint16_t kProbePorts[] = {1122, 8080, 4396, 2060, 8081, 9080, 80, 1234};
inline constexpr size_t kProbePortCount = sizeof(kProbePorts) / sizeof(kProbePorts[0]);

// API path probes (relative to base). First match wins for that host:port.
inline constexpr const char* kProbePaths[] = {
    "/getBookshelf",          // official app web service
    "/reader3/getBookshelf",  // some server-edition builds
};
inline constexpr size_t kProbePathCount = sizeof(kProbePaths) / sizeof(kProbePaths[0]);

// Stable 16-hex-digit FNV-1a over the locator. Never exposes the URL in UI,
// history URIs, cache paths or FileRows keys.
inline std::string shortId(const std::string& s) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (unsigned char c : s) {
    h ^= c;
    h *= 0x100000001b3ULL;
  }
  char buf[17];
  snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
  return std::string(buf);
}

inline bool idOkShort(const std::string& id) {
  if (id.size() != 16) return false;
  for (char c : id) {
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!hex) return false;
  }
  return true;
}

inline std::string sidecarPath(const std::string& appDataRoot) {
  // Keep the sidecar next to shelf_rows.tsv under provider/ — same directory
  // the discovery AtomicRowsSink already ensureParentDirs()'d, so open/write
  // is reliable on device FAT.
  return appDataRoot.empty() ? std::string() : appDataRoot + "/provider/legado_books.tsv";
}

inline std::string endpointPath(const std::string& appDataRoot) {
  return appDataRoot.empty() ? std::string() : appDataRoot + "/provider/endpoint.txt";
}

// Current base URL (http://host:port, no trailing slash). Empty until loaded
// or discovered. Thread-safe for read after ensureEndpoint.
std::string baseUrl();

// Adopt a verified base. Manual entry stages a candidate with persist=false and
// only persists it after the bookshelf request succeeds.
void setBaseUrl(const std::string& appDataRoot, const std::string& base, bool persist = true);

// Drop an in-memory candidate after a failed manual request. The last saved
// successful endpoint remains on SD for the next automatic attempt.
void clearBaseUrl();

// Load saved base for this app; return empty if missing.
std::string loadSavedBase(const std::string& appDataRoot);

// Ensure a working base is available: try saved URL, then probe visitor IPs on
// the current Wi-Fi against kProbePorts × kProbePaths. Returns false when no
// Legado-like HTTP service answers. On success baseUrl() is non-empty and
// persisted under appDataRoot.
bool ensureEndpoint(const std::string& appDataRoot);

// Host-testable pure helpers (no SD / no network).
struct ParsedEndpoint {
  std::string host;
  uint16_t port = 0;
  std::string base;
};

inline std::string trimEndpointWhitespace(std::string value) {
  size_t begin = 0;
  while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
  size_t end = value.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
  value.erase(end);
  value.erase(0, begin);
  return value;
}

inline bool endpointDigitsPort(const std::string& raw, uint16_t& port) {
  if (raw.empty() || raw.size() > 5) return false;
  unsigned value = 0;
  for (char c : raw) {
    if (c < '0' || c > '9') return false;
    value = value * 10u + static_cast<unsigned>(c - '0');
    if (value > 65535u) return false;
  }
  if (value == 0) return false;
  port = static_cast<uint16_t>(value);
  return true;
}

inline bool endpointIpv4Ok(const std::string& host) {
  size_t start = 0;
  int parts = 0;
  while (start < host.size()) {
    const size_t dot = host.find('.', start);
    const size_t end = dot == std::string::npos ? host.size() : dot;
    if (end == start || end - start > 3) return false;
    unsigned value = 0;
    for (size_t i = start; i < end; ++i) {
      const char c = host[i];
      if (c < '0' || c > '9') return false;
      value = value * 10u + static_cast<unsigned>(c - '0');
    }
    if (value > 255u) return false;
    ++parts;
    if (dot == std::string::npos) break;
    start = dot + 1;
  }
  return parts == 4;
}

inline bool endpointHostnameOk(const std::string& host) {
  if (host.empty() || host.size() > 253 || host.front() == '.' || host.back() == '.') return false;
  bool hasNonNumeric = false;
  size_t labelStart = 0;
  for (size_t i = 0; i <= host.size(); ++i) {
    if (i != host.size() && host[i] != '.') continue;
    if (i == labelStart || i - labelStart > 63) return false;
    if (host[labelStart] == '-' || host[i - 1] == '-') return false;
    for (size_t j = labelStart; j < i; ++j) {
      const unsigned char c = static_cast<unsigned char>(host[j]);
      if (!(std::isalnum(c) || c == '-')) return false;
      if (!(c >= '0' && c <= '9') && c != '.') hasNonNumeric = true;
    }
    labelStart = i + 1;
  }
  return hasNonNumeric || host.find('.') == std::string::npos;
}

// Accept either a host/IP plus a separate port or a complete http://host:port
// value. If a complete URL contains a port, that port wins over the separate
// field; this prevents the common http://host:1122:1122 duplication bug.
inline bool parseEndpoint(const std::string& hostOrUrl, const std::string& portText,
                          ParsedEndpoint& out, std::string* error = nullptr) {
  out = {};
  auto fail = [&](const char* why) {
    if (error) *error = why;
    return false;
  };

  std::string raw = trimEndpointWhitespace(hostOrUrl);
  std::string suppliedPort = trimEndpointWhitespace(portText);
  if (raw.empty()) return fail("host_required");
  if (raw.find("@") != std::string::npos || raw.find_first_of("?#\\") != std::string::npos) {
    return fail("invalid_host");
  }

  if (raw.rfind("http://", 0) == 0) {
    raw.erase(0, 7);
  } else if (raw.find("://") != std::string::npos) {
    return fail("unsupported_scheme");
  }

  while (!raw.empty() && raw.back() == '/') raw.pop_back();
  if (raw.empty() || raw.find('/') != std::string::npos) return fail("unsupported_path");
  if (raw.front() == '[' || raw.back() == ']') return fail("ipv6_not_supported");

  std::string host = raw;
  std::string embeddedPort;
  const size_t colon = raw.rfind(':');
  if (colon != std::string::npos) {
    if (raw.find(':') != colon || colon == 0 || colon + 1 >= raw.size()) return fail("invalid_host");
    host = raw.substr(0, colon);
    embeddedPort = raw.substr(colon + 1);
  }
  if (host.empty() || host.find(':') != std::string::npos) return fail("invalid_host");

  uint16_t port = 0;
  if (!embeddedPort.empty()) {
    if (!endpointDigitsPort(embeddedPort, port)) return fail("invalid_port");
  } else {
    if (!endpointDigitsPort(suppliedPort, port)) return fail("invalid_port");
  }

  if (!endpointIpv4Ok(host) && !endpointHostnameOk(host)) return fail("invalid_host");

  char portBuf[8];
  std::snprintf(portBuf, sizeof(portBuf), "%u", static_cast<unsigned>(port));
  out.host = host;
  out.port = port;
  out.base = std::string("http://") + host + ":" + portBuf;
  if (error) error->clear();
  return true;
}

inline bool baseUrlOk(const std::string& base) {
  ParsedEndpoint parsed;
  std::string error;
  return parseEndpoint(base, {}, parsed, &error) && parsed.base == trimEndpointWhitespace(base);
}

enum class ManualEndpointPhase : uint8_t { Editing = 0, Connecting, Ready, Error };

// Small state seam shared by the endpoint activity and host tests. A failed
// candidate never replaces lastSuccessful; only a verified response does.
struct ManualEndpointState {
  ManualEndpointPhase phase = ManualEndpointPhase::Editing;
  std::string candidate;
  std::string lastSuccessful;
  std::string error;

  void begin(const std::string& value) {
    candidate = value;
    error.clear();
    phase = ManualEndpointPhase::Connecting;
  }
  void fail(const std::string& value) {
    error = value;
    phase = ManualEndpointPhase::Error;
  }
  void succeed() {
    lastSuccessful = candidate;
    error.clear();
    phase = ManualEndpointPhase::Ready;
  }
};

inline std::string makeBase(const std::string& ip, uint16_t port) {
  if (ip.empty() || port == 0) return {};
  char buf[48];
  std::snprintf(buf, sizeof(buf), "http://%s:%u", ip.c_str(), static_cast<unsigned>(port));
  const std::string out(buf);
  return baseUrlOk(out) ? out : std::string();
}

inline bool probeBodyLooksLikeLegado(const std::string& body, int httpStatus,
                                     const std::string& /*error*/) {
  if (httpStatus < 200 || httpStatus >= 300) return false;
  if (body.find("\"data\"") != std::string::npos) return true;
  if (body.find("isSuccess") != std::string::npos) return true;
  if (body.find("bookUrl") != std::string::npos) return true;
  if (body.find("getBookshelf") != std::string::npos) return true;
  if (body.find('[') != std::string::npos && body.find('{') != std::string::npos) return true;
  return false;
}

// Implemented in M4LegadoBridge.cpp (SDCardManager dependency).
std::string readLocator(const std::string& appDataRoot, const std::string& bookId);

// Deprecated name kept for any leftover call sites — same as baseUrl() once
// ensureEndpoint has run; otherwise the compile-time default.
inline constexpr const char* kBase = kDefaultBase;

}  // namespace M4LegadoBridge
