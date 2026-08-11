#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "../ActivityWithSubactivity.h"

class MyLibraryActivity final : public ActivityWithSubactivity {
 private:

  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;

  size_t selectorIndex = 0;
  bool updateRequired = false;

  // Files state
  std::string basepath = "/";
  std::vector<std::string> files;
  std::vector<uint32_t> fileSizes;          // parallel to files: byte size (0 = directory)
  std::vector<uint32_t> searchResultSizes;  // parallel to searchResults
  bool showAllFiles = false;                // false = book-only, true = show all files

  // Callbacks
  const std::function<void(const std::string& path, const std::string& originalSourcePath)> onSelectBook;
  const std::function<void()> onGoHome;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;

  // Data loading
  void loadFiles();
  size_t findEntry(const std::string& name) const;

  //文件管理
  std::string copySourcePath;  // 待复制的源路径
  bool hasCopyData = false;    // 是否有待复制内容
  bool isCutMode = false;  
  //搜索模式
  bool isSearchMode = false;
  std::vector<std::string> searchResults; //搜索结果
  std::string originalBasePath; // 进入搜索前的路径

  // pending search request from keyboard callback; handled in loop()
  bool pendingSearch = false;
  std::string pendingKeyword;

  void executeSearch();
  void cancelSearch();
  static void sortFileList(std::vector<std::string>& strs); // 新增静态成员函数声明
  void doSearch(const char* keyword); // 执行搜索（接收char[])
    
  // 壁纸相关功能
  void previewImage(const std::string& imagePath);
  bool setAsSleepWallpaper(const std::string& imagePath);
  bool setAsSleepWallpaperWithMode(const std::string& imagePath, bool invert);
  bool setAsSleepWallpaperTransparent(const std::string& overlayImagePath);
  bool generatePxcCache(const std::string& imagePath, const std::string& pxcPath);
  void drawPreviewImageMenu();
  void handlePreviewImageMenuAction();
  // Shared by button Confirm and touch tap on the action popup.
  void executeActionMenu(int index);

  // 图片预览状态（非阻塞状态机）
  bool isPreviewingImage = false;
  bool isPreviewImageMenuShowing = false;
  bool grayPreviewActive = false;  // 屏幕当前处于灰阶物理状态，BW 渲染前需做 FULL_REFRESH 归一化
  int previewImageMenuIndex = 0;
  std::string currentPreviewPath;

  // 操作菜单弹窗状态
  bool showingActionMenu = false;
  int actionMenuIndex = 0;
  std::string actionTargetPath;
  bool actionMenuToggled = false;  // 防止长按重复弹出菜单
  bool menuJustOpened = false;     // 长按开菜单后吸收按键松手事件
  char SEARCH_KEYWORD[100] = "赛博";


 public:
  explicit MyLibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                             const std::function<void()>& onGoHome,
                             const std::function<void(const std::string& path, const std::string& originalSourcePath)>& onSelectBook,
                             std::string initialPath = "/")
      : ActivityWithSubactivity("MyLibrary", renderer, mappedInput),
        basepath(initialPath.empty() ? "/" : std::move(initialPath)),
        onSelectBook(onSelectBook),
        onGoHome(onGoHome) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
