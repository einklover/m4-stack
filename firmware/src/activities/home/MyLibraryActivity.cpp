#include "MyLibraryActivity.h"

#include <GfxRenderer.h>
#include <SDCardManager.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/apps/AppInstallActivity.h"
#include "util/TouchHitGeometry.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4UiText.h"
#include "util/StringUtils.h"
//加入搜索
#include "../util/KeyboardEntryActivity.h"
// 壁纸功能
#include "../../lib/GfxRenderer/Bitmap.h"
#include "../../lib/Epub/Epub/converters/PngToFramebufferConverter.h"
#include "../../lib/Epub/Epub/converters/JpegToFramebufferConverter.h"
#include "../../lib/Epub/Epub/converters/ImageDecoderFactory.h"
// 图片 .pxc 缓存（预览与关机壁纸共用）
#include "../../util/ImageCache.h"
// PNG 编码（用于透明壁纸叠加合成）
#include "../../../lib/miniz/miniz.h"


namespace {
constexpr int SKIP_PAGE_MS = 700;
constexpr unsigned long GO_HOME_MS = 1000;
//防止误删，把删除改为长按confirm
constexpr int COPY_BUF_SIZE = 256; // 256字节缓冲区，适配小运存

// Format a byte count as a human-readable size string (e.g. "1.2MB", "512KB").
// Returns an empty string for 0 (used to suppress size display for directories).
std::string formatFileSize(uint32_t bytes) {
  if (bytes == 0) return "";
  char buf[16];
  if (bytes >= 1024u * 1024u) {
    snprintf(buf, sizeof(buf), "%.1fMB", bytes / (1024.0f * 1024.0f));
  } else if (bytes >= 1024u) {
    snprintf(buf, sizeof(buf), "%uKB", static_cast<unsigned>(bytes / 1024u));
  } else {
    snprintf(buf, sizeof(buf), "%uB", static_cast<unsigned>(bytes));
  }
  return buf;
}
}  // namespace
//把原来几个函数加上
//删除
bool deleteFileOrDir(const std::string& fullPath) {
  if (fullPath.back() == '/') {
    std::string dirPath = fullPath.substr(0, fullPath.length() - 1);
    bool deleted = SdMan.removeDir(dirPath.c_str());
    if (deleted) {
      Serial.printf("[删除] 成功删除一级文件夹：%s\n", dirPath.c_str());
    } else {
      Serial.printf("[删除] 失败删除一级文件夹（非空/不存在）：%s\n", dirPath.c_str());
    }
    return deleted;
  } else {
    if (!SdMan.exists(fullPath.c_str())) {
      Serial.printf("[删除] 文件不存在：%s\n", fullPath.c_str());
      return false;
    }
    bool deleted = SdMan.remove(fullPath.c_str());
    if (deleted) {
      Serial.printf("[删除] 成功删除文件：%s\n", fullPath.c_str());
    } else {
      Serial.printf("[删除] 失败删除文件：%s\n", fullPath.c_str());
    }
    return deleted;
  }
}
//复制
bool copyFile(const char* srcPath, const char* dstPath) {
  // 检查源文件是否存在
  if (!SdMan.exists(srcPath)) {
    Serial.printf("[复制] 源文件不存在：%s\n", srcPath);
    return false;
  }
  // 检查目标文件是否已存在
  if (SdMan.exists(dstPath)) {
    Serial.printf("[复制] 目标文件已存在：%s\n", dstPath);
    return false;
  }

  FsFile srcFile, dstFile;
  // 打开源文件
  if (!SdMan.openFileForRead("FileSelection", srcPath, srcFile)) {
    Serial.printf("[复制] 打开源文件失败：%s\n", srcPath);
    return false;
  }
  // 打开目标文件（创建新文件）
  if (!SdMan.openFileForWrite("FileSelection", dstPath, dstFile)) {
    Serial.printf("[复制] 创建目标文件失败：%s\n", dstPath);
    srcFile.close();
    return false;
  }

  // 256字节缓冲区，边读边写
  uint8_t buf[COPY_BUF_SIZE];
  size_t readBytes = 0;
  while ((readBytes = srcFile.read(buf, COPY_BUF_SIZE)) > 0) {
    dstFile.write(buf, readBytes);
  }

  // 关闭文件句柄，释放资源
  srcFile.close();
  dstFile.close();
  
  Serial.printf("[复制] 成功：%s → %s\n", srcPath, dstPath);
  return true;
}
//复制文件夹
bool copyDir(const char* srcPath, const char* dstPath) {
  // 检查源文件夹是否存在
  if (!SdMan.exists(srcPath)) {
    Serial.printf("[复制] 源文件夹不存在：%s\n", srcPath);
    return false;
  }
  // 创建目标文件夹
  if (!SdMan.mkdir(dstPath, true)) {
    Serial.printf("[复制] 创建目标文件夹失败：%s\n", dstPath);
    return false;
  }
  Serial.printf("[复制] 文件夹成功：%s → %s\n", srcPath, dstPath);
  return true;
}

// 递归搜索含关键词文件，并收集对应文件大小
void searchFilesRecursive(const std::string& currentDir, const std::string& keyword,
                           std::vector<std::string>& result, std::vector<uint32_t>& sizes) {
  auto root = SdMan.open(currentDir.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  char name[500];
  root.rewindDirectory();
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if (name[0] == '.' || strcmp(name, "System Volume Information") == 0) {
      file.close();
      continue;
    }

    std::string fullPath = currentDir;
    if (fullPath.back() != '/') fullPath += "/";
    fullPath += name;

    if (file.isDirectory()) {
      searchFilesRecursive(fullPath + "/", keyword, result, sizes);
    } else {
      std::string fn = name;
      std::transform(fn.begin(), fn.end(), fn.begin(), ::tolower);
      std::string kw = keyword;
      std::transform(kw.begin(), kw.end(), kw.begin(), ::tolower);

      if (fn.find(kw) != std::string::npos) {
        if (StringUtils::checkFileExtension(fn, ".epub") ||
            StringUtils::checkFileExtension(fn, ".xtch") ||
            StringUtils::checkFileExtension(fn, ".xtc") ||
            StringUtils::checkFileExtension(fn, ".txt") ||
            StringUtils::checkFileExtension(fn, ".md")) {
          result.push_back(fullPath);
          sizes.push_back(static_cast<uint32_t>(file.size()));
        }
      }
    }
    file.close();
  }
  root.close();
}
void MyLibraryActivity::sortFileList(std::vector<std::string>& strs) {
  std::sort(begin(strs), end(strs), [](const std::string& str1, const std::string& str2) {
    if (str1.back() == '/' && str2.back() != '/') return true;
    if (str1.back() != '/' && str2.back() == '/') return false;
    return lexicographical_compare(
        begin(str1), end(str1), begin(str2), end(str2),
        [](const char& char1, const char& char2) { return tolower(char1) < tolower(char2); });
  });
}
// 执行搜索（接收char*关键词，适配100字符限制）
void MyLibraryActivity::doSearch(const char* keyword) {
  // 安全拷贝关键词（防止超长，最多99字符+结束符）
  strncpy(SEARCH_KEYWORD, keyword, sizeof(SEARCH_KEYWORD)-1);
  SEARCH_KEYWORD[sizeof(SEARCH_KEYWORD)-1] = '\0'; // 确保字符串结束

  isSearchMode = true;
  originalBasePath = basepath;
  searchResults.clear();
  searchResultSizes.clear();
  
  Serial.printf("[搜索] 开始搜索 %s 及其子目录中包含'%s'的文件\n", basepath.c_str(), SEARCH_KEYWORD);
  // 调用递归搜索（传char数组）
  searchFilesRecursive(basepath, SEARCH_KEYWORD, searchResults, searchResultSizes);
  
  // Sort results and their sizes together to keep the two vectors in sync
  {
    std::vector<std::pair<std::string, uint32_t>> pairs;
    pairs.reserve(searchResults.size());
    for (size_t i = 0; i < searchResults.size(); i++)
      pairs.emplace_back(std::move(searchResults[i]),
                         i < searchResultSizes.size() ? searchResultSizes[i] : 0u);
    std::sort(pairs.begin(), pairs.end(),
              [](const std::pair<std::string, uint32_t>& a,
                 const std::pair<std::string, uint32_t>& b) {
                return std::lexicographical_compare(
                    a.first.begin(), a.first.end(), b.first.begin(), b.first.end(),
                    [](const char& c1, const char& c2) { return tolower(c1) < tolower(c2); });
              });
    searchResults.clear();
    searchResultSizes.clear();
    for (auto& p : pairs) {
      searchResults.push_back(std::move(p.first));
      searchResultSizes.push_back(p.second);
    }
  }
  selectorIndex = 0;
  updateRequired = true;
  
  if (searchResults.empty()) {
    // 提示文字适配char数组
    char emptyHint[128];
    snprintf(emptyHint, sizeof(emptyHint), "未找到含'%s'的文件", SEARCH_KEYWORD);
    Serial.printf("[搜索] %s\n", emptyHint);
  } else {
    Serial.printf("[搜索] 共找到 %d 个匹配文件\n", searchResults.size());
  }
}
// 打开键盘输入Activity（核心修复）
void MyLibraryActivity::executeSearch() {
  // 清除现有子活动然后弹出输入框获取关键词
  exitActivity();
  updateRequired = true;
  enterNewActivity(new KeyboardEntryActivity(
      renderer, mappedInput, "输入搜索关键词", SEARCH_KEYWORD, 10,
      63,     // 最大长度63，与其它地方保持一致
      false,  // 非密码模式
      [this](const std::string& keyword) {
          // 保存关键词；不要立即删除键盘（会在键盘自身loop中导致对象自毁)
          std::string safeKeyword = keyword;
          // 确保不超过数组大小（100字节）
          if (safeKeyword.size() >= sizeof(SEARCH_KEYWORD)) {
              safeKeyword = safeKeyword.substr(0, sizeof(SEARCH_KEYWORD) - 1);
          }
          // ✅ 用memcpy替代strncpy，保证UTF-8完整
          memset(SEARCH_KEYWORD, 0, sizeof(SEARCH_KEYWORD)); // 先清空
          memcpy(SEARCH_KEYWORD, safeKeyword.c_str(), safeKeyword.size());
          
          // 隐藏键盘，使后续按键直接落到父活动
          if (subActivity) {
              static_cast<KeyboardEntryActivity*>(subActivity.get())->hide();
          }
          pendingSearch = true;
          pendingKeyword = SEARCH_KEYWORD;
          updateRequired = true;
      },
      [this]() {
          // 取消输入，仅请求退出子活动
          pendingSearch = false;
          updateRequired = true;
      }));
}

void MyLibraryActivity::cancelSearch() {
  isSearchMode = false;
  searchResults.clear();
  searchResultSizes.clear();
  basepath = originalBasePath;
  loadFiles();
  selectorIndex = 0;
  updateRequired = true;
}






void MyLibraryActivity::taskTrampoline(void* param) {
  auto* self = static_cast<MyLibraryActivity*>(param);
  self->displayTaskLoop();
}

void MyLibraryActivity::loadFiles() {
  files.clear();
  fileSizes.clear();

  // 修复：确保路径以/结尾，否则SdMan.open可能识别失败
  std::string realBasePath = basepath;
  if (realBasePath != "/" && realBasePath.back() != '/') {
    realBasePath += "/";
  }

  auto root = SdMan.open(realBasePath.c_str()); // 用修复后的路径打开
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  root.rewindDirectory();

  // Build entries as pairs <name, size> so files and sizes are sorted together
  std::vector<std::pair<std::string, uint32_t>> entries;
  char name[500];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));

    if (!showAllFiles) {
      // 书籍模式：跳过隐藏文件、系统目录
      if (name[0] == '.' || strcmp(name, "System Volume Information") == 0) {
        file.close();
        continue;
      }
    } else {
      // 全部显示模式：仅跳过 Windows 系统卷目录
      if (strcmp(name, "System Volume Information") == 0) {
        file.close();
        continue;
      }
    }

    if (file.isDirectory()) {
      entries.emplace_back(std::string(name) + "/", 0u); // 目录不显示大小
    } else {
      auto filename = std::string(name);
      const bool isBookFile =
          StringUtils::checkFileExtension(filename, ".epub") ||
          StringUtils::checkFileExtension(filename, ".xtch") ||
          StringUtils::checkFileExtension(filename, ".xtc") ||
          StringUtils::checkFileExtension(filename, ".txt") ||
          StringUtils::checkFileExtension(filename, ".md");
      const bool isImageFile =
          StringUtils::checkFileExtension(filename, ".png") ||
          StringUtils::checkFileExtension(filename, ".bmp") ||
          StringUtils::checkFileExtension(filename, ".jpg") ||
          StringUtils::checkFileExtension(filename, ".jpeg");
      const bool isFontFile =
          StringUtils::checkFileExtension(filename, ".epdfont");
      const bool isAppPackage =
          StringUtils::checkFileExtension(filename, ".m4x");
      if (showAllFiles || isBookFile || isImageFile || isFontFile || isAppPackage) {
        entries.emplace_back(filename, static_cast<uint32_t>(file.size()));
      }
    }
    file.close();
  }
  root.close();

  // Sort entries: directories first, then alphabetically (case-insensitive)
  std::sort(entries.begin(), entries.end(),
            [](const std::pair<std::string, uint32_t>& a,
               const std::pair<std::string, uint32_t>& b) {
              const bool aDir = a.first.back() == '/';
              const bool bDir = b.first.back() == '/';
              if (aDir && !bDir) return true;
              if (!aDir && bDir) return false;
              return std::lexicographical_compare(
                  a.first.begin(), a.first.end(), b.first.begin(), b.first.end(),
                  [](const char& c1, const char& c2) { return tolower(c1) < tolower(c2); });
            });

  // Split sorted pairs back into the two parallel vectors
  files.reserve(entries.size());
  fileSizes.reserve(entries.size());
  for (auto& e : entries) {
    files.push_back(std::move(e.first));
    fileSizes.push_back(e.second);
  }
  
  // 关键优化：释放 entries 占用的内存，减少内存碎片化
  // 在WiFi AP模式下，内存非常紧张，必须及时释放临时缓冲区
  entries.clear();
  entries.shrink_to_fit();  // 真正释放 vector 内部缓冲区
}

//enter也需要改
void MyLibraryActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  // 如果有设置Home目录，进入时直接跳转到该目录
  if (basepath == "/" && SETTINGS.libraryHomePath[0] != '\0') {
    basepath = std::string(SETTINGS.libraryHomePath);
  }

  loadFiles();

  selectorIndex = 0;
  //新增
  showingActionMenu = false;
  actionMenuIndex = 0;
  actionTargetPath = "";
  copySourcePath = "";
  hasCopyData = false;
  isCutMode = false;
  isSearchMode = false;
  showAllFiles = false;
  searchResults.clear();
  searchResultSizes.clear();
  originalBasePath = "";
  isPreviewingImage = false;
  //新增结束

  updateRequired = true;

  xTaskCreate(&MyLibraryActivity::taskTrampoline, "MyLibraryActivityTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void MyLibraryActivity::onExit() {
  ActivityWithSubactivity::onExit();

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;

  files.clear();
}

//核心：修改loop，匹配这几个我需要的操作
void MyLibraryActivity::loop() {
  if (subActivity) {
      pumpSubActivityFrame();
      // if a search was requested while the keyboard was running, close it now
      if (pendingSearch) {
          // perform exit after keyboard loop returns to avoid self-delete
          exitActivity();
          doSearch(pendingKeyword.c_str());
          pendingSearch = false;
          updateRequired = true;
      }
      return;
  }
  // if the keyboard has already been dismissed but search still pending (edge case)
  if (pendingSearch) {
      doSearch(pendingKeyword.c_str());
      pendingSearch = false;
      updateRequired = true;
  }

  // ---- Touch path (M4): list select/activate, action popup, back/home ----
  // Swipe is checked before tap so scrolling never also activates a row.
  if (mappedInput.hasTouch()) {
    auto openSelected = [this]() {
      std::string fullPath;
      bool hasValidSelection = false;
      if (isSearchMode) {
        if (!searchResults.empty() && selectorIndex < searchResults.size()) {
          fullPath = searchResults[selectorIndex];
          hasValidSelection = true;
        }
      } else if (!files.empty() && selectorIndex < files.size()) {
        fullPath = basepath;
        if (fullPath.back() != '/') fullPath += "/";
        fullPath += files[selectorIndex];
        hasValidSelection = true;
      }
      if (!hasValidSelection) return;
      if (!fullPath.empty() && fullPath.back() == '/') {
        basepath = fullPath.substr(0, fullPath.length() - 1);
        loadFiles();
        selectorIndex = 0;
        updateRequired = true;
      } else if (StringUtils::checkFileExtension(fullPath, ".png") ||
                 StringUtils::checkFileExtension(fullPath, ".bmp") ||
                 StringUtils::checkFileExtension(fullPath, ".jpg") ||
                 StringUtils::checkFileExtension(fullPath, ".jpeg")) {
        previewImage(fullPath);
      } else if (StringUtils::checkFileExtension(fullPath, ".m4x")) {
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        exitActivity();
        enterNewActivity(new AppInstallActivity(renderer, mappedInput, fullPath, [this]() {
          exitActivity();
          updateRequired = true;
        }));
        xSemaphoreGive(renderingMutex);
      } else {
        std::string originalSourcePath;
        if (StringUtils::checkFileExtension(fullPath, ".epub")) {
          const auto& recentBooks = RECENT_BOOKS.getBooks();
          for (const auto& book : recentBooks) {
            if (book.path == fullPath) {
              originalSourcePath = book.originalSourcePath;
              break;
            }
          }
        }
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        renderer.clearScreen();
        GUI.drawPopup(renderer, "正在建立书籍索引...");
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
        xSemaphoreGive(renderingMutex);
        onSelectBook(fullPath, originalSourcePath);
      }
    };

    // Action popup: shared geometry with render()
    if (showingActionMenu) {
      constexpr int ACTION_MENU_COUNT = 7;
      const int pageWidth = renderer.getScreenWidth();
      const int pageHeight = renderer.getScreenHeight();
      const auto popup = TouchHitGeometry::makeCenteredPopupMenu(pageWidth, pageHeight, ACTION_MENU_COUNT);
      int tx = 0, ty = 0;
      if (mappedInput.wasBackGesture()) {
        showingActionMenu = false;
        updateRequired = true;
        return;
      }
      if (mappedInput.wasScreenTapped(tx, ty)) {
        int hit = -1;
        if (TouchHitGeometry::popupMenuIndexFromPoint(popup, tx, ty, hit)) {
          actionMenuIndex = hit;
          menuJustOpened = false;
          executeActionMenu(actionMenuIndex);
        } else {
          showingActionMenu = false;
          updateRequired = true;
        }
        return;
      }
      if (mappedInput.wasScreenTouchDown(tx, ty)) {
        int hit = -1;
        if (TouchHitGeometry::popupMenuIndexFromPoint(popup, tx, ty, hit) && actionMenuIndex != hit) {
          actionMenuIndex = hit;
          updateRequired = true;
        }
        return;
      }
      return;  // while popup open, don't process list touch
    }

    if (isPreviewingImage) {
      int tx = 0, ty = 0;
      if (mappedInput.wasBackGesture()) {
        if (isPreviewImageMenuShowing) {
          isPreviewImageMenuShowing = false;
          previewImage(currentPreviewPath);
          return;
        }
        if (grayPreviewActive) {
          xSemaphoreTake(renderingMutex, portMAX_DELAY);
          renderer.clearScreen();
          renderer.displayBuffer(HalDisplay::FAST_REFRESH);
          xSemaphoreGive(renderingMutex);
          grayPreviewActive = false;
        }
        isPreviewingImage = false;
        isPreviewImageMenuShowing = false;
        updateRequired = true;
        return;
      }

      // Preview action popup (2 rows) — shared geometry with drawPreviewImageMenu().
      if (isPreviewImageMenuShowing) {
        constexpr int PREVIEW_MENU_COUNT = 2;
        // Match drawPreviewImageMenu(): popupW=190, itemH=40, padding=4.
        const auto popup = TouchHitGeometry::makeCenteredPopupMenu(
            renderer.getScreenWidth(), renderer.getScreenHeight(), PREVIEW_MENU_COUNT, 190, 40, 4);
        if (mappedInput.wasScreenTapped(tx, ty)) {
          int hit = -1;
          if (TouchHitGeometry::popupMenuIndexFromPoint(popup, tx, ty, hit)) {
            previewImageMenuIndex = hit;
            isPreviewImageMenuShowing = false;
            handlePreviewImageMenuAction();
          } else {
            isPreviewImageMenuShowing = false;
            previewImage(currentPreviewPath);
          }
          return;
        }
        if (mappedInput.wasScreenTouchDown(tx, ty)) {
          int hit = -1;
          if (TouchHitGeometry::popupMenuIndexFromPoint(popup, tx, ty, hit) && previewImageMenuIndex != hit) {
            previewImageMenuIndex = hit;
            drawPreviewImageMenu();
          }
          return;
        }
        return;
      }

      if (mappedInput.wasScreenTapped(tx, ty)) {
        // Center third opens preview menu; sides exit
        const auto zone =
            TouchHitGeometry::readerZoneFromPoint(tx, ty, renderer.getScreenWidth(), renderer.getScreenHeight());
        if (zone == TouchHitGeometry::ReaderZone::Menu) {
          previewImageMenuIndex = 0;
          isPreviewImageMenuShowing = true;
          drawPreviewImageMenu();
        } else {
          if (grayPreviewActive) {
            xSemaphoreTake(renderingMutex, portMAX_DELAY);
            renderer.clearScreen();
            renderer.displayBuffer(HalDisplay::FAST_REFRESH);
            xSemaphoreGive(renderingMutex);
            grayPreviewActive = false;
          }
          isPreviewingImage = false;
          isPreviewImageMenuShowing = false;
          updateRequired = true;
        }
        return;
      }
      // fall through to button handling for preview menu keys
    } else {
      // Main file list
      if (mappedInput.wasBackGesture()) {
        if (basepath != "/") {
          size_t lastSlash = basepath.find_last_of('/');
          basepath = (lastSlash == 0) ? "/" : basepath.substr(0, lastSlash);
          loadFiles();
          selectorIndex = 0;
          updateRequired = true;
        } else {
          onGoHome();
        }
        return;
      }
      const auto swipe = mappedInput.wasSwipe();
      const int itemCount = static_cast<int>(isSearchMode ? searchResults.size() : files.size());
      const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, false);
      if (swipe == MappedInputManager::SwipeDir::Up && itemCount > 0) {
        selectorIndex = std::min(itemCount - 1, static_cast<int>(selectorIndex) + std::max(1, pageItems));
        updateRequired = true;
        return;
      }
      if (swipe == MappedInputManager::SwipeDir::Down && itemCount > 0) {
        selectorIndex = std::max(0, static_cast<int>(selectorIndex) - std::max(1, pageItems));
        updateRequired = true;
        return;
      }

      auto metrics = UITheme::getInstance().getMetrics();
      const int pageHeight = renderer.getScreenHeight();
      int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
      // Match MyLibraryActivity::render() copy/cut status strip.
      if (hasCopyData && !copySourcePath.empty()) {
        contentTop += 40 + 6;
      }
      const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
      // drawList uses listRowHeight when subtitle callback is null.
      const int rowStep = metrics.listRowHeight;
      int tx = 0, ty = 0;
      if (itemCount > 0 && mappedInput.wasScreenTapped(tx, ty)) {
        int hit = -1;
        if (TouchHitGeometry::listIndexFromPoint(ty, contentTop, contentHeight, rowStep, itemCount,
                                                 static_cast<int>(selectorIndex), hit)) {
          selectorIndex = static_cast<size_t>(hit);
          openSelected();
        }
        return;
      }
      if (itemCount > 0 && mappedInput.wasScreenTouchDown(tx, ty)) {
        int hit = -1;
        if (TouchHitGeometry::listIndexFromPoint(ty, contentTop, contentHeight, rowStep, itemCount,
                                                 static_cast<int>(selectorIndex), hit)) {
          if (static_cast<int>(selectorIndex) != hit) {
            selectorIndex = static_cast<size_t>(hit);
            updateRequired = true;
          }
        }
        return;
      }
    }
  }

  // 图片预览模式：Back/Up/Down 退出预览，Confirm 弹出菜单
  if (isPreviewingImage) {
    if (isPreviewImageMenuShowing) {
      constexpr int PREVIEW_MENU_COUNT = 2;
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        isPreviewImageMenuShowing = false;
        // 重新渲染预览图（关闭菜单后恢复）
        previewImage(currentPreviewPath);
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                 mappedInput.wasReleased(MappedInputManager::Button::Left)) {
        previewImageMenuIndex = (previewImageMenuIndex + PREVIEW_MENU_COUNT - 1) % PREVIEW_MENU_COUNT;
        drawPreviewImageMenu();
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                 mappedInput.wasReleased(MappedInputManager::Button::Right)) {
        previewImageMenuIndex = (previewImageMenuIndex + 1) % PREVIEW_MENU_COUNT;
        drawPreviewImageMenu();
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        handlePreviewImageMenuAction();
      }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      // 灰阶状态：先做白色归一化 pass（FAST_REFRESH 驱灰→白），再由 render() 做第二次 FAST_REFRESH
      // 与 previewImage 的 BW-pass → displayGrayBuffer 两步节奏一致
      if (grayPreviewActive) {
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        renderer.clearScreen();
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
        xSemaphoreGive(renderingMutex);
        grayPreviewActive = false;
      }
      isPreviewingImage = false;
      updateRequired = true;
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      previewImageMenuIndex = 0;
      isPreviewImageMenuShowing = true;
      // 灰阶状态：先做白色归一化 pass（FAST_REFRESH 驱灰→白），再由 drawPreviewImageMenu 做第二次 FAST_REFRESH
      if (grayPreviewActive) {
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        renderer.clearScreen();
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
        xSemaphoreGive(renderingMutex);
        grayPreviewActive = false;
      }
      drawPreviewImageMenu();
    }
    return;
  }
  // Long press BACK (1s+) goes to root folder
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= GO_HOME_MS &&
      basepath != "/") {
    basepath = "/";
    loadFiles();
    selectorIndex = 0;
    updateRequired = true;
    return;
  }
 //弹出操作菜单时，拦截所有按键由菜单处理
  if (showingActionMenu) {
    constexpr int ACTION_MENU_COUNT = 7;
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      showingActionMenu = false;
      updateRequired = true;
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
               mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      actionMenuIndex = (actionMenuIndex + ACTION_MENU_COUNT - 1) % ACTION_MENU_COUNT;
      updateRequired = true;
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
               mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      actionMenuIndex = (actionMenuIndex + 1) % ACTION_MENU_COUNT;
      updateRequired = true;
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // 如果是长按开菜单后的第一次松手，吸收该事件不执行操作
      if (menuJustOpened) {
        menuJustOpened = false;
      } else {
        executeActionMenu(actionMenuIndex);
      }  // end else (not menuJustOpened)
    }
    return;
  }

  // 长按 Confirm（700ms+）：弹出操作菜单（仅当「长按打开菜单」开启时）
  if (SETTINGS.libraryLongPressMenu &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= SKIP_PAGE_MS) {
    if (!actionMenuToggled) {
      actionMenuToggled = true;
      std::string fullPath;
      bool hasValidSelection = false;
      if (isSearchMode) {
        if (!searchResults.empty() && selectorIndex < searchResults.size()) {
          fullPath = searchResults[selectorIndex];
          hasValidSelection = true;
        }
      } else {
        if (!files.empty() && selectorIndex < files.size()) {
          fullPath = basepath;
          if (fullPath.back() != '/') fullPath += "/";
          fullPath += files[selectorIndex];
          hasValidSelection = true;
        }
      }
      if (hasValidSelection) {
        actionTargetPath = fullPath;
        showingActionMenu = true;
        menuJustOpened = true;  // 吸收紧随其后的按键松手事件
        actionMenuIndex = 0;
        updateRequired = true;
      }
    }
    return;
  }
  // 短按 Confirm：单击弹出菜单模式下弹出菜单，长按模式下直接打开文件
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && !actionMenuToggled) {
    // 单击弹菜单模式（libraryLongPressMenu=0）：短按弹出操作菜单
    if (!SETTINGS.libraryLongPressMenu) {
      std::string fullPath;
      bool hasValidSelection = false;
      if (isSearchMode) {
        if (!searchResults.empty() && selectorIndex < searchResults.size()) {
          fullPath = searchResults[selectorIndex];
          hasValidSelection = true;
        }
      } else {
        if (!files.empty() && selectorIndex < files.size()) {
          fullPath = basepath;
          if (fullPath.back() != '/') fullPath += "/";
          fullPath += files[selectorIndex];
          hasValidSelection = true;
        }
      }
      if (hasValidSelection) {
        actionTargetPath = fullPath;
        showingActionMenu = true;
        menuJustOpened = false;
        actionMenuIndex = 0;
        updateRequired = true;
      }
      return;
    }
    // 长按弹菜单模式（libraryLongPressMenu=1）：短按直接打开文件 / 进入文件夹
    std::string fullPath;
    bool hasValidSelection = false;

    if (isSearchMode) {
      if (!searchResults.empty() && selectorIndex < searchResults.size()) {
        fullPath = searchResults[selectorIndex];
        hasValidSelection = true;
      }
    } else {
      if (!files.empty() && selectorIndex < files.size()) {
        const std::string& selectedItem = files[selectorIndex];
        fullPath = basepath;
        if (fullPath.back() != '/') fullPath += "/";
        fullPath += selectedItem;
        hasValidSelection = true;
      }
    }

    if (hasValidSelection) {
      if (!fullPath.empty() && fullPath.back() == '/') {
        // 进入文件夹
        basepath = fullPath.substr(0, fullPath.length() - 1);
        loadFiles();
        selectorIndex = 0;
      } else if (StringUtils::checkFileExtension(fullPath, ".png") ||
                 StringUtils::checkFileExtension(fullPath, ".bmp") ||
                 StringUtils::checkFileExtension(fullPath, ".jpg") ||
                 StringUtils::checkFileExtension(fullPath, ".jpeg")) {
        // 预览 PNG/BMP/JPG 图片（previewImage 内部管理渲染，不设 updateRequired）
        previewImage(fullPath);
        return;  // 预览后直接 return，避免外层再次设置 updateRequired
      } else if (StringUtils::checkFileExtension(fullPath, ".m4x")) {
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        exitActivity();
        enterNewActivity(new AppInstallActivity(renderer, mappedInput, fullPath, [this]() {
          exitActivity();
          updateRequired = true;
        }));
        xSemaphoreGive(renderingMutex);
        return;
      } else {
        // 打开文件
        std::string originalSourcePath;
        if (StringUtils::checkFileExtension(fullPath, ".epub")) {
          const auto& recentBooks = RECENT_BOOKS.getBooks();
          for (const auto& book : recentBooks) {
            if (book.path == fullPath) {
              originalSourcePath = book.originalSourcePath;
              break;
            }
          }
        }
        // 关闭菜单并显示加载提示，防止卡在菜单页面
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        renderer.clearScreen();
        GUI.drawPopup(renderer, "正在建立书籍索引...");
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
        xSemaphoreGive(renderingMutex);
        onSelectBook(fullPath, originalSourcePath);
      }
      updateRequired = true;
    }
    return;
  }
  // 按键释放后重置长按标志（必须在 wasReleased 检查之后）
  if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    actionMenuToggled = false;
  }





  //新增结束
  // 第3/4按钮（Left/Right）同样触发上下移动
  const bool upReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                          mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool downReleased = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);

  const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;
  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, false);

  //把文件打开的逻辑放上面了
  //这里去掉了
  //后面没动

if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
  // Short press or BT virtual: go up one directory, or go home if at root.
  // NOTE: Do NOT check getHeldTime() here. The long-press case (go to root / go home)
  // is already handled above via isPressed(Back) && getHeldTime() >= GO_HOME_MS.
  // Checking getHeldTime() here would block BT virtual button releases when the user
  // previously held a physical key for a long time (getHeldTime() stays large until
  // a new physical press resets it).
  if (basepath != "/") {
    const std::string oldPath = basepath;

    // 修复：正确截取上级目录（处理嵌套路径）
    size_t lastSlash = basepath.find_last_of('/');
    // 避免截取后为空（比如 /dir1 → 截取后是 ""，要改成 "/"）
    basepath = (lastSlash == 0) ? "/" : basepath.substr(0, lastSlash);
    
    loadFiles(); // 重新加载上级目录内容

    // 修复：返回上级后定位到之前的目录项
    const std::string dirName = oldPath.substr(lastSlash + 1) + "/";
    selectorIndex = findEntry(dirName);

    updateRequired = true;
  } else {
    onGoHome();
  }
}

  const auto& displayList = isSearchMode ? searchResults : files;
  int listSize = static_cast<int>(displayList.size());
  if (upReleased) {
    if (skipPage) {
      selectorIndex = std::max(static_cast<int>((selectorIndex / pageItems - 1) * pageItems), 0);
    } else {
      selectorIndex = (selectorIndex + listSize - 1) % listSize;
    }
    updateRequired = true;
  } else if (downReleased) {
    if (skipPage) {
      selectorIndex = std::min(static_cast<int>((selectorIndex / pageItems + 1) * pageItems), listSize - 1);
    } else {
      selectorIndex = (selectorIndex + 1) % listSize;
    }
    updateRequired = true;
  }
}



void MyLibraryActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired && !isPreviewingImage) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

  //添加四个按鈕：删除、复制、剪切、粘贴
  //添加搜索和取消搜索
void MyLibraryActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = UITheme::getInstance().getMetrics();
  auto folderName = basepath == "/" ? "SD卡" : basepath.substr(basepath.rfind('/') + 1).c_str();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, folderName);



  int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  // 有复制/剪切标记时显示状态栏
  if (hasCopyData && !copySourcePath.empty()) {
    const size_t lastSlash = copySourcePath.find_last_of('/');
    const std::string srcName = copySourcePath.substr(lastSlash + 1);
    const std::string statusText = std::string(isCutMode ? "[剪切] " : "[复制] ") + srcName;
    constexpr int statusBarH = 40;
    // 用淡灰背景区分状态栏
    renderer.fillRectDither(0, contentTop, pageWidth, statusBarH, Color::LightGray);
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, contentTop + 4, statusText.c_str(), true);
    contentTop += statusBarH + 6;  // 状态栏高度 + 6px 间距
  }
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  // 核心：根据是否搜索模式，选择要显示的列表（files 或 searchResults）
  const auto& displayList = isSearchMode ? searchResults : files;

  // 显示空列表提示（区分普通模式和搜索模式）
  if (displayList.empty()) {
      // 先定义提示文本的基础部分
      char emptyHint[128];
      // 拼接 "未找到含'关键词'的文件"
      snprintf(emptyHint, sizeof(emptyHint), "未找到含'%s'的文件", SEARCH_KEYWORD);
      // 赋值给emptyText
      std::string emptyText = isSearchMode ? emptyHint : "No books found";
      M4UiText::draw(renderer, UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, emptyText.c_str());
  } else {
      // 绘制列表时，用 displayList 替代原来的 files
      const auto& displaySizes = isSearchMode ? searchResultSizes : fileSizes;
      GUI.drawList(
          renderer, Rect{0, contentTop, pageWidth, contentHeight}, displayList.size(), selectorIndex,
          [this](int index) { 
              // 这里返回当前模式下的列表项
              return isSearchMode ? searchResults[index] : files[index];
          },
          nullptr, 
          [this](int index) -> UIIcon {
              // 返回文件/目录的图标
              const std::string& item = isSearchMode ? searchResults[index] : files[index];
              return UITheme::getFileIcon(item);
          },
          [&displaySizes](int index) -> std::string {
              if (index < static_cast<int>(displaySizes.size()))
                  return formatFileSize(displaySizes[index]);
              return "";
          });
  }

  // Help text
  if (showingActionMenu) {
    const auto labels = mappedInput.mapLabels("« 取消", "确认", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    const char* confirmHint = SETTINGS.libraryLongPressMenu ? "打开" : "菜单";
    const auto labels = mappedInput.mapLabels("« 返回", confirmHint, "上移", "下移");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  // 展示操作菜单弹窗
  if (showingActionMenu) {
    // 最后一项根据当前显示模式动态更新
    char toggleLabel[20];
    snprintf(toggleLabel, sizeof(toggleLabel), "7) %s", showAllFiles ? "显示书籍" : "显示全部");
    const char* menuItems[] = {"1) 打开文件", "2) 删除文件", "3) 复制文件",
                               "4) 剪切文件", "5) 粘贴文件", "6) 设为主页",
                               toggleLabel};
    const int menuItemCount = 7;
    constexpr int popupW = 170;
    constexpr int itemH = 40;
    constexpr int padding = 4;
    const int popupH = menuItemCount * itemH + padding * 2;
    const int popupX = (pageWidth - popupW) / 2;
    const int popupY = (pageHeight - popupH) / 2;
    renderer.fillRect(popupX - 4, popupY - 4, popupW + 8, popupH + 8, false);
    renderer.drawRect(popupX, popupY, popupW, popupH, true);
    for (int i = 0; i < menuItemCount; i++) {
      const int itemY = popupY + padding + i * itemH;
      const bool selected = (i == actionMenuIndex);
      if (selected) {
        renderer.fillRect(popupX + 2, itemY, popupW - 4, itemH, true);
      }
      const int textX = popupX + (popupW - M4UiText::textWidth(renderer, UI_12_FONT_ID, menuItems[i])) / 2;
      M4UiText::draw(renderer, UI_12_FONT_ID, textX, itemY + 6, menuItems[i], !selected);
    }
  }

  renderer.displayBuffer();
}



void MyLibraryActivity::executeActionMenu(int index) {
  const std::string fullPath = actionTargetPath;
  showingActionMenu = false;
  switch (index) {
    case 0: {  // 打开
      if (!fullPath.empty() && fullPath.back() == '/') {
        basepath = fullPath.substr(0, fullPath.length() - 1);
        loadFiles();
        selectorIndex = 0;
      } else if (StringUtils::checkFileExtension(fullPath, ".png") ||
                 StringUtils::checkFileExtension(fullPath, ".bmp") ||
                 StringUtils::checkFileExtension(fullPath, ".jpg") ||
                 StringUtils::checkFileExtension(fullPath, ".jpeg")) {
        previewImage(fullPath);
        return;
      } else {
        std::string originalSourcePath;
        if (StringUtils::checkFileExtension(fullPath, ".epub")) {
          const auto& recentBooks = RECENT_BOOKS.getBooks();
          for (const auto& book : recentBooks) {
            if (book.path == fullPath) {
              originalSourcePath = book.originalSourcePath;
              break;
            }
          }
        }
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        renderer.clearScreen();
        GUI.drawPopup(renderer, "正在建立书籍索引...");
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
        xSemaphoreGive(renderingMutex);
        onSelectBook(fullPath, originalSourcePath);
      }
      break;
    }
    case 1: {  // 删除
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      GUI.drawPopup(renderer, "正在删除...");
      xSemaphoreGive(renderingMutex);
      deleteFileOrDir(fullPath);
      if (isSearchMode) {
        doSearch(SEARCH_KEYWORD);
      } else {
        loadFiles();
      }
      break;
    }
    case 2: {  // 复制
      copySourcePath = fullPath;
      hasCopyData = true;
      isCutMode = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      GUI.drawPopup(renderer, "已标记为复制，切换目录后粘贴");
      xSemaphoreGive(renderingMutex);
      break;
    }
    case 3: {  // 剪切
      copySourcePath = fullPath;
      hasCopyData = true;
      isCutMode = true;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      GUI.drawPopup(renderer, "已标记为剪切，切换目录后粘贴");
      xSemaphoreGive(renderingMutex);
      break;
    }
    case 4: {  // 粘贴
      if (!hasCopyData) {
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        GUI.drawPopup(renderer, "无待粘贴内容");
        xSemaphoreGive(renderingMutex);
        break;
      }
      std::string dstPath = basepath;
      if (dstPath.back() != '/') dstPath += "/";
      size_t lastSlash = copySourcePath.find_last_of('/');
      std::string fileName = copySourcePath.substr(lastSlash + 1);
      dstPath += fileName;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      GUI.drawPopup(renderer, "正在粘贴...");
      xSemaphoreGive(renderingMutex);
      bool pasteSuccess = false;
      if (!copySourcePath.empty() && copySourcePath.back() == '/') {
        pasteSuccess = copyDir(copySourcePath.c_str(), dstPath.c_str());
      } else {
        pasteSuccess = copyFile(copySourcePath.c_str(), dstPath.c_str());
      }
      if (pasteSuccess && isCutMode) {
        deleteFileOrDir(copySourcePath);
        isCutMode = false;
      }
      hasCopyData = false;
      copySourcePath = "";
      if (isSearchMode) {
        doSearch(SEARCH_KEYWORD);
      } else {
        loadFiles();
      }
      break;
    }
    case 5: {  // 设为Home目录
      strncpy(SETTINGS.libraryHomePath, basepath.c_str(), sizeof(SETTINGS.libraryHomePath) - 1);
      SETTINGS.libraryHomePath[sizeof(SETTINGS.libraryHomePath) - 1] = '\0';
      SETTINGS.saveToFile();
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      const std::string homeMsg = std::string("已设为Home：") + basepath;
      GUI.drawPopup(renderer, homeMsg.c_str());
      xSemaphoreGive(renderingMutex);
      break;
    }
    case 6: {  // 显示全部 / 显示书籍
      showAllFiles = !showAllFiles;
      if (!isSearchMode) {
        loadFiles();
      }
      selectorIndex = 0;
      break;
    }
  }
  updateRequired = true;
}

// 预览图像功能（非阻塞：仅渲染一次，通过 isPreviewingImage 标志让 loop() 拦截按键）
void MyLibraryActivity::previewImage(const std::string& imagePath) {
  // 先暂停 displayTaskLoop 刷新，防止并发渲染
  updateRequired = false;

  bool success = false;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // ── 预检：灰阶模式时提前查缓存，命中则跳过弹窗（减少一次多余刷新）──────
  const bool isGrayMode = SETTINGS.imageQuality >= CrossPointSettings::QUALITY_NORMAL
                          && !StringUtils::checkFileExtension(imagePath, ".bmp");
  uint32_t preCheckSrcSize = 0;
  bool preCheckHdHit = false;
  if (isGrayMode) {
    preCheckSrcSize = ImageCache::getSourceSize(imagePath);
    preCheckHdHit  = ImageCache::isHdValid(imagePath, preCheckSrcSize);
  }

  // 非灰阶，或灰阶首次解码（需要 1~2s，显示加载提示）
  if (!preCheckHdHit) {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    renderer.clearScreen();
    GUI.drawPopup(renderer, "加载中...");
    xSemaphoreGive(renderingMutex);
  }

  if (StringUtils::checkFileExtension(imagePath, ".bmp")) {
    // BMP 使用 Bitmap 类直接渲染
    FsFile file;
    if (SdMan.openFileForRead("Preview", imagePath, file)) {
      Bitmap bitmap(file, true);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        int x = 0, y = 0;
        float cropX = 0, cropY = 0;
        if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
          float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
          const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);
          if (ratio > screenRatio) {
            y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
          } else {
            x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
          }
        } else {
          x = (pageWidth - bitmap.getWidth()) / 2;
          y = (pageHeight - bitmap.getHeight()) / 2;
        }
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        renderer.clearScreen();
        renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
        renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 28, "← 返回  确认 菜单");
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
        xSemaphoreGive(renderingMutex);
        success = true;
      }
      file.close();
    }
  } else {
    // PNG / JPG / JPEG — 使用 ImageDecoderFactory，支持 .pxc 缓存
    // 复用顶部预检结果（避免重复 stat 调用）
    uint32_t srcSize = isGrayMode ? preCheckSrcSize : ImageCache::getSourceSize(imagePath);

    // ── 灰阶 4阶预览路径（正常/高清）──────────────────────────────────────
    if (SETTINGS.imageQuality >= CrossPointSettings::QUALITY_NORMAL) {
      bool useHalf = (SETTINGS.imageQuality == CrossPointSettings::QUALITY_HD);
      bool hdHit = preCheckHdHit;  // 复用顶部预检

      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      renderer.clearScreen();

      bool hdSuccess = false;
      if (hdHit) {
        hdSuccess = ImageCache::renderFromHdCache(imagePath, renderer);
        Serial.printf("[%lu] [PRV] HD cache hit: %s\n", millis(), imagePath.c_str());
      } else {
        ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
        if (decoder) {
          RenderConfig rc;
          rc.x = 0; rc.y = 0;
          rc.maxWidth = pageWidth; rc.maxHeight = pageHeight;
          rc.useDithering = false;
          rc.cachePath = ImageCache::getHdDecodeCachePath(imagePath);
          hdSuccess = decoder->decodeToFramebuffer(imagePath, renderer, rc);
          Serial.printf("[%lu] [PRV] HD decoded: %s\n", millis(), imagePath.c_str());
        }
      }

      if (hdSuccess) {
        // BW 预览 pass（含提示文字）
        renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 28, "← 返回  确认 菜单");
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
        if (useHalf) delay(200);
        // GRAYSCALE_LSB pass
        renderer.clearScreen(0x00);
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
        ImageCache::renderFromHdCache(imagePath, renderer);
        renderer.copyGrayscaleLsbBuffers();
        // GRAYSCALE_MSB pass
        renderer.clearScreen(0x00);
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
        ImageCache::renderFromHdCache(imagePath, renderer);
        renderer.copyGrayscaleMsbBuffers();
        renderer.displayGrayBuffer();
        renderer.setRenderMode(GfxRenderer::BW);
        success = true;
        grayPreviewActive = true;  // 屏幕进入灰阶物理状态
      }
      xSemaphoreGive(renderingMutex);

      // 提交 HD 索引（mutex 外，避免持锁时 IO）
      if (hdSuccess && !hdHit && srcSize > 0) ImageCache::commitHd(imagePath, srcSize);
    }

    // ── 普通 BW 路径（高清未启用，或 HD 渲染失败时回退）─────────────────
    if (!success) {
      // 1. 先在 mutex 外做磁盘操作（获取文件大小、检查缓存有效性）
      bool cacheHit = ImageCache::isValid(imagePath, srcSize);
      // 若没有缓存，获取解码输出路径（会确保 /.crosspoint/lock_screen/ 目录存在）
      std::string decodeCachePath = cacheHit ? "" : ImageCache::getDecodeCachePath(imagePath);

      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      renderer.clearScreen();

      if (cacheHit) {
        // 2a. 缓存命中：直接从 .pxc 渲染，跳过解码
        Serial.printf("[%lu] [PRV] Cache hit: %s\n", millis(), imagePath.c_str());
        if (ImageCache::renderFromCache(imagePath, renderer)) {
          renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 28, "← 返回  确认 菜单");
          renderer.displayBuffer(HalDisplay::FAST_REFRESH);
          success = true;
        } else {
          cacheHit = false;  // 缓存损坏，回退到解码
        }
      }

      if (!cacheHit) {
        // 2b. 缓存未命中：正常解码，同时写入 .pxc 缓存
        ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
        if (decoder) {
          RenderConfig renderConfig;
          renderConfig.x = 0;
          renderConfig.y = 0;
          renderConfig.maxWidth = pageWidth;
          renderConfig.maxHeight = pageHeight;
          renderConfig.useDithering = true;
          renderConfig.cachePath = decodeCachePath;  // 解码时同步写 .pxc

          if (decoder->decodeToFramebuffer(imagePath, renderer, renderConfig)) {
            renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 28, "← 返回  确认 菜单");
            renderer.displayBuffer(HalDisplay::FAST_REFRESH);
            success = true;
            grayPreviewActive = false;  // BW 预览，屏幕不在灰阶状态
          }
        }
      }

      xSemaphoreGive(renderingMutex);

      // 3. 解码成功后（mutex 已释放）提交索引
      if (success && !cacheHit && srcSize > 0) {
        ImageCache::commit(imagePath, srcSize);
      }
    }
  }

  if (success) {
    currentPreviewPath = imagePath;
    isPreviewingImage = true;
    isPreviewImageMenuShowing = false;
  } else {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    // 检查是否为非JPEG格式伪装成.jpg的情况
    const char* errorMsg = "加载失败";
    if (StringUtils::checkFileExtension(imagePath, ".jpg") ||
        StringUtils::checkFileExtension(imagePath, ".jpeg")) {
      FsFile checkFile;
      if (SdMan.openFileForRead("JPG-CHECK", imagePath, checkFile)) {
        uint8_t header[3];
        if (checkFile.read(header, 3) != 3 || header[0] != 0xFF || header[1] != 0xD8 || header[2] != 0xFF) {
          errorMsg = "该文件不是真正的JPEG格式";
        }
        checkFile.close();
      }
    }
    GUI.drawPopup(renderer, errorMsg);
    xSemaphoreGive(renderingMutex);
    delay(1000);
  }
}

// 设置为关机壁纸（默认普通壁纸模式，调用带模式版本）
bool MyLibraryActivity::setAsSleepWallpaper(const std::string& imagePath) {
  return setAsSleepWallpaperWithMode(imagePath, true);
}

// 设置为关机壁纸（带模式：invert=true 普通壁纸/深色背景，invert=false 透明壁纸/浅色背景）
bool MyLibraryActivity::setAsSleepWallpaperWithMode(const std::string& imagePath, bool invert) {
  bool isPng  = StringUtils::checkFileExtension(imagePath, ".png");
  bool isBmp  = StringUtils::checkFileExtension(imagePath, ".bmp");
  bool isJpg  = StringUtils::checkFileExtension(imagePath, ".jpg") ||
                StringUtils::checkFileExtension(imagePath, ".jpeg");

  if (!isPng && !isBmp && !isJpg) return false;

  size_t lastSlash = imagePath.find_last_of('/');
  std::string fileName = imagePath.substr(lastSlash + 1);

  // BMP 复制到 /sleep/，PNG/JPG 复制到 /lock_screen/
  std::string destPath;
  if (isBmp) {
    destPath = "/sleep/" + fileName;
    if (!SdMan.exists("/sleep")) SdMan.mkdir("/sleep");
  } else {
    destPath = "/lock_screen/" + fileName;
    if (!SdMan.exists("/lock_screen")) {
      SdMan.mkdir("/lock_screen");
    } else {
      // 先清空 /lock_screen/ 目录下所有旧壁纸文件，确保只保留新设置的壁纸
      auto dir = SdMan.open("/lock_screen");
      if (dir && dir.isDirectory()) {
        char name[500];
        for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
          if (!file.isDirectory()) {
            file.getName(name, sizeof(name));
            std::string delPath = "/lock_screen/" + std::string(name);
            SdMan.remove(delPath.c_str());
            Serial.printf("[壁纸] 清除旧壁纸：%s\n", delPath.c_str());
          }
          file.close();
        }
        dir.close();
      }
    }
  }

  bool fileReady = false;
  if (SdMan.exists(destPath.c_str())) {
    if (isBmp) {
      // BMP 已存在：返回失败（与原行为一致）
      Serial.printf("[壁纸] BMP已存在：%s\n", destPath.c_str());
      return false;
    }
    // PNG/JPG 已存在：允许切换普通/透明模式
    Serial.printf("[壁纸] PNG/JPG已存在，更新模式：%s\n", destPath.c_str());
    fileReady = true;
  } else {
    if (!copyFile(imagePath.c_str(), destPath.c_str())) {
      Serial.printf("[壁纸] 复制文件失败：%s -> %s\n", imagePath.c_str(), destPath.c_str());
      return false;
    }
    fileReady = true;
    Serial.printf("[壁纸] 已复制：%s -> %s\n", imagePath.c_str(), destPath.c_str());
  }

  if (fileReady) {
    if (isBmp) {
      SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM;
    } else {
      SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::MARSK2;
      SETTINGS.sleepPngInvert = invert ? 1 : 0;
    }
    SETTINGS.saveToFile();
    Serial.printf("[壁纸] 已设为%s\n", invert ? "普通壁纸" : "透明壁纸");
  }
  return fileReady;
}

// ── 设置透明壁纸：保存 .pxc 路径到 settings，关机时由 SleepActivity 叠加 ──
// 如果缓存不存在，会尝试实时解码生成（适用于大尺寸图片）
// 原理：关机时 framebuffer 保留阅读页内容，SleepActivity 直接叠加覆盖层像素
bool MyLibraryActivity::setAsSleepWallpaperTransparent(const std::string& overlayImagePath) {
  // 确认覆盖层的 _hd.pxc 缓存存在（图片需预先被查看过以生成缓存）
  std::string pxcPath = ImageCache::getHdDecodeCachePath(overlayImagePath);
  if (!SdMan.exists(pxcPath.c_str())) {
    // 尝试普通 .pxc 缓存
    pxcPath = ImageCache::getDecodeCachePath(overlayImagePath);
    if (!SdMan.exists(pxcPath.c_str())) {
      // 缓存不存在，尝试实时解码生成（适用于大尺寸图片）
      Serial.printf("[壁纸] .pxc 缓存不存在，尝试实时生成: %s\n", overlayImagePath.c_str());
      if (!generatePxcCache(overlayImagePath, pxcPath)) {
        Serial.printf("[壁纸] 实时生成缓存失败: %s\n", overlayImagePath.c_str());
        return false;
      }
    }
  }

  // 路径长度检查（128 字节限制）
  if (pxcPath.length() >= 128) {
    Serial.printf("[壁纸] .pxc 路径过长: %u chars\n", (unsigned)pxcPath.length());
    return false;
  }

  // 保存路径并切换模式
  memset(SETTINGS.transparentOverlayPxc, 0, sizeof(SETTINGS.transparentOverlayPxc));
  memcpy(SETTINGS.transparentOverlayPxc, pxcPath.c_str(), pxcPath.length());
  SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::TRANSPARENT;
  SETTINGS.saveToFile();

  Serial.printf("[壁纸] 透明壁纸覆盖层已设置: %s\n", pxcPath.c_str());
  return true;
}

// ── 实时生成 .pxc 缓存（用于大尺寸图片的透明壁纸设置）────────────────────────
// 使用行缓冲方式，避免一次性分配大量内存
bool MyLibraryActivity::generatePxcCache(const std::string& imagePath, const std::string& pxcPath) {
  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
  if (!decoder) {
    Serial.printf("[壁纸] 不支持的图片格式: %s\n", imagePath.c_str());
    return false;
  }

  // 获取图片尺寸
  ImageDimensions dims;
  if (!decoder->getDimensions(imagePath, dims)) {
    Serial.printf("[壁纸] 无法获取图片尺寸: %s\n", imagePath.c_str());
    return false;
  }

  const int srcW = dims.width;
  const int srcH = dims.height;
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();

  // 计算缩放后的尺寸（保持宽高比，适应屏幕）
  float scaleX = (float)screenW / srcW;
  float scaleY = (float)screenH / srcH;
  float scale = (scaleX < scaleY) ? scaleX : scaleY;
  if (scale > 1.0f) scale = 1.0f;

  const int dstW = (int)(srcW * scale);
  const int dstH = (int)(srcH * scale);

  Serial.printf("[壁纸] 生成缓存: %s (%dx%d -> %dx%d)\n", imagePath.c_str(), srcW, srcH, dstW, dstH);

  // 创建 .pxc 文件
  FsFile cacheFile;
  if (!SdMan.openFileForWrite("PXC-GEN", pxcPath, cacheFile)) {
    Serial.printf("[壁纸] 无法创建缓存文件: %s\n", pxcPath.c_str());
    return false;
  }

  // 写入文件头（宽度和高度）
  uint16_t w = dstW;
  uint16_t h = dstH;
  cacheFile.write(&w, 2);
  cacheFile.write(&h, 2);

  // 分配行缓冲（2-bit packed，每行 (width+3)/4 字节）
  const int bytesPerRow = (dstW + 3) / 4;
  uint8_t* rowBuffer = (uint8_t*)malloc(bytesPerRow);
  if (!rowBuffer) {
    Serial.printf("[壁纸] 无法分配行缓冲\n");
    cacheFile.close();
    SdMan.remove(pxcPath.c_str());
    return false;
  }

  // 分配 RGBA 缓冲区用于解码
  const int rgbaBufSize = dstW * dstH * 4;
  uint8_t* rgbaBuf = (uint8_t*)malloc(rgbaBufSize);
  if (!rgbaBuf) {
    Serial.printf("[壁纸] 无法分配 RGBA 缓冲区\n");
    free(rowBuffer);
    cacheFile.close();
    SdMan.remove(pxcPath.c_str());
    return false;
  }

  // 解码到 RGBA 缓冲区
  int outW, outH;
  size_t pixelsWritten = decoder->decodeToPixelBuf(imagePath, rgbaBuf, dstW, dstH, outW, outH);

  if (pixelsWritten == 0) {
    Serial.printf("[壁纸] 解码失败\n");
    free(rgbaBuf);
    free(rowBuffer);
    cacheFile.close();
    SdMan.remove(pxcPath.c_str());
    return false;
  }

  // 将 RGBA 转换为 2-bit 灰度并写入文件
  for (int y = 0; y < dstH; y++) {
    memset(rowBuffer, 0, bytesPerRow);
    for (int x = 0; x < dstW; x++) {
      int rgbaIdx = (y * dstW + x) * 4;
      uint8_t r = rgbaBuf[rgbaIdx];
      uint8_t g = rgbaBuf[rgbaIdx + 1];
      uint8_t b = rgbaBuf[rgbaIdx + 2];
      // 转换为灰度
      uint8_t gray = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
      // 转换为 2-bit (0-3)
      uint8_t pixelValue = (gray < 64) ? 0 : (gray < 128) ? 1 : (gray < 192) ? 2 : 3;
      // 打包到 2-bit (MSB first)
      int byteIdx = x / 4;
      int bitShift = 6 - (x % 4) * 2;
      rowBuffer[byteIdx] |= (pixelValue << bitShift);
    }
    cacheFile.write(rowBuffer, bytesPerRow);
  }

  free(rgbaBuf);
  free(rowBuffer);
  cacheFile.close();

  Serial.printf("[壁纸] 缓存生成完成: %s (%dx%d, %d bytes)\n", pxcPath.c_str(), dstW, dstH, 4 + bytesPerRow * dstH);
  return true;
}

// 图片预览时的三项菜单绘制
void MyLibraryActivity::drawPreviewImageMenu() {
  static constexpr const char* menuItems[] = {"设为关机壁纸", "删除图片"};
  const char* const* items = menuItems;
  constexpr int MENU_COUNT = 2;
  static constexpr int popupW = 190;
  static constexpr int itemH = 40;
  static constexpr int padding = 4;
  const int popupH = MENU_COUNT * itemH + padding * 2;

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  const auto pageWidth  = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int popupX = (pageWidth  - popupW) / 2;
  const int popupY = (pageHeight - popupH) / 2;

  renderer.fillRect(popupX - 4, popupY - 4, popupW + 8, popupH + 8, false);
  renderer.drawRect(popupX, popupY, popupW, popupH, true);
  for (int i = 0; i < MENU_COUNT; i++) {
    const int itemY   = popupY + padding + i * itemH;
    const bool sel    = (i == previewImageMenuIndex);
    if (sel) renderer.fillRect(popupX + 2, itemY, popupW - 4, itemH, true);
    const int textX = popupX + (popupW - M4UiText::textWidth(renderer, UI_12_FONT_ID, items[i])) / 2;
    M4UiText::draw(renderer, UI_12_FONT_ID, textX, itemY + 6, items[i], !sel);
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  xSemaphoreGive(renderingMutex);
}

// 处理预览菜单选项
void MyLibraryActivity::handlePreviewImageMenuAction() {
  isPreviewImageMenuShowing = false;
  // 0=设为关机壁纸, 1=删除图片
  if (previewImageMenuIndex == 0) {  // 设为关机壁纸
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    GUI.drawPopup(renderer, "正在设置...");
    xSemaphoreGive(renderingMutex);
    bool ok = setAsSleepWallpaperWithMode(currentPreviewPath, false);
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    GUI.drawPopup(renderer, ok ? "已设为关机壁纸" : "设置失败");
    xSemaphoreGive(renderingMutex);
    delay(1000);
    isPreviewingImage = false;
    if (isSearchMode) { doSearch(SEARCH_KEYWORD); } else { loadFiles(); }
    updateRequired = true;
  } else {  // 删除图片（index 1）
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    GUI.drawPopup(renderer, "正在删除...");
    xSemaphoreGive(renderingMutex);
    bool ok = deleteFileOrDir(currentPreviewPath);
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    GUI.drawPopup(renderer, ok ? "已删除" : "删除失败");
    xSemaphoreGive(renderingMutex);
    delay(1000);
    isPreviewingImage = false;
    if (isSearchMode) { doSearch(SEARCH_KEYWORD); } else { loadFiles(); }
    updateRequired = true;
  }
}



size_t MyLibraryActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == name) return i;
  return 0;
}
