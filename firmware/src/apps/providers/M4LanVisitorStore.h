#pragma once

// Records LAN client IPs that have contacted this device's Wi-Fi transfer HTTP
// server, keyed by the current STA SSID. Used as a cheap discovery seed for
// phone-hosted services (Legado web, etc.) on the same Wi-Fi: the phone usually
// opens the transfer page from the browser, so its IP shows up here without any
// extra user action.
//
// Storage is a small TSV on SD (newest first, bounded). Pure header helpers are
// host-testable; SD load/save live in the .cpp.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace M4LanVisitorStore {

inline constexpr size_t kMaxIpsPerSsid = 8;
inline constexpr size_t kMaxSsidRecords = 8;
// Absolute path under the device data root (no SDCardManager dependency here).
inline constexpr const char* kStorePath = "/.crosspoint/lan_visitors.tsv";

// One SSID's visitor list (IPs as dotted-decimal strings, newest first).
struct SsidVisitors {
  std::string ssid;
  std::vector<std::string> ips;
};

// Record that `ip` talked to us while associated to `ssid`. Empty args ignored.
// Safe to call from WebServer handlers (throttled SD writes).
void note(const char* ssid, const char* ip);

// Return visitor IPs for `ssid` (newest first). Empty if unknown / SD missing.
std::vector<std::string> visitorsFor(const char* ssid);

// IPv4 dotted-decimal sanity (rejects 0.0.0.0 and non-v4 shapes).
inline bool ipOk(const char* ip) {
  if (!ip || !ip[0]) return false;
  size_t dots = 0;
  size_t len = 0;
  for (const char* p = ip; *p; ++p, ++len) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (!(c == '.' || (c >= '0' && c <= '9'))) return false;
    if (c == '.') ++dots;
    if (len > 15) return false;
  }
  if (dots != 3 || len < 7) return false;
  if (std::strcmp(ip, "0.0.0.0") == 0 || std::strcmp(ip, "255.255.255.255") == 0) return false;
  if (std::strncmp(ip, "127.", 4) == 0) return false;
  return true;
}

inline bool parseStore(const std::string& raw, std::vector<SsidVisitors>& out) {
  out.clear();
  size_t i = 0;
  while (i < raw.size()) {
    size_t nl = raw.find('\n', i);
    if (nl == std::string::npos) nl = raw.size();
    std::string line = raw.substr(i, nl - i);
    i = nl + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    const size_t tab = line.find('\t');
    if (tab == std::string::npos || tab == 0) continue;
    SsidVisitors row;
    row.ssid = line.substr(0, tab);
    const std::string rest = line.substr(tab + 1);
    size_t p = 0;
    while (p < rest.size() && row.ips.size() < kMaxIpsPerSsid) {
      size_t c = rest.find(',', p);
      if (c == std::string::npos) c = rest.size();
      std::string ip = rest.substr(p, c - p);
      while (!ip.empty() && ip.front() == ' ') ip.erase(ip.begin());
      while (!ip.empty() && ip.back() == ' ') ip.pop_back();
      if (ipOk(ip.c_str())) row.ips.push_back(ip);
      p = c + 1;
    }
    if (!row.ips.empty()) out.push_back(std::move(row));
    if (out.size() >= kMaxSsidRecords) break;
  }
  return true;
}

inline std::string serializeStore(const std::vector<SsidVisitors>& rows) {
  std::string out;
  out.reserve(256);
  size_t n = 0;
  for (const auto& row : rows) {
    if (row.ssid.empty() || row.ips.empty()) continue;
    if (n >= kMaxSsidRecords) break;
    out += row.ssid;
    out += '\t';
    for (size_t i = 0; i < row.ips.size() && i < kMaxIpsPerSsid; ++i) {
      if (i) out += ',';
      out += row.ips[i];
    }
    out += '\n';
    ++n;
  }
  return out;
}

}  // namespace M4LanVisitorStore
