#pragma once

#include "apps/M4xInstaller.h"
#include "apps/M4xPaths.h"
#include "apps/M4xRegistry.h"

#include <SDCardManager.h>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

// Persistent four-slot home launcher. Empty slots are filled with the first
// installed apps that are not already pinned, preserving the old auto-fill
// behavior while making an explicit pin stable across registry reloads.
namespace M4HomeDock {

inline constexpr int kSlotCount = 4;
inline constexpr const char* kPath = "/system/home_dock.cfg";

inline std::array<std::string, kSlotCount> load() {
  std::array<std::string, kSlotCount> slots{};
  FsFile file;
  if (!SdMan.openFileForRead("HomeDock", kPath, file)) return slots;

  std::string line;
  int slot = 0;
  while (file.available() && slot < kSlotCount) {
    const int c = file.read();
    if (c < 0) break;
    if (c == '\n' || c == '\r') {
      if (!line.empty()) slots[slot++] = line;
      line.clear();
    } else if (line.size() < 63) {
      line.push_back(static_cast<char>(c));
    }
  }
  if (!line.empty() && slot < kSlotCount) slots[slot] = line;
  file.close();
  return slots;
}

inline bool save(const std::array<std::string, kSlotCount>& slots) {
  M4xInstaller::ensureLayout();
  String content;
  for (const auto& id : slots) {
    if (!id.empty()) content += id.c_str();
    content += '\n';
  }
  return SdMan.writeFile(kPath, content);
}

inline bool setSlot(int slot, const std::string& appId) {
  if (slot < 0 || slot >= kSlotCount || appId.empty()) return false;
  auto slots = load();
  // A pin is unique: moving an app to a new slot vacates its old slot.
  for (auto& id : slots) {
    if (id == appId) id.clear();
  }
  slots[static_cast<size_t>(slot)] = appId;
  return save(slots);
}

inline bool clear(const std::string& appId) {
  if (appId.empty()) return false;
  auto slots = load();
  bool changed = false;
  for (auto& id : slots) {
    if (id == appId) {
      id.clear();
      changed = true;
    }
  }
  return !changed || save(slots);
}

// Built-in destinations are virtual dock tiles, not installed .m4x packages.
inline bool isBuiltin(const std::string& id) { return id.compare(0, 8, "builtin.") == 0; }
inline M4xInstalledApp builtin(const std::string& id) {
  M4xInstalledApp app;
  app.id = id;
  if (id == "builtin.files") app.name = "文件管理";
  else if (id == "builtin.transfer") app.name = "传书";
  else if (id == "builtin.store") app.name = "应用商店";
  else if (id == "builtin.settings") app.name = "设置";
  else if (id == "builtin.history") app.name = "阅读历史";
  else if (id == "builtin.network") app.name = "网络";
  else if (id == "builtin.bookmarks") app.name = "书签";
  else if (id == "builtin.opds") app.name = "OPDS";
  else if (id == "builtin.jianguo") app.name = "坚果云";
  else if (id == "builtin.datacapsule") app.name = "数据胶囊";
  else app.id.clear();
  app.icon = id;
  app.runtime = M4xRuntimeKind::Native;
  return app;
}

inline std::vector<M4xInstalledApp> orderedApps(const std::vector<M4xInstalledApp>& apps) {
  const auto pinned = load();
  std::array<M4xInstalledApp, kSlotCount> slots{};
  std::vector<std::string> seen;
  auto make = [&](const std::string& id) -> M4xInstalledApp {
    for (const auto& app : apps) if (app.id == id) return app;
    return builtin(id);
  };
  auto place = [&](int slot, const std::string& id) {
    if (id.empty() || std::find(seen.begin(), seen.end(), id) != seen.end()) return false;
    M4xInstalledApp app = make(id);
    if (app.id.empty()) return false;
    slots[static_cast<size_t>(slot)] = std::move(app);
    seen.push_back(id);
    return true;
  };
  // Honor exact pinned positions before assigning any defaults.
  for (int i = 0; i < kSlotCount; ++i) place(i, pinned[static_cast<size_t>(i)]);
  const std::array<const char*, kSlotCount> defaults = {
      "builtin.files", "builtin.transfer", "builtin.store", "builtin.settings"};
  for (const char* id : defaults) {
    for (int i = 0; i < kSlotCount; ++i) {
      if (slots[static_cast<size_t>(i)].id.empty() && place(i, id)) break;
    }
  }
  std::vector<M4xInstalledApp> ordered;
  ordered.reserve(kSlotCount);
  for (auto& slot : slots) if (!slot.id.empty()) ordered.push_back(std::move(slot));
  return ordered;
}

}  // namespace M4HomeDock
