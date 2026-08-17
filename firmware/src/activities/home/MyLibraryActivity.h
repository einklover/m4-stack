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
  std::string copySourcePath;
  bool hasCopyData = false;
  bool isCutMode = false;
  //搜索模式
  bool isSearchMode = false;
  std::vector<std::string> searchResults;
  std::string originalBasePath;

  bool pendingSearch = false;
  std::string pendingKeyword;

  void executeSearch();
  void cancelSearch();
  static void sortFileList(std::vector<std::string>& strs);
  void doSearch(const char* keyword);

  void previewImage(const std::string& imagePath);
  bool setAsSleepWallpaper(const std::string& imagePath);
  bool setAsSleepWallpaperWithMode(const std::string& imagePath, bool invert);
  bool setAsSleepWallpaperTransparent(const std::string& overlayImagePath);
  bool generatePxcCache(const std::string& imagePath, const std::string& pxcPath);
  void drawPreviewImageMenu();
  void handlePreviewImageMenuAction();
  void executeActionMenu(int index);

  bool isPreviewingImage = false;
  bool isPreviewImageMenuShowing = false;
  bool grayPreviewActive = false;
  int previewImageMenuIndex = 0;
  std::string currentPreviewPath;

  bool showingActionMenu = false;
  int actionMenuIndex = 0;
  std::string actionTargetPath;
  bool actionMenuToggled = false;
  bool menuJustOpened = false;
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

  // Read-only state for simulator journeys. File names are sanitized so this
  // remains valid JSON even when a user book contains quotes/control bytes.
  std::string debugUiJson() override {
    auto safe = [](const std::string& in) {
      std::string out;
      out.reserve(in.size());
      for (unsigned char c : in) {
        if (c == '"' || c == '\\' || c < 0x20 || c == 0x7f) out.push_back('_');
        else out.push_back(static_cast<char>(c));
      }
      return out;
    };
    const auto& visible = isSearchMode ? searchResults : files;
    const std::string selected = selectorIndex < visible.size() ? visible[selectorIndex] : std::string();
    std::string out = "{\"basepath\":\"" + safe(basepath) + "\",\"selected\":\"" + safe(selected) +
                      "\",\"selected_index\":" + std::to_string(selectorIndex) +
                      ",\"count\":" + std::to_string(visible.size()) +
                      ",\"search\":" + (isSearchMode ? "true" : "false") + "}";
    return out;
  }
};
