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

inline std::vector<M4xInstalledApp> orderedApps(const std::vector<M4xInstalledApp>& apps) {
  const auto slots = load();
  std::vector<M4xInstalledApp> ordered;
  ordered.reserve(std::min<size_t>(apps.size(), kSlotCount));
  std::vector<bool> used(apps.size(), false);

  auto appendId = [&](const std::string& id) {
    if (id.empty() || ordered.size() >= kSlotCount) return;
    for (size_t i = 0; i < apps.size(); ++i) {
      if (!used[i] && apps[i].id == id) {
        ordered.push_back(apps[i]);
        used[i] = true;
        return;
      }
    }
  };
  for (const auto& id : slots) appendId(id);
  for (size_t i = 0; i < apps.size() && ordered.size() < kSlotCount; ++i) {
    if (!used[i]) ordered.push_back(apps[i]);
  }
  return ordered;
}

}  // namespace M4HomeDock
