#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"
#include "./MyLibraryActivity.h"
#include "../../RecentBooksStore.h"

struct Rect;

class HomeActivity final : public Activity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  int selectorIndex = 0;
  bool updateRequired = false;
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


  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
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
                        bool animateEntry = false, int animationDirection = 0)
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
        onAppsOpen(onAppsOpen) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool isHomeActivity() const override { return true; }
};
