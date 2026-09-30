#pragma once

#include "../ActivityWithSubactivity.h"
#include "apps/M4xRegistry.h"
#include "components/themes/BaseTheme.h"
#include "util/TouchHitGeometry.h"

#include <atomic>
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
    std::function<void()> onFileTransferOpen;
    std::function<void()> onAppStoreOpen;
  };

  explicit AppListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                           const std::function<void()>& onGoBack, Callbacks callbacks = {})
      : ActivityWithSubactivity("AppList", renderer, mappedInput),
        onGoBack(onGoBack),
        callbacks_(std::move(callbacks)) {}

  void onEnter() override;
  void onExit() override;
  ~AppListActivity() override;
  bool readyForDestruction() const override {
    return displayTaskExited_.load(std::memory_order_acquire) &&
           ActivityWithSubactivity::readyForDestruction();
  }
  void loop() override;
  bool showTouchNavigation() const override { return false; }
  uint8_t touchFooterButtonsMask() const override {
    return mode_ == 1 || mode_ == 2 ? M4FooterTouchPolicy::Back | M4FooterTouchPolicy::Confirm
                      : M4FooterTouchPolicy::Back | M4FooterTouchPolicy::Confirm |
                            M4FooterTouchPolicy::Right |
                            (selectedIsPlugin() ? M4FooterTouchPolicy::Left : 0);
  }

 private:
  enum class BuiltinAction : uint8_t {
    FileManager,
    FileTransfer,
    AppStore,
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
  // because acceptPendingDrawer() replaces items_ and apps_ together.
  // The main loop fills snapshot_ once under the local mutex and render()
  // reads that member under the global guard. The background task never
  // submits and never writes the lists loop() reads unlocked. The drawer
  // is small, so one dirty-frame fill beats a shared-pointer refactor here.
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
  std::atomic<bool> updateRequired_{false};
  // 0 = list mode, 1 = confirm uninstall, 2 = plugin context menu
  int mode_ = 0;
  bool uninstallClearData_ = true;

  TaskHandle_t displayTaskHandle_ = nullptr;
  SemaphoreHandle_t renderingMutex_ = nullptr;
  // onExit only signals the display owner to stop. The reaper deletes the
  // local mutexes after that owner publishes exit. There is no timeout kill
  // and no join on the UI thread.
  std::atomic<bool> exitDisplayTask_{false};
  std::atomic<bool> displayTaskExited_{true};
  // Set before installing a child so the display task never has to inspect
  // ActivityWithSubactivity::subActivity across task boundaries.
  std::atomic<bool> childScreenOwned_{false};
  // Return visits arm a background cache check. The worker reloads afterwards.
  // The main loop submits another frame only when that reload changed the list.
  std::atomic<bool> verifyDrawerCache_{false};
  // Set by the main loop after a successful drawer submit. The worker waits
  // for this before the first background reload.
  std::atomic<bool> showedDrawer_{false};
  // reload() publishes the next drawer here under renderingMutex_. loop()
  // moves it into apps_ and items_. The worker never writes those, nor
  // selectedIndex_ or mode_.
  std::vector<M4xInstalledApp> pendingApps_;
  std::vector<DrawerItem> pendingItems_;
  std::atomic<bool> pendingReady_{false};
  SemaphoreHandle_t reloadLock_ = nullptr;
  // Staged submit input: the main loop fills this once under the local mutex,
  // and render() reads it under the global guard. Single writer and reader,
  // both on the UI thread, so the handoff itself needs no further locking.
  AppListFrameSnapshot snapshot_;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void submitDirtyFrame();
  void acceptPendingDrawer();
  void discardPendingDrawer();
  bool reload(bool preserveDialog = false);
  bool applyCachedDrawer();
  static bool sameDrawer(const std::vector<M4xInstalledApp>& appsA, const std::vector<DrawerItem>& itemsA,
                         const std::vector<M4xInstalledApp>& appsB, const std::vector<DrawerItem>& itemsB);
  void render() const;
  void openSelected();
  void openInstall();
  void pinSelected(int slot);
  void openContextMenu();
  void uninstallSelected();
  bool selectedIsPlugin() const;
  void selectIndex(int index);
  void moveSelection(int delta);
  void pageBy(int pages);
  void activateBuiltin(BuiltinAction action);
  void drawItemIcon(const DrawerItem& item, const TouchHitGeometry::Rect& tile) const;
};
