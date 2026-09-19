#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../Activity.h"
#include "./MyLibraryActivity.h"
#include "../../RecentBooksStore.h"
#include "ui/pages/HomeSceneModel.h"
#include "ui/scene/UiSceneAssets.h"

struct Rect;

class HomeActivity final : public Activity {
  TaskHandle_t displayTaskHandle = nullptr;
#ifdef CROSSPOINT_MURPHY_M4
  struct BackendContext {
    HomeScene::HomeSceneModel model;
    std::vector<RecentBook> recentBooks;
    std::atomic<bool> cancelled{false};
    std::atomic<uint32_t> epoch{0};
    std::atomic<bool> exiting{false};
    std::atomic<bool> updateRequired{false};
    BackendContext() = default;
  };
  TaskHandle_t sceneBackendTaskHandle = nullptr;
  std::shared_ptr<BackendContext> backendCtx;
  UiScene::UiSceneAssets sceneAssets{};
  UiScene::UiSceneActionQueue sceneActionQueue;
  UiScene::UiSceneActionDispatcher sceneActionDispatcher;
  uint8_t sceneFocusIndex = 0;
#endif
  SemaphoreHandle_t renderingMutex = nullptr;
  int selectorIndex = 0;
  std::atomic<bool> updateRequired{false};
  bool recentsLoading = false;
  bool recentsLoaded = false;
  bool firstRenderDone = false;
  bool animateEntry = false;
  int entryAnimationDirection = 0;
  bool hasOpdsUrl = false;
  bool hasjianguoUrl = false;
  bool hasDataCapsuleUrl = false;  // 数据胶囊配置标志
  bool hasBookmarkNotes = false;   // 书签笔记标志
  bool coverRendered = false;      // Track if cover has been rendered once
  bool coverBufferStored = false;  // Track if cover buffer is stored
  bool showMemWarning = false;     // Show low memory warning dialog
  bool memWarningSelected = false; // false=取消, true=立即重启
  uint8_t* coverBuffer = nullptr;  // HomeActivity's own buffer for cover image
  size_t coverBufferRowBytes = 0;
  size_t coverBufferRows = 0;
  size_t coverBufferXByteOffset = 0;
  size_t coverBufferYStart = 0;
  std::vector<RecentBook> recentBooks;
  const std::function<void(const std::string& path, const std::string& originalSourcePath)> onSelectBook;
  const std::function<void()> onMyLibraryOpen;
  const std::function<void()> onRecentsOpen;
  const std::function<void()> onSettingsOpen;
  const std::function<void()> onFileTransferOpen;
  const std::function<void()> onOpdsBrowserOpen;
  const std::function<void()> onJianGuoYunOpen;
  const std::function<void()> onDataCapsuleOpen;  // 数据胶囊回调
  const std::function<void()> onBookmarkNotesOpen;  // 书签笔记回调
  const std::function<void()> onAppsOpen;           // 扩展应用列表
  const std::function<void(const std::string& appId)> onOpenNativeApp;


  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
#ifdef CROSSPOINT_MURPHY_M4
  static void sceneBackendTaskTrampoline(void* param);
  // Lifetime-safe backend: owns its own context, never touches raw HomeActivity `this`.
  static void backendLoop(BackendContext& ctx);
  static void loadRecentBooksInto(BackendContext& ctx, int maxBooks);
  static bool tryEnsureCoverThumbInCtx(BackendContext& ctx, const std::string& coverBmpPath, int w, int h,
                                       const std::function<bool()>& cancelled = {});
  static void publishHomeSceneFromBackendCtx(BackendContext& ctx);
  static bool publishHomeSceneWithAssetsCtx(BackendContext& ctx);
  // Legacy trampoline for compatibility (unused after refactor, kept to avoid ODR)
  [[noreturn]] void sceneBackendTaskLoop();
  void publishHomeSceneFromBackend();
  bool publishHomeSceneWithAssets();
  bool tryEnsureCoverThumb(const std::string& coverBmpPath, int w, int h);
  bool queueHomeSceneAction(const UiScene::UiSceneAction& action);
  static bool dispatchHomeSceneAction(void* user, const UiScene::UiSceneAction& action);
  bool dispatchHomeSceneAction(const UiScene::UiSceneAction& action);
  void dispatchHomeSceneActions();
  void handleSnapshotInput();
  void renderSnapshotScene();
#endif
  void render();
  int getMenuItemCount() const;
  bool storeCoverBuffer();    // Store frame buffer for cover image
  bool restoreCoverBuffer();  // Restore frame buffer from stored cover
  void freeCoverBuffer();     // Free the stored cover buffer
  bool computeCoverBufferLayout(size_t& xByteOffset, size_t& yStart, size_t& rowBytes, size_t& rows) const;
  void loadRecentBooks(int maxBooks);
  void loadRecentCovers(int coverWidth, int coverHeight);
  void renderMemWarning();
  static int loadBookProgress(const std::string& path);  // 读取书籍进度

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        const std::function<void(const std::string& path, const std::string& originalSourcePath)>& onSelectBook,
                        const std::function<void()>& onMyLibraryOpen, const std::function<void()>& onRecentsOpen,
                        const std::function<void()>& onSettingsOpen, const std::function<void()>& onFileTransferOpen,
                        const std::function<void()>& onOpdsBrowserOpen, const std::function<void()>& onJianGuoYunOpen,
                        const std::function<void()>& onDataCapsuleOpen,
                        const std::function<void()>& onBookmarkNotesOpen,
                        const std::function<void()>& onAppsOpen,
                        bool animateEntry = false, int animationDirection = 0,
                        const std::function<void(const std::string& appId)>& onOpenNativeApp = {})
      : Activity("Home", renderer, mappedInput),
        animateEntry(animateEntry),
        entryAnimationDirection(animationDirection),
        onSelectBook(onSelectBook),
        onMyLibraryOpen(onMyLibraryOpen),
        onRecentsOpen(onRecentsOpen),
        onSettingsOpen(onSettingsOpen),
        onFileTransferOpen(onFileTransferOpen),
        onOpdsBrowserOpen(onOpdsBrowserOpen),
        onJianGuoYunOpen(onJianGuoYunOpen),
        onDataCapsuleOpen(onDataCapsuleOpen),
        onBookmarkNotesOpen(onBookmarkNotesOpen),
        onAppsOpen(onAppsOpen),
        onOpenNativeApp(onOpenNativeApp) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool isHomeActivity() const override { return true; }
  bool showTouchNavigation() const override { return false; }
  uint8_t touchFooterButtonsMask() const override {
    return M4FooterTouchPolicy::Confirm | M4FooterTouchPolicy::Left | M4FooterTouchPolicy::Right;
  }
};
