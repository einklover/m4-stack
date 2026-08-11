#include "apps/M4xManifest.h"

#include "apps/M4xPathSafe.h"

#include <ArduinoJson.h>
#include <cctype>
#include <cstring>

const char* M4xRuntimeKey(M4xRuntimeKind runtime) {
  return runtime == M4xRuntimeKind::Native ? "native" : "lua";
}

bool M4xParseRuntimeKind(const std::string& value, M4xRuntimeKind& out) {
  if (value.empty() || value == "lua") {
    out = M4xRuntimeKind::Lua;
    return true;
  }
  if (value == "native") {
    out = M4xRuntimeKind::Native;
    return true;
  }
  return false;
}

bool M4xIsValidPackageId(const std::string& id) {
  if (id.size() < 3 || id.size() > 64) return false;
  bool hasDot = false;
  for (size_t i = 0; i < id.size(); ++i) {
    const char c = id[i];
    if (c == '.') {
      hasDot = true;
      if (i == 0 || i + 1 == id.size()) return false;
      continue;
    }
    if (!(std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '_' ||
          c == '-')) {
      return false;
    }
  }
  return hasDot;
}

bool M4xIsAllowedPermission(const std::string& perm) {
  static const char* kAllowed[] = {
      "display", "input", "filesystem.appdata", "filesystem.sd_read", "network", "settings.read",
  };
  for (const char* a : kAllowed) {
    if (perm == a) return true;
  }
  return false;
}

M4xManifest M4xParseManifest(const char* json, size_t len) {
  M4xManifest m;
  if (!json || len == 0) {
    m.error = "empty_manifest";
    return m;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, json, len);
  if (err) {
    m.error = std::string("json:") + err.c_str();
    return m;
  }

  m.id = doc["id"] | "";
  m.name = doc["name"] | "";
  m.version = doc["version"] | "0.0.0";
  m.versionCode = doc["versionCode"] | 0;
  m.minFirmware = doc["minFirmware"] | "";
  m.author = doc["author"] | "";
  const std::string runtimeText = doc["runtime"] | "lua";
  if (!M4xParseRuntimeKind(runtimeText, m.runtime)) {
    m.error = "invalid_runtime";
    return m;
  }
  m.entry = doc["entry"] | "";
  if (m.entry.empty()) m.entry = m.runtime == M4xRuntimeKind::Native ? "main.xml" : "main.lua";
  m.provider = doc["provider"] | "";
  m.icon = doc["icon"] | "";
  m.description = doc["description"] | "";

  if (doc["permissions"].is<JsonArray>()) {
    for (JsonVariant v : doc["permissions"].as<JsonArray>()) {
      if (v.is<const char*>()) m.permissions.emplace_back(v.as<const char*>());
    }
  }
  if (doc["files"].is<JsonArray>()) {
    for (JsonVariant v : doc["files"].as<JsonArray>()) {
      if (v.is<const char*>()) m.files.emplace_back(v.as<const char*>());
    }
  }

  if (!M4xIsValidPackageId(m.id)) {
    m.error = "invalid_id";
    return m;
  }
  if (m.name.empty()) {
    m.error = "missing_name";
    return m;
  }
  if (m.versionCode <= 0) {
    m.error = "invalid_versionCode";
    return m;
  }
  if (m.runtime == M4xRuntimeKind::Native && m.provider.size() > 32) {
    m.error = "invalid_provider";
    return m;
  }
  for (unsigned char c : m.provider) {
    if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.')) {
      m.error = "invalid_provider";
      return m;
    }
  }

  // Path safety for every package-relative path the installer may write.
  {
    const std::string e = M4xPathSafe::validatePackageRelPath(m.entry);
    if (!e.empty()) {
      m.error = std::string("bad_entry:") + e;
      return m;
    }
  }
  if (!m.icon.empty()) {
    const std::string e = M4xPathSafe::validatePackageRelPath(m.icon);
    if (!e.empty()) {
      m.error = std::string("bad_icon:") + e;
      return m;
    }
  }
  if (m.files.size() > M4xPathSafe::kMaxPackageFiles) {
    m.error = "too_many_files";
    return m;
  }
  for (const auto& f : m.files) {
    const std::string e = M4xPathSafe::validatePackageRelPath(f);
    if (!e.empty()) {
      m.error = std::string("bad_file:") + e + ":" + f;
      return m;
    }
  }
  {
    const auto plan = M4xPathSafe::makeExtractList(m.entry, m.icon, m.files);
    if (!plan.ok) {
      m.error = plan.error.empty() ? "bad_extract_list" : plan.error;
      return m;
    }
  }

  for (const auto& p : m.permissions) {
    if (!M4xIsAllowedPermission(p)) {
      m.error = std::string("bad_permission:") + p;
      return m;
    }
  }

  auto ensure = [&](const char* p) {
    for (const auto& x : m.permissions) {
      if (x == p) return;
    }
    m.permissions.emplace_back(p);
  };
  ensure("display");
  ensure("input");

  m.valid = true;
  return m;
}
