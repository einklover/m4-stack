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

// Persist and adopt a verified base (also used after manual override).
void setBaseUrl(const std::string& appDataRoot, const std::string& base);

// Load saved base for this app; return empty if missing.
std::string loadSavedBase(const std::string& appDataRoot);

// Ensure a working base is available: try saved URL, then probe visitor IPs on
// the current Wi-Fi against kProbePorts × kProbePaths. Returns false when no
// Legado-like HTTP service answers. On success baseUrl() is non-empty and
// persisted under appDataRoot.
bool ensureEndpoint(const std::string& appDataRoot);

// Host-testable pure helpers (no SD / no network).
inline bool baseUrlOk(const std::string& base) {
  if (base.size() < 12 || base.size() > 64) return false;
  if (base.rfind("http://", 0) != 0) return false;
  if (base.find('@') != std::string::npos) return false;
  const std::string rest = base.substr(7);
  if (rest.find('/') != std::string::npos) return false;
  const size_t colon = rest.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= rest.size()) return false;
  const std::string host = rest.substr(0, colon);
  const std::string port = rest.substr(colon + 1);
  // Auto-discovery only trusts IPv4 hosts (visitor store records A.B.C.D).
  size_t dots = 0;
  for (char c : host) {
    if (c == '.') ++dots;
    else if (c < '0' || c > '9') return false;
  }
  if (dots != 3 || host.size() < 7 || host.size() > 15) return false;
  if (port.empty() || port.size() > 5) return false;
  for (char c : port) {
    if (c < '0' || c > '9') return false;
  }
  int p = 0;
  for (char c : port) p = p * 10 + (c - '0');
  return p > 0 && p <= 65535;
}

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
