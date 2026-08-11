#include "apps/M4xRegistry.h"

#include "apps/M4xPaths.h"

#include <ArduinoJson.h>
#include <SDCardManager.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace {

constexpr const char* kRegistryTmp = "/system/app_registry.json.tmp";
constexpr const char* kRegistryBak = "/system/app_registry.json.bak";

std::string readAllText(const char* path) {
  FsFile f;
  if (!SdMan.openFileForRead("M4xReg", path, f)) return {};
  const size_t n = f.fileSize();
  if (n > 256u * 1024u) {
    f.close();
    return {};
  }
  std::string out;
  out.resize(n);
  if (n > 0) {
    size_t off = 0;
    while (off < n) {
      const int r = f.read(reinterpret_cast<uint8_t*>(&out[off]), n - off);
      if (r <= 0) {
        out.clear();
        break;
      }
      off += static_cast<size_t>(r);
    }
    if (off != n) out.clear();
  }
  f.close();
  return out;
}

bool writeAllTextExact(const char* path, const std::string& body) {
  if (SdMan.exists(path)) SdMan.remove(path);
  FsFile f;
  if (!SdMan.openFileForWrite("M4xReg", path, f)) return false;
  const size_t n = body.size();
  size_t off = 0;
  while (off < n) {
    const size_t chunk = std::min<size_t>(4096, n - off);
    const int w = f.write(reinterpret_cast<const uint8_t*>(body.data() + off), chunk);
    if (w <= 0) {
      f.close();
      SdMan.remove(path);
      return false;
    }
    off += static_cast<size_t>(w);
  }
  f.close();
  return true;
}

// During crash recovery the install journal intentionally carries only the
// minimal old schema. Re-read the live manifest so a native package cannot be
// reconstructed as Lua merely because runtime/provider were introduced after
// that journal format. The live tree has already been promoted at this point.
void applyLiveManifestRuntime(const std::string& installPath, M4xRuntimeKind& runtime,
                              std::string& entry, std::string& provider) {
  std::string p = installPath;
  if (!p.empty() && p.back() != '/') p += '/';
  p += M4xPaths::kManifestName;
  const std::string raw = readAllText(p.c_str());
  if (raw.empty() || raw.size() > 32u * 1024u) return;
  const M4xManifest live = M4xParseManifest(raw.data(), raw.size());
  if (!live.valid) return;
  runtime = live.runtime;
  entry = live.entry;
  provider = live.provider;
}

bool parseRegistry(const std::string& raw, std::vector<M4xInstalledApp>& apps) {
  apps.clear();
  if (raw.empty()) return false;
  JsonDocument doc;
  if (deserializeJson(doc, raw)) return false;
  if (!doc["apps"].is<JsonArray>()) return false;

  for (JsonObject o : doc["apps"].as<JsonArray>()) {
    M4xInstalledApp a;
    a.id = o["id"] | "";
    a.name = o["name"] | "";
    a.version = o["version"] | "";
    a.versionCode = o["versionCode"] | 0;
    a.path = o["path"] | "";
    const std::string runtimeText = o["runtime"] | "lua";
    if (!M4xParseRuntimeKind(runtimeText, a.runtime)) a.runtime = M4xRuntimeKind::Lua;
    a.entry = o["entry"] | "";
    if (a.entry.empty()) a.entry = a.runtime == M4xRuntimeKind::Native ? "main.xml" : "main.lua";
    a.provider = o["provider"] | "";
    a.icon = o["icon"] | "";
    a.installedAt = o["installedAt"] | 0;
    if (o["permissions"].is<JsonArray>()) {
      for (JsonVariant v : o["permissions"].as<JsonArray>()) {
        if (v.is<const char*>()) a.permissions.emplace_back(v.as<const char*>());
      }
    }
    if (o["files"].is<JsonArray>()) {
      for (JsonVariant v : o["files"].as<JsonArray>()) {
        if (v.is<const char*>()) a.files.emplace_back(v.as<const char*>());
      }
    }
    if (!a.id.empty() && !a.path.empty()) apps.push_back(std::move(a));
  }
  return true;
}

}  // namespace

std::vector<M4xInstalledApp> M4xRegistry::load() {
  std::vector<M4xInstalledApp> apps;

  const std::string primary = readAllText(M4xPaths::kRegistryPath);
  if (parseRegistry(primary, apps)) return apps;

  const std::string bak = readAllText(kRegistryBak);
  if (parseRegistry(bak, apps)) {
    if (!bak.empty()) writeAllTextExact(M4xPaths::kRegistryPath, bak);
    return apps;
  }

  apps.clear();
  return apps;
}

bool M4xRegistry::save(const std::vector<M4xInstalledApp>& apps) {
  JsonDocument doc;
  JsonArray arr = doc["apps"].to<JsonArray>();
  for (const auto& a : apps) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = a.id;
    o["name"] = a.name;
    o["version"] = a.version;
    o["versionCode"] = a.versionCode;
    o["path"] = a.path;
    o["runtime"] = M4xRuntimeKey(a.runtime);
    o["entry"] = a.entry;
    o["provider"] = a.provider;
    o["icon"] = a.icon;
    o["installedAt"] = a.installedAt;
    JsonArray perms = o["permissions"].to<JsonArray>();
    for (const auto& p : a.permissions) perms.add(p);
    JsonArray files = o["files"].to<JsonArray>();
    for (const auto& f : a.files) files.add(f);
  }
  std::string out;
  serializeJson(doc, out);

  SdMan.mkdir("/system", true);

  if (!writeAllTextExact(kRegistryTmp, out)) return false;

  if (SdMan.exists(M4xPaths::kRegistryPath)) {
    if (SdMan.exists(kRegistryBak)) SdMan.remove(kRegistryBak);
    if (!SdMan.rename(M4xPaths::kRegistryPath, kRegistryBak)) {
      const std::string prev = readAllText(M4xPaths::kRegistryPath);
      if (!prev.empty()) writeAllTextExact(kRegistryBak, prev);
      SdMan.remove(M4xPaths::kRegistryPath);
    }
  }

  if (!SdMan.rename(kRegistryTmp, M4xPaths::kRegistryPath)) {
    if (!writeAllTextExact(M4xPaths::kRegistryPath, out)) {
      if (SdMan.exists(kRegistryBak)) SdMan.rename(kRegistryBak, M4xPaths::kRegistryPath);
      return false;
    }
    SdMan.remove(kRegistryTmp);
  }
  return true;
}

const M4xInstalledApp* M4xRegistry::find(const std::vector<M4xInstalledApp>& apps, const std::string& id) {
  for (const auto& a : apps) {
    if (a.id == id) return &a;
  }
  return nullptr;
}

void M4xRegistry::upsert(std::vector<M4xInstalledApp>& apps, const M4xManifest& m, const std::string& installPath,
                         uint32_t installedAt) {
  M4xRuntimeKind runtime = m.runtime;
  std::string entry = m.entry;
  std::string provider = m.provider;
  applyLiveManifestRuntime(installPath, runtime, entry, provider);
  if (entry.empty()) entry = runtime == M4xRuntimeKind::Native ? "main.xml" : "main.lua";

  std::vector<std::string> inv;
  inv.push_back("manifest.json");
  inv.push_back(entry);
  if (!m.icon.empty()) inv.push_back(m.icon);
  for (const auto& f : m.files) inv.push_back(f);

  for (auto& a : apps) {
    if (a.id == m.id) {
      a.name = m.name;
      a.version = m.version;
      a.versionCode = m.versionCode;
      a.path = installPath;
      a.runtime = runtime;
      a.entry = entry;
      a.provider = provider;
      a.icon = m.icon;
      a.permissions = m.permissions;
      a.files = inv;
      a.installedAt = installedAt;
      return;
    }
  }
  M4xInstalledApp a;
  a.id = m.id;
  a.name = m.name;
  a.version = m.version;
  a.versionCode = m.versionCode;
  a.path = installPath;
  a.runtime = runtime;
  a.entry = entry;
  a.provider = provider;
  a.icon = m.icon;
  a.permissions = m.permissions;
  a.files = inv;
  a.installedAt = installedAt;
  apps.push_back(std::move(a));
}

bool M4xRegistry::remove(std::vector<M4xInstalledApp>& apps, const std::string& id) {
  for (auto it = apps.begin(); it != apps.end(); ++it) {
    if (it->id == id) {
      apps.erase(it);
      return true;
    }
  }
  return false;
}
