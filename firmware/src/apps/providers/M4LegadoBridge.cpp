#include "apps/providers/M4LegadoBridge.h"

#include "apps/providers/M4LanVisitorStore.h"
#include "apps/providers/M4NativeProviderHttp.h"
#include "apps/providers/M4NativeProviderIo.h"
#include "apps/providers/M4NativeWifi.h"

#include <Arduino.h>
#include <SDCardManager.h>
#include <WiFi.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <vector>

namespace M4LegadoBridge {
namespace {

std::mutex gMu;
std::string gBase;  // http://ip:port, no trailing slash

std::string trimTrailingSlash(std::string s) {
  while (!s.empty() && s.back() == '/') s.pop_back();
  return s;
}

bool readFileSmall(const std::string& path, std::string& out, size_t cap = 256) {
  out.clear();
  FsFile f;
  if (!SdMan.openFileForRead("LegadoEp", path.c_str(), f)) return false;
  char buf[64];
  while (f.available() && out.size() < cap) {
    const size_t want = std::min(sizeof(buf) - 1, cap - out.size());
    const int n = f.read(reinterpret_cast<uint8_t*>(buf), want);
    if (n <= 0) break;
    out.append(buf, static_cast<size_t>(n));
  }
  f.close();
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
  return !out.empty();
}

bool writeFileSmall(const std::string& path, const std::string& body) {
  (void)M4NativeProviderIo::ensureParentDirs(path);
  FsFile f;
  if (!SdMan.openFileForWrite("LegadoEp", path.c_str(), f)) return false;
  if (!body.empty()) {
    (void)f.write(reinterpret_cast<const uint8_t*>(body.data()), body.size());
  }
  f.sync();
  f.close();
  return true;
}

// Probe one absolute URL. Accepts a real JSON bookshelf OR a too-large 200
// (bookshelf can exceed the probe hard cap).
bool probeUrl(const std::string& url) {
  M4NativeProviderHttp::Request req;
  req.method = "GET";
  req.url = url;
  req.timeoutMs = 2500;
  req.maxBytes = 8u * 1024u;
  req.headers = {{"User-Agent", "Mozilla/5.0 Murphy-M4 LegadoProbe/1"}, {"Connection", "close"}};
  std::string body;
  M4NativeProviderHttp::Result net;
  const bool ok = M4NativeProviderHttp::requestSmall(req, body, net, req.maxBytes, {});
  if (ok) return probeBodyLooksLikeLegado(body, net.status, net.error);
  // Full bookshelf often exceeds the probe cap: treat capped 2xx as a hit.
  if (net.status >= 200 && net.status < 300 &&
      (net.error == "response_too_large" || net.error == "sink_failed")) {
    return true;
  }
  return probeBodyLooksLikeLegado(body, net.status, net.error);
}

bool probeBase(const std::string& base) {
  if (!baseUrlOk(base)) return false;
  for (size_t i = 0; i < kProbePathCount; ++i) {
    if (probeUrl(base + kProbePaths[i])) return true;
  }
  return false;
}

}  // namespace

std::string baseUrl() {
  std::lock_guard<std::mutex> lock(gMu);
  return gBase.empty() ? std::string(kDefaultBase) : gBase;
}

void setBaseUrl(const std::string& appDataRoot, const std::string& base) {
  const std::string cleaned = trimTrailingSlash(base);
  if (!baseUrlOk(cleaned)) return;
  {
    std::lock_guard<std::mutex> lock(gMu);
    gBase = cleaned;
  }
  if (!appDataRoot.empty()) {
    (void)writeFileSmall(endpointPath(appDataRoot), cleaned);
  }
  Serial.printf("[Legado] endpoint set %s\n", cleaned.c_str());
}

std::string loadSavedBase(const std::string& appDataRoot) {
  if (appDataRoot.empty()) return {};
  std::string raw;
  if (!readFileSmall(endpointPath(appDataRoot), raw)) return {};
  raw = trimTrailingSlash(raw);
  return baseUrlOk(raw) ? raw : std::string();
}

bool ensureEndpoint(const std::string& appDataRoot) {
  // 1) In-memory base still good?
  {
    std::lock_guard<std::mutex> lock(gMu);
    if (!gBase.empty() && baseUrlOk(gBase)) {
      // Keep using it; cheap re-probe only when discovery later fails.
      return true;
    }
  }

  // 2) Saved endpoint — verify with a short probe.
  const std::string saved = loadSavedBase(appDataRoot);
  if (!saved.empty()) {
    Serial.printf("[Legado] probing saved %s\n", saved.c_str());
    if (probeBase(saved)) {
      std::lock_guard<std::mutex> lock(gMu);
      gBase = saved;
      return true;
    }
  }

  // 3) Need Wi-Fi for discovery.
  const auto wifi = M4NativeWifi::ensureConnected(15000u, {});
  if (!wifi.ok) {
    Serial.printf("[Legado] endpoint discover: wifi not ready\n");
    return false;
  }

  const String ssid = WiFi.SSID();
  std::vector<std::string> ips = M4LanVisitorStore::visitorsFor(ssid.c_str());

  // Always try the previous saved host IP first even if not in visitor list
  // (phone kept the same IP but transfer page was not reopened this session).
  if (!saved.empty() && saved.rfind("http://", 0) == 0) {
    const std::string hostport = saved.substr(7);
    const size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) {
      const std::string ip = hostport.substr(0, colon);
      if (M4LanVisitorStore::ipOk(ip.c_str())) {
        ips.erase(std::remove(ips.begin(), ips.end(), ip), ips.end());
        ips.insert(ips.begin(), ip);
      }
    }
  }

  if (ips.empty()) {
    Serial.printf("[Legado] endpoint discover: no visitor IPs for SSID '%s'\n",
                  ssid.c_str());
    // Last resort: keep compile-time default if it answers (dev bench).
    if (probeBase(kDefaultBase)) {
      setBaseUrl(appDataRoot, kDefaultBase);
      return true;
    }
    return false;
  }

  Serial.printf("[Legado] endpoint discover: %u visitor IP(s) on '%s'\n",
                static_cast<unsigned>(ips.size()), ssid.c_str());

  for (const auto& ip : ips) {
    for (size_t pi = 0; pi < kProbePortCount; ++pi) {
      const std::string base = makeBase(ip, kProbePorts[pi]);
      if (base.empty()) continue;
      Serial.printf("[Legado] probe %s\n", base.c_str());
      if (probeBase(base)) {
        setBaseUrl(appDataRoot, base);
        return true;
      }
    }
  }

  Serial.printf("[Legado] endpoint discover: no service found\n");
  return false;
}

std::string readLocator(const std::string& appDataRoot, const std::string& bookId) {
  const std::string path = sidecarPath(appDataRoot);
  if (path.empty() || bookId.empty()) return {};
  FsFile f;
  if (!SdMan.openFileForRead("LegadoSidecarRead", path.c_str(), f)) return {};
  std::string line;
  char buf[96];
  while (f.available()) {
    const int n = f.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf) - 1);
    if (n <= 0) break;
    buf[n] = 0;
    line.append(buf, static_cast<size_t>(n));
    size_t nl = 0;
    while ((nl = line.find('\n')) != std::string::npos) {
      const std::string row = line.substr(0, nl);
      line.erase(0, nl + 1);
      const size_t tab = row.find('\t');
      if (tab != std::string::npos && row.compare(0, tab, bookId) == 0) {
        f.close();
        return row.substr(tab + 1);
      }
      if (row.size() > 1024) line.clear();
    }
    if (line.size() > 4096) line.clear();
  }
  f.close();
  return {};
}

}  // namespace M4LegadoBridge
