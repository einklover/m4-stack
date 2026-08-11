#pragma once

// Installable extension (.m4x) layout on SD card — APK-like model.

namespace M4xPaths {

// Installed app payload (immutable after install except upgrades).
inline constexpr const char* kAppsRoot = "/apps";
// Per-app private data (writable by that app only).
inline constexpr const char* kAppsDataRoot = "/apps_data";
// Drop-in install packages waiting for user confirm.
inline constexpr const char* kInbox = "/apps_inbox";
// Registry of installed packages.
inline constexpr const char* kRegistryPath = "/system/app_registry.json";
// Package extension (zip container).
inline constexpr const char* kPackageExt = ".m4x";

inline constexpr const char* kManifestName = "manifest.json";
inline constexpr const char* kEntryDefault = "main.lua";

}  // namespace M4xPaths
