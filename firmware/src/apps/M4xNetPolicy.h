#pragma once

// Pure network policy helpers for M4x host (host-testable).

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace M4xNetPolicy {

// Response body caps must remain representable in Lua (heap 512 KiB total).
// Without PSRAM: keep HTTP body ≤ ~192 KiB so scripts/UI state still fit.
// With PSRAM: allow larger network buffers, but pushNetResult still caps by Lua headroom.
inline constexpr size_t kMaxBodyInternalRam = 192 * 1024;
inline constexpr size_t kMaxBodyWithPsram = 768 * 1024;
inline constexpr int kMaxRedirects = 5;
inline constexpr int kMinTimeoutMs = 1000;
inline constexpr int kMaxTimeoutMs = 60000;
inline constexpr int kDefaultTimeoutMs = 20000;

// Lowercase ASCII helper.
inline std::string toLowerAscii(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Extract scheme (http/https) lowercased; empty if missing.
inline std::string urlScheme(const std::string& url) {
  const size_t colon = url.find("://");
  if (colon == std::string::npos || colon == 0) return {};
  return toLowerAscii(url.substr(0, colon));
}

// Host (no port) lowercased; empty on parse failure.
inline std::string urlHost(const std::string& url) {
  const size_t scheme = url.find("://");
  if (scheme == std::string::npos) return {};
  size_t start = scheme + 3;
  if (start >= url.size()) return {};
  // skip userinfo
  const size_t at = url.find('@', start);
  const size_t slash = url.find('/', start);
  const size_t limit = slash == std::string::npos ? url.size() : slash;
  if (at != std::string::npos && at < limit) start = at + 1;
  size_t end = start;
  while (end < url.size() && url[end] != '/' && url[end] != '?' && url[end] != '#' && url[end] != ':') ++end;
  if (end <= start) return {};
  return toLowerAscii(url.substr(start, end - start));
}

inline bool isHttpsUrl(const std::string& url) { return urlScheme(url) == "https"; }

// Default: only HTTPS is allowed for plugins (cookie safety).
inline bool isAllowedUrl(const std::string& url) { return isHttpsUrl(url); }

// True when redirect target is same host (and both https, or same scheme).
inline bool sameOriginHost(const std::string& fromUrl, const std::string& toUrl) {
  const std::string h1 = urlHost(fromUrl);
  const std::string h2 = urlHost(toUrl);
  if (h1.empty() || h2.empty()) return false;
  if (h1 != h2) return false;
  // Prefer same scheme; allow https→https only when schemes present.
  const std::string s1 = urlScheme(fromUrl);
  const std::string s2 = urlScheme(toUrl);
  if (!s1.empty() && !s2.empty() && s1 != s2) return false;
  return true;
}

// Sensitive request headers that must not follow cross-host redirects.
inline bool isSensitiveRequestHeader(const std::string& name) {
  const std::string n = toLowerAscii(name);
  return n == "cookie" || n == "authorization" || n == "proxy-authorization";
}

// Resolve Location against base URL (absolute, protocol-relative, path-absolute, relative).
inline std::string resolveRedirectUrl(const std::string& base, const std::string& location) {
  if (location.empty()) return {};
  if (location.find("://") != std::string::npos) return location;
  if (location.size() >= 2 && location[0] == '/' && location[1] == '/') {
    const std::string sch = urlScheme(base);
    if (sch.empty()) return std::string("https:") + location;
    return sch + ":" + location;
  }
  const size_t scheme = base.find("://");
  if (scheme == std::string::npos) return location;
  const size_t pathStart = base.find('/', scheme + 3);
  const std::string origin = pathStart == std::string::npos ? base : base.substr(0, pathStart);
  if (!location.empty() && location[0] == '/') return origin + location;
  // Relative to last path segment directory.
  std::string dir = pathStart == std::string::npos ? origin + "/" : base.substr(0, base.find_last_of('/') + 1);
  // Strip query/fragment from dir base
  const size_t q = dir.find('?');
  if (q != std::string::npos) dir = dir.substr(0, q);
  return dir + location;
}

// Filter request headers for a redirect hop.
// If sameHost, keep all; else drop Cookie/Authorization.
inline std::vector<std::pair<std::string, std::string>> headersForRedirect(
    const std::vector<std::pair<std::string, std::string>>& headers, bool sameHost) {
  if (sameHost) return headers;
  std::vector<std::pair<std::string, std::string>> out;
  out.reserve(headers.size());
  for (const auto& hv : headers) {
    if (!isSensitiveRequestHeader(hv.first)) out.push_back(hv);
  }
  return out;
}

// Multi-value response headers (preserves all Set-Cookie lines).
struct ResponseHeader {
  std::string name;
  std::string value;
};

// Collect Set-Cookie values (case-insensitive name match).
inline std::vector<std::string> allSetCookieValues(const std::vector<ResponseHeader>& headers) {
  std::vector<std::string> out;
  for (const auto& h : headers) {
    if (toLowerAscii(h.name) == "set-cookie") out.push_back(h.value);
  }
  return out;
}

// Parse first name=value from a Set-Cookie line (ignore attributes after ';').
inline bool parseSetCookiePair(const std::string& setCookieLine, std::string& nameOut, std::string& valueOut) {
  size_t end = setCookieLine.find(';');
  const std::string nv = end == std::string::npos ? setCookieLine : setCookieLine.substr(0, end);
  const size_t eq = nv.find('=');
  if (eq == std::string::npos || eq == 0) return false;
  std::string name = nv.substr(0, eq);
  while (!name.empty() && name[0] == ' ') name.erase(name.begin());
  while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
  if (name.empty()) return false;
  nameOut = name;
  valueOut = nv.substr(eq + 1);
  return true;
}

// Parse wr_* cookie pairs from a single Set-Cookie line or Cookie header value.
inline std::vector<std::pair<std::string, std::string>> parseWereadCookieAttrs(const std::string& setCookieLine) {
  std::vector<std::pair<std::string, std::string>> out;
  std::string name, value;
  if (!parseSetCookiePair(setCookieLine, name, value)) return out;
  const std::string ln = toLowerAscii(name);
  if (ln == "wr_vid" || ln == "wr_skey" || ln == "wr_rt") {
    out.emplace_back(ln, value);
  }
  return out;
}

// Merge Set-Cookie name=value into a Cookie request header map (case-insensitive name).
// Does not include attributes; overwrites prior values for the same cookie name.
inline void mergeSetCookiesIntoRequestHeaders(std::vector<std::pair<std::string, std::string>>& headers,
                                              const std::vector<std::string>& setCookieLines) {
  std::map<std::string, std::string> jar;  // lower name -> value
  // Seed from existing Cookie header
  for (const auto& hv : headers) {
    if (toLowerAscii(hv.first) != "cookie") continue;
    size_t i = 0;
    const std::string& c = hv.second;
    while (i < c.size()) {
      while (i < c.size() && (c[i] == ' ' || c[i] == ';')) ++i;
      size_t start = i;
      while (i < c.size() && c[i] != ';') ++i;
      std::string part = c.substr(start, i - start);
      const size_t eq = part.find('=');
      if (eq != std::string::npos && eq > 0) {
        std::string n = part.substr(0, eq);
        while (!n.empty() && n[0] == ' ') n.erase(n.begin());
        jar[toLowerAscii(n)] = part.substr(eq + 1);
      }
    }
  }
  for (const auto& line : setCookieLines) {
    std::string n, v;
    if (parseSetCookiePair(line, n, v)) jar[toLowerAscii(n)] = v;
  }
  // Rebuild Cookie header; remove old Cookie entries first
  std::vector<std::pair<std::string, std::string>> out;
  out.reserve(headers.size() + 1);
  for (const auto& hv : headers) {
    if (toLowerAscii(hv.first) != "cookie") out.push_back(hv);
  }
  if (!jar.empty()) {
    std::string cookie;
    for (const auto& kv : jar) {
      if (!cookie.empty()) cookie += "; ";
      cookie += kv.first;
      cookie += "=";
      cookie += kv.second;
    }
    out.emplace_back("Cookie", cookie);
  }
  headers.swap(out);
}

// Clamp timeout.
inline int clampTimeoutMs(int ms) {
  if (ms < kMinTimeoutMs) return kMinTimeoutMs;
  if (ms > kMaxTimeoutMs) return kMaxTimeoutMs;
  return ms;
}

}  // namespace M4xNetPolicy
