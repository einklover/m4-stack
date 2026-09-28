#include "apps/M4xRegistry.h"

#include "apps/M4xPaths.h"

#include <ArduinoJson.h>
#include <SDCardManager.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>

namespace {

constexpr const char* kRegistryTmp = "/system/app_registry.json.tmp";
constexpr const char* kRegistryBak = "/system/app_registry.json.bak";

// Independent of installGate. Install already holds that non-recursive mutex
// across load/save, so taking it again here would deadlock. This mutex is the
// only registry transaction lock: load (including bak write-back) and save
// (primary->bak and tmp->primary) both hold it for the whole call. load must
// not call save.
std::mutex& registryTxnMu() {
  static std::mutex mu;
  return mu;
}

enum class RegistryReadKind { Missing, Ok, IoError };

struct RegistryRead {
  RegistryReadKind kind;
  std::string text;
};

// Missing means the path is not on the card. IoError means it is present but
// this read did not return the full file (open, size, or short read). Callers
// that repair a registry must not delete or replace an IoError primary.
RegistryRead readRegistryFile(const char* path) {
  FsFile f;
  if (!SdMan.openFileForRead("M4xReg", path, f)) {
    return {SdMan.exists(path) ? RegistryReadKind::IoError : RegistryReadKind::Missing, {}};
  }
  const size_t n = f.fileSize();
  if (n > 256u * 1024u) {
    f.close();
    return {RegistryReadKind::IoError, {}};
  }
  std::string out;
  out.resize(n);
  if (n > 0) {
    size_t off = 0;
    while (off < n) {
      const int r = f.read(reinterpret_cast<uint8_t*>(&out[off]), n - off);
      if (r <= 0) {
        f.close();
        return {RegistryReadKind::IoError, {}};
      }
      off += static_cast<size_t>(r);
    }
    if (off != n) {
      f.close();
      return {RegistryReadKind::IoError, {}};
    }
  }
  f.close();
  return {RegistryReadKind::Ok, std::move(out)};
}

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

// Create-only. openFileForWrite uses O_TRUNC, so an existing path is refused
// and only a file this call created is removed on failure.
bool writeAllTextExact(const char* path, const std::string& body) {
  if (SdMan.exists(path)) return false;
  FsFile f;
  if (!SdMan.openFileForWrite("M4xReg", path, f)) return false;
  const size_t n = body.size();
  size_t off = 0;
  bool ok = true;
  while (ok && off < n) {
    const size_t chunk = std::min<size_t>(4096, n - off);
    const int w = f.write(reinterpret_cast<const uint8_t*>(body.data() + off), chunk);
    if (w <= 0) ok = false;
    else off += static_cast<size_t>(w);
  }
  if (ok && (off != n || f.getWriteError())) ok = false;
  if (ok && f.fileSize() != n) ok = false;
  if (ok && !f.sync()) ok = false;
  if (!f.close()) ok = false;
  if (!ok) {
    SdMan.remove(path);
    return false;
  }
  FsFile verify;
  if (!SdMan.openFileForRead("M4xReg", path, verify)) {
    SdMan.remove(path);
    return false;
  }
  const bool sizeOk = verify.fileSize() == n;
  verify.close();
  if (!sizeOk) {
    SdMan.remove(path);
    return false;
  }
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

enum class RegistryParse { Ok, Corrupt, Transient };

RegistryParse parseRegistry(const std::string& raw, std::vector<M4xInstalledApp>& apps) {
  apps.clear();
  if (raw.empty()) return RegistryParse::Corrupt;
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, raw);
  if (err == DeserializationError::NoMemory || err == DeserializationError::TooDeep) {
    return RegistryParse::Transient;
  }
  if (err) return RegistryParse::Corrupt;
  if (doc.overflowed()) return RegistryParse::Transient;
  if (!doc["apps"].is<JsonArray>()) return RegistryParse::Corrupt;

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
  return RegistryParse::Ok;
}

struct RegistryLoadResult {
  bool failClosed = false;
  std::vector<M4xInstalledApp> apps;
};

// strictWrite is the modify/save path. IoError and Transient must not substitute
// the backup or an empty table. Confirmed-corrupt and missing still repair from bak.
RegistryLoadResult loadRegistryUnlocked(bool strictWrite) {
  RegistryLoadResult out;
  const RegistryRead primary = readRegistryFile(M4xPaths::kRegistryPath);
  if (primary.kind == RegistryReadKind::Ok) {
    const RegistryParse parsed = parseRegistry(primary.text, out.apps);
    if (parsed == RegistryParse::Ok) return out;
    if (parsed == RegistryParse::Transient) {
      out.apps.clear();
      if (strictWrite) {
        out.failClosed = true;
        return out;
      }
      const RegistryRead bak = readRegistryFile(kRegistryBak);
      if (bak.kind == RegistryReadKind::Ok &&
          parseRegistry(bak.text, out.apps) == RegistryParse::Ok) {
        return out;
      }
      out.apps.clear();
      return out;
    }
    out.apps.clear();
  } else if (primary.kind == RegistryReadKind::IoError) {
    out.apps.clear();
    if (strictWrite) {
      out.failClosed = true;
      return out;
    }
    const RegistryRead bak = readRegistryFile(kRegistryBak);
    if (bak.kind == RegistryReadKind::Ok &&
        parseRegistry(bak.text, out.apps) == RegistryParse::Ok) {
      return out;
    }
    out.apps.clear();
    return out;
  }

  // Primary is absent or confirmed corrupt. A readable backup may replace it.
  const RegistryRead bak = readRegistryFile(kRegistryBak);
  if (bak.kind == RegistryReadKind::Ok && parseRegistry(bak.text, out.apps) == RegistryParse::Ok) {
    if (!bak.text.empty()) {
      if (SdMan.exists(M4xPaths::kRegistryPath)) SdMan.remove(M4xPaths::kRegistryPath);
      if (!SdMan.exists(M4xPaths::kRegistryPath)) {
        (void)writeAllTextExact(M4xPaths::kRegistryPath, bak.text);
      }
    }
    return out;
  }

  out.apps.clear();
  return out;
}

}  // namespace

std::vector<M4xInstalledApp> M4xRegistry::load() {
  std::lock_guard<std::mutex> registryTxnLock(registryTxnMu());
  return loadRegistryUnlocked(false).apps;
}

bool M4xRegistry::tryLoad(std::vector<M4xInstalledApp>& apps) {
  std::lock_guard<std::mutex> registryTxnLock(registryTxnMu());
  RegistryLoadResult loaded = loadRegistryUnlocked(true);
  if (loaded.failClosed) {
    apps.clear();
    return false;
  }
  apps = std::move(loaded.apps);
  return true;
}

bool M4xRegistry::save(const std::vector<M4xInstalledApp>& apps) {
  std::lock_guard<std::mutex> registryTxnLock(registryTxnMu());
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
  if (doc.overflowed()) return false;
  std::string out;
  const size_t written = serializeJson(doc, out);
  if (doc.overflowed() || written == 0 || written != out.size()) return false;

  SdMan.mkdir("/system", true);

  if (SdMan.exists(kRegistryTmp) && !SdMan.remove(kRegistryTmp)) return false;
  if (!writeAllTextExact(kRegistryTmp, out)) return false;

  const bool hadPrimary = SdMan.exists(M4xPaths::kRegistryPath);
  if (hadPrimary) {
    if (SdMan.exists(kRegistryBak) && !SdMan.remove(kRegistryBak)) {
      SdMan.remove(kRegistryTmp);
      return false;
    }
    if (!SdMan.rename(M4xPaths::kRegistryPath, kRegistryBak)) {
      const std::string prev = readAllText(M4xPaths::kRegistryPath);
      if (prev.empty() || !writeAllTextExact(kRegistryBak, prev)) {
        SdMan.remove(kRegistryTmp);
        return false;
      }
      const std::string bakCheck = readAllText(kRegistryBak);
      if (bakCheck != prev || !SdMan.remove(M4xPaths::kRegistryPath)) {
        SdMan.remove(kRegistryTmp);
        return false;
      }
    }
  }

  if (!SdMan.rename(kRegistryTmp, M4xPaths::kRegistryPath)) {
    if (SdMan.exists(M4xPaths::kRegistryPath)) {
      SdMan.remove(kRegistryTmp);
      return false;
    }
    if (!writeAllTextExact(M4xPaths::kRegistryPath, out)) {
      if (SdMan.exists(M4xPaths::kRegistryPath)) SdMan.remove(M4xPaths::kRegistryPath);
      if (hadPrimary && SdMan.exists(kRegistryBak)) SdMan.rename(kRegistryBak, M4xPaths::kRegistryPath);
      SdMan.remove(kRegistryTmp);
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
