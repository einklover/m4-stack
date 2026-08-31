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
