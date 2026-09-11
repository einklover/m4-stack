#pragma once

#include "../ActivityWithSubactivity.h"
#include "apps/M4xRegistry.h"
#include "components/themes/BaseTheme.h"
#include "util/TouchHitGeometry.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// Phone-style app drawer for built-in M4 destinations and installed extensions.
class AppListActivity final : public ActivityWithSubactivity {
 public:
  struct Callbacks {
    std::function<void()> onSettingsOpen;
    std::function<void()> onFileManagerOpen;
    std::function<void()> onRecentBooksOpen;
    std::function<void()> onOpdsOpen;
    std::function<void()> onJianGuoOpen;
    std::function<void()> onDataCapsuleOpen;
    std::function<void()> onBookmarkNotesOpen;
    std::function<void()> onNetworkOpen;
  };

  explicit AppListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                           const std::function<void()>& onGoBack, Callbacks callbacks = {})
      : ActivityWithSubactivity("AppList", renderer, mappedInput),
        onGoBack(onGoBack),
        callbacks_(std::move(callbacks)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool showTouchNavigation() const override { return false; }
  uint8_t touchFooterButtonsMask() const override {
    return mode_ == 1 ? M4FooterTouchPolicy::Back | M4FooterTouchPolicy::Confirm
                      : M4FooterTouchPolicy::Back | M4FooterTouchPolicy::Confirm |
                            M4FooterTouchPolicy::Right |
                            (selectedIsPlugin() ? M4FooterTouchPolicy::Left : 0);
  }

 private:
  enum class BuiltinAction : uint8_t {
    FileManager,
    RecentBooks,
    Opds,
    JianGuo,
    DataCapsule,
    BookmarkNotes,
    Network,
    Settings,
  };

  struct DrawerItem {
    bool plugin = false;
    BuiltinAction builtin = BuiltinAction::Settings;
    int appIndex = -1;
    std::string id;
    std::string label;
    UIIcon icon = UIIcon::Library;
    std::vector<uint8_t> pluginIcon;
  };

  // Phase 1 (INV-R1): dirty-frame snapshot. Indices alone are insufficient
  // because reload() replaces items_/apps_ wholesale (torn-drawer hazard);
  // the display task deep-copies the dirty frame under the local mutex and
  // render() consumes it under the global guard. The drawer is small, so a
  // dirty-only deep copy beats a shared-pointer refactor here.
  struct AppListFrameSnapshot {
    int selectedIndex = 0;
    int mode = 0;
    bool uninstallClearData = true;
    std::vector<DrawerItem> items;
    std::vector<M4xInstalledApp> apps;
  };

  std::function<void()> onGoBack;
  Callbacks callbacks_;
  std::vector<M4xInstalledApp> apps_;
  std::vector<DrawerItem> items_;
  int selectedIndex_ = 0;
  bool updateRequired_ = false;
  // 0 = list mode, 1 = confirm uninstall
  int mode_ = 0;
  bool uninstallClearData_ = true;

  TaskHandle_t displayTaskHandle_ = nullptr;
  SemaphoreHandle_t renderingMutex_ = nullptr;
  // Staged submit input: written by the display task under the local mutex,
  // read by render() under the global guard. Single writer/reader (the display
  // task), so the handoff itself needs no further locking.
  AppListFrameSnapshot snapshot_;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void reload();
  void render() const;
  void openSelected();
  void openInstall();
  void uninstallSelected();
  bool selectedIsPlugin() const;
  void selectIndex(int index);
  void moveSelection(int delta);
  void activateBuiltin(BuiltinAction action);
  void drawItemIcon(const DrawerItem& item, const TouchHitGeometry::Rect& tile) const;
};
