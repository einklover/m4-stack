#pragma once

#include "apps/M4xRegistry.h"
#include "ui/pages/HomeSceneModel.h"

#include <cstdint>
#include <string>
#include <vector>

// PSRAM copies of the last home publication and drawer inventory. Returning
// to either screen paints from these and only submits another frame when the
// freshly loaded content differs.
namespace M4ReturnCache {

bool seedHome(HomeScene::HomeSceneModel& model);
void rememberHome(const HomeScene::HomeScenePublication& publication);
bool homeMatches(const HomeScene::HomeScenePublication& publication);

struct DrawerItem {
  bool plugin = false;
  uint8_t builtin = 0;
  int appIndex = -1;
  std::string id;
  std::string label;
  int icon = 0;
  std::vector<uint8_t> pluginIcon;
};

bool copyDrawerIcon(const std::string& id, int versionCode, const std::string& path, const std::string& icon,
                    uint32_t installedAt, std::vector<uint8_t>& out);
bool restoreDrawer(std::vector<M4xInstalledApp>& apps, std::vector<DrawerItem>& items);
void rememberDrawer(const std::vector<M4xInstalledApp>& apps, const std::vector<DrawerItem>& items);

}  // namespace M4ReturnCache
