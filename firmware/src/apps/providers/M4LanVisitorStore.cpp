#include "apps/providers/M4LanVisitorStore.h"

#include "apps/providers/M4NativeProviderIo.h"

#include <Arduino.h>
#include <SDCardManager.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>

namespace M4LanVisitorStore {
namespace {

std::mutex gMu;
std::vector<SsidVisitors> gCache;
bool gLoaded = false;
uint32_t gLastFlushMs = 0;
bool gDirty = false;

constexpr uint32_t kFlushMinIntervalMs = 3000;

void loadLocked() {
  if (gLoaded) return;
  gLoaded = true;
  gCache.clear();
  std::string raw;
  FsFile f;
  if (!SdMan.openFileForRead("LanVisitors", kStorePath, f)) return;
  char buf[128];
  while (f.available()) {
    const int n = f.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf) - 1);
    if (n <= 0) break;
    buf[n] = 0;
    raw.append(buf, static_cast<size_t>(n));
    if (raw.size() > 8u * 1024u) break;
  }
  f.close();
  (void)parseStore(raw, gCache);
}

bool flushLocked() {
  if (!gDirty) return true;
  const uint32_t now = millis();
  if (gLastFlushMs && now - gLastFlushMs < kFlushMinIntervalMs) return true;
  (void)M4NativeProviderIo::ensureParentDirs(kStorePath);
  const std::string body = serializeStore(gCache);
  FsFile f;
  if (!SdMan.openFileForWrite("LanVisitors", kStorePath, f)) return false;
  if (!body.empty()) {
    (void)f.write(reinterpret_cast<const uint8_t*>(body.data()), body.size());
  }
  f.sync();
  f.close();
  gDirty = false;
  gLastFlushMs = now;
  return true;
}

}  // namespace

void note(const char* ssid, const char* ip) {
  if (!ssid || !ssid[0] || !ipOk(ip)) return;
  // SSID must not inject TSV control characters.
  for (const char* p = ssid; *p; ++p) {
    if (*p == '\t' || *p == '\n' || *p == '\r') return;
  }

  std::lock_guard<std::mutex> lock(gMu);
  loadLocked();

  // Move/create SSID row to front.
  auto it = std::find_if(gCache.begin(), gCache.end(),
                         [&](const SsidVisitors& r) { return r.ssid == ssid; });
  SsidVisitors row;
  if (it != gCache.end()) {
    row = std::move(*it);
    gCache.erase(it);
  } else {
    row.ssid = ssid;
  }

  // Dedupe IP to front.
  row.ips.erase(std::remove(row.ips.begin(), row.ips.end(), std::string(ip)), row.ips.end());
  row.ips.insert(row.ips.begin(), std::string(ip));
  if (row.ips.size() > kMaxIpsPerSsid) row.ips.resize(kMaxIpsPerSsid);

  gCache.insert(gCache.begin(), std::move(row));
  if (gCache.size() > kMaxSsidRecords) gCache.resize(kMaxSsidRecords);
  gDirty = true;
  (void)flushLocked();
}

std::vector<std::string> visitorsFor(const char* ssid) {
  if (!ssid || !ssid[0]) return {};
  std::lock_guard<std::mutex> lock(gMu);
  loadLocked();
  for (const auto& row : gCache) {
    if (row.ssid == ssid) return row.ips;
  }
  return {};
}

}  // namespace M4LanVisitorStore
