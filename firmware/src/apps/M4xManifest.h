#pragma once

#include <cstddef>
#include <string>
#include <vector>

enum class M4xRuntimeKind {
  Lua = 0,
  Native = 1,
};

const char* M4xRuntimeKey(M4xRuntimeKind runtime);
bool M4xParseRuntimeKind(const std::string& value, M4xRuntimeKind& out);

// Parsed install-package manifest (manifest.json inside .m4x).
struct M4xManifest {
  std::string id;           // unique package id, e.g. com.example.clock
  std::string name;         // display name
  std::string version;      // human version string
  int versionCode = 0;      // integer for upgrade compare
  std::string minFirmware;  // optional
  std::string author;
  M4xRuntimeKind runtime = M4xRuntimeKind::Lua;
  // Runtime entry. Lua defaults to main.lua; native apps default to main.xml.
  std::string entry;
  // Optional built-in native provider adapter id (fanqie/weread/jjwxc/...).
  // It is a capability binding, never executable package code.
  std::string provider;
  std::string icon;  // relative path inside package
  std::string description;
  std::vector<std::string> permissions;
  // Explicit extra package files the installer may extract (assets/config).
  // Always extracted in addition: manifest.json, entry, optional icon.
  std::vector<std::string> files;

  bool valid = false;
  std::string error;
};

// Parse JSON text into manifest; sets valid/error.
// Also validates entry/icon/files package-relative paths (M4xPathSafe).
M4xManifest M4xParseManifest(const char* json, size_t len);

// Validate package id: reverse-DNS-ish [a-z0-9_.-] with at least one dot.
bool M4xIsValidPackageId(const std::string& id);

// Permission is in the system allow-list.
bool M4xIsAllowedPermission(const std::string& perm);
