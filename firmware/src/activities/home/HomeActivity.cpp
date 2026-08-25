#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <SDCardManager.h>
#include <Txt.h>
#include <Utf8.h>
#include <Xtc.h>

#include <cstring>
#include <string>

#include <esp_heap_caps.h>
#include <vector>

#include <HalPowerManager.h>
#include "CrossPointSettings.h"
#include "I18n.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "apps/M4xRegistry.h"
#include "util/M4ContentProviderContract.h"
#include "util/M4HistoryReopen.h"
#include "util/M4HomeBookDetailMeta.h"
#include "BookmarkStore.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "util/M4UiText.h"
#include "util/StringUtils.h"
#include "util/TouchHitGeometry.h"
void HomeActivity::taskTrampoline(void* param) {
  auto* self = static_cast<HomeActivity*>(param);
  self->displayTaskLoop();
}

int HomeActivity::getMenuItemCount() const {
  int count = 5;  // My Library, Recents, File transfer, Apps, Settings
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  if (hasOpdsUrl) {
    count++;
  }
  if (hasjianguoUrl) count++;
  if (hasDataCapsuleUrl) count++;  // 数据胶囊
  if (hasBookmarkNotes) count++;   // 书签笔记
  return count;

}


void HomeActivity::loadRecentBooks(int maxBooks) {
  {
    std::vector<M4HomeBookDetailMeta::InstalledPlugin> plugins;
    const auto apps = M4xRegistry::load();
    plugins.reserve(apps.size());
    for (const auto& app : apps) {
      plugins.push_back({app.id, app.name, app.provider});
    }
    M4HomeBookDetailMeta::setInstalledPlugins(std::move(plugins));
  }

  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));

  for (RecentBook book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    if (M4ContentProvider::isHistoryUri(book.path.c_str())) {
      // Keep provider entries for history reopen (cache or app launch).
      if (!book.originalSourcePath.empty() && book.originalSourcePath.compare(0, 4, "app:") != 0 &&
          !SdMan.exists(book.originalSourcePath.c_str()) && book.author.find('.') == std::string::npos) {
        continue;
      }
      book.progress = loadBookProgress(book.originalSourcePath.empty() ? book.path : book.originalSourcePath);
      recentBooks.push_back(book);
      continue;
    }

    // Skip if file no longer exists
    if (!SdMan.exists(book.path.c_str())) {
      continue;
    }

    // 读取书籍进度
    book.progress = loadBookProgress(book.path);

    recentBooks.push_back(book);
  }
}

int HomeActivity::loadBookProgress(const std::string& path) {
  // 根据文件扩展名确定缓存路径和进度文件格式
  std::string cachePath;
  
  if (StringUtils::checkFileExtension(path, ".epub")) {
    // EPUB: 缓存路径在 /.crosspoint/epub_cache/{hash}/
    // 进度文件格式: spineIndex(2字节) + currentPage(2字节) + pageCount(2字节)
    Epub epub(path, "/.crosspoint");
    cachePath = epub.getCachePath();
    if (cachePath.empty()) return 0;
    
    std::string progressPath = cachePath + "/progress.bin";
    FsFile f;
    if (SdMan.openFileForRead("HAP", progressPath, f)) {
      uint8_t data[6];
      if (f.read(data, 6) == 6) {
        int spineIndex = data[0] | (data[1] << 8);
        int currentPage = data[2] | (data[3] << 8);
        int pageCount = data[4] | (data[5] << 8);
        f.close();
        
        // 计算总进度百分比
        if (epub.load(false, true)) {
          float chapterProgress = (pageCount > 0) ? (float)currentPage / pageCount : 0;
          float bookProgress = epub.calculateProgress(spineIndex, chapterProgress) * 100;
          return std::min(100, std::max(0, (int)(bookProgress + 0.5f)));
        }
      }
      f.close();
    }
  } else if (StringUtils::checkFileExtension(path, ".xtc") ||
             StringUtils::checkFileExtension(path, ".xtch")) {
    // XTC: 进度文件格式: currentPage(4字节) + m_loadedMax(4字节)
    Xtc xtc(path, "/.crosspoint");
    cachePath = xtc.getCachePath();
    if (cachePath.empty()) return 0;
    
    std::string progressPath = cachePath + "/progress.bin";
    FsFile f;
    if (SdMan.openFileForRead("HAP", progressPath, f)) {
      uint8_t data[8];
      if (f.read(data, 8) >= 4) {
        int currentPage = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
        f.close();
        
        // XTC 总页数需要从文件读取
        int totalPages = xtc.getPageCount();
        if (totalPages > 0) {
          return std::min(100, (currentPage * 100) / totalPages);
        }
      }
      f.close();
    }
  } else if (StringUtils::checkFileExtension(path, ".txt") ||
             StringUtils::checkFileExtension(path, ".md")) {
    // TXT progress: progress.dat (preferred) / progress.tmp / progress.bin
    // Format (8 bytes): page uint16 LE, pad, chapter uint16 LE, pad
    // (matches TxtReaderActivity::saveProgress / loadProgress)
    Txt txt(path, "/.crosspoint");
    if (!txt.load()) return 0;
    const size_t fileSize = txt.getFileSize();
    if (fileSize == 0) return 0;

    const std::string dir = txt.getCachePath();
    auto readProg = [&](const char* name, int& pageOut, int& chOut) -> bool {
      FsFile f;
      if (!SdMan.openFileForRead("HAP", (dir + name).c_str(), f)) return false;
      uint8_t data[8];
      const size_t n = f.read(data, 8);
      f.close();
      if (n != 8) return false;
      pageOut = data[0] | (data[1] << 8);
      chOut = data[4] | (data[5] << 8);
      return true;
    };

    int page = 0, ch = 0;
    if (!readProg("/progress.dat", page, ch) && !readProg("/progress.tmp", page, ch) &&
        !readProg("/progress.bin", page, ch)) {
      return 0;
    }
    if (ch < 0 || ch > 5000) return 0;
    if (page < 0) page = 0;

    // Prefer byte position from chapter batch cache (no full-file scan on home).
    const int batch = (ch / 25) * 25;
    size_t bytePos = 0;
    bool haveOffset = false;
    if (txt.hasChapterBatchCache(batch)) {
      txt.parseChapterIndexAndOffset(batch, /*allowScan=*/false);
      if (txt.isChapterExist(ch)) {
        const uint32_t begin = txt.getChapterOffsetByIndex(ch);
        uint32_t end = txt.getChapterendOffsetByIndex(ch);
        if (end == 0 || end <= begin) end = static_cast<uint32_t>(fileSize);
        // Within-chapter fraction from page index when available is ideal; without
        // parsing chapterN.bin headers, use a smooth page curve.
        float frac = 0.f;
        if (page > 0) {
          frac = std::min(0.95f, static_cast<float>(page) / (static_cast<float>(page) + 12.0f));
        }
        bytePos = begin + static_cast<size_t>((static_cast<double>(end - begin) * frac));
        haveOffset = true;
      }
    }
    if (!haveOffset) {
      // Fallback: chapter index vs highest cached batch (still no full scan).
      int maxCh = ch;
      for (int b = 0; b <= 2500; b += 25) {
        if (!txt.hasChapterBatchCache(b)) {
          if (b > batch) break;
          continue;
        }
        txt.parseChapterIndexAndOffset(b, /*allowScan=*/false);
        for (int i = 0; i < 25; ++i) {
          if (txt.isChapterExist(b + i)) maxCh = b + i;
        }
      }
      float p = static_cast<float>(ch) / static_cast<float>(maxCh + 1);
      p += (1.0f / static_cast<float>(maxCh + 1)) * std::min(0.9f, page / 30.0f);
      return std::min(100, std::max(0, static_cast<int>(p * 100.0f + 0.5f)));
    }
    if (bytePos >= fileSize) bytePos = fileSize - 1;
    return std::min(100, std::max(0, static_cast<int>(bytePos * 100.0 / static_cast<double>(fileSize) + 0.5)));
  }
  
  return 0;
}

void HomeActivity::loadRecentCovers(int coverWidth, int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  int progress = 0;
  for (RecentBook& book : recentBooks) {
    if (!book.coverBmpPath.empty()) {
      std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverWidth, coverHeight);
      if (!SdMan.exists(coverPath.c_str())) {
        // If epub, try to load the metadata for title/author and cover
        if (StringUtils::checkFileExtension(book.path, ".epub")) {
          {
            Epub epub(book.path, "/.crosspoint");
            // Skip loading css since we only need metadata here
            epub.load(false, true);

            // Try to generate thumbnail image for Continue Reading card
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, "生成封面中...");
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
            bool success = epub.generateThumbBmp(coverWidth, coverHeight);
            if (!success) {
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
              book.coverBmpPath = "";
            }
          }  // Epub 对象在此析构，释放内存
        } else if (StringUtils::checkFileExtension(book.path, ".xtch") ||
                   StringUtils::checkFileExtension(book.path, ".xtc")) {
          // XTC files use default cover, no generation needed
          RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
          book.coverBmpPath = "";
        }
      }
    }
    progress++;
  }

  recentsLoaded = true;
  recentsLoading = false;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  // 强制竖屏（防止阅读器横屏后未正常退出导致首页横屏）
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  renderer.setRenderMode(GfxRenderer::BW);

  // Normal boot still uses the established HALF first paint. Returns from
  // another activity opt into the explicit bottom-to-top transition below.
  firstRenderDone = false;

  showMemWarning = false;
  memWarningSelected = false;

  // Low-memory warning: use the CURRENT internal heap state, not the
  // lifetime minimum (ESP.getMinFreeHeap() is the all-time low — a transient
  // 3.4KB dip during a chapter open would then keep warning long after the
  // heap recovered to 80KB+, a false positive). Warn only when free internal
  // RAM is currently critical or the largest free block can't serve a TLS
  // handshake (~40KB gate).
  const uint32_t freeInternal =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const uint32_t largestInternal =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  Serial.printf("[%lu] [Home] internal free=%lu largest=%lu lifetime_min=%lu\n", millis(),
                static_cast<unsigned long>(freeInternal),
                static_cast<unsigned long>(largestInternal),
                static_cast<unsigned long>(ESP.getMinFreeHeap()));
  if (freeInternal < 20 * 1024 || largestInternal < 12 * 1024) {
    showMemWarning = true;
    Serial.printf("[%lu] [Home] Low memory warning triggered (free=%lu largest=%lu)\n", millis(),
                  static_cast<unsigned long>(freeInternal),
                  static_cast<unsigned long>(largestInternal));
  }

  renderingMutex = xSemaphoreCreateMutex();

  // Check if OPDS browser URL is configured
  hasOpdsUrl = strlen(SETTINGS.opdsServerUrl) > 0;
  hasjianguoUrl = strlen(SETTINGS.jgUsername) > 0;
  hasDataCapsuleUrl = strlen(SETTINGS.dcUsername) > 0;  // 数据胶囊配置检查
  hasBookmarkNotes = BookmarkStore::hasAnyBookmarks();  // 书签笔记检查

  selectorIndex = 0;

  auto metrics = UITheme::getInstance().getMetrics();
  loadRecentBooks(metrics.homeRecentBooksCount);

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&HomeActivity::taskTrampoline, "HomeActivityTask",
              8192,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  size_t xByteOffset = 0;
  size_t yStart = 0;
  size_t rowBytes = 0;
  size_t rows = 0;
  if (!computeCoverBufferLayout(xByteOffset, yStart, rowBytes, rows)) {
    return false;
  }

  const size_t bufferSize = rowBytes * rows;
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    return false;
  }

  // Free any existing buffer first
  freeCoverBuffer();

  coverBuffer = static_cast<uint8_t*>(malloc(bufferSize));
  if (!coverBuffer) {
    return false;
  }

  for (size_t row = 0; row < rows; ++row) {
    const size_t sourceOffset = (yStart + row) * HalDisplay::DISPLAY_WIDTH_BYTES + xByteOffset;
    memcpy(coverBuffer + row * rowBytes, frameBuffer + sourceOffset, rowBytes);
  }
  coverBufferXByteOffset = xByteOffset;
  coverBufferYStart = yStart;
  coverBufferRowBytes = rowBytes;
  coverBufferRows = rows;
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer) {
    return false;
  }

  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    return false;
  }

  if (coverBufferRowBytes == 0 || coverBufferRows == 0 ||
      coverBufferXByteOffset + coverBufferRowBytes > HalDisplay::DISPLAY_WIDTH_BYTES ||
      coverBufferYStart + coverBufferRows > HalDisplay::DISPLAY_HEIGHT) {
    return false;
  }

  for (size_t row = 0; row < coverBufferRows; ++row) {
    const size_t destOffset = (coverBufferYStart + row) * HalDisplay::DISPLAY_WIDTH_BYTES + coverBufferXByteOffset;
    memcpy(frameBuffer + destOffset, coverBuffer + row * coverBufferRowBytes, coverBufferRowBytes);
  }
  return true;
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferRowBytes = 0;
  coverBufferRows = 0;
  coverBufferXByteOffset = 0;
  coverBufferYStart = 0;
  coverBufferStored = false;
}

bool HomeActivity::computeCoverBufferLayout(size_t& xByteOffset, size_t& yStart, size_t& rowBytes, size_t& rows) const {
  const auto metrics = UITheme::getInstance().getMetrics();
  const int logicalStripY = metrics.homeTopPadding;
  const int logicalStripHeight = metrics.homeCoverTileHeight;
  if (logicalStripY < 0 || logicalStripHeight <= 0) {
    return false;
  }

  switch (renderer.getOrientation()) {
    case GfxRenderer::Portrait: {
      const int physicalX = logicalStripY;
      const int physicalWidth = logicalStripHeight;
      xByteOffset = physicalX / 8;
      yStart = 0;
      rowBytes = (physicalWidth + 7) / 8;
      rows = HalDisplay::DISPLAY_HEIGHT;
      break;
    }
    case GfxRenderer::PortraitInverted: {
      const int physicalX = HalDisplay::DISPLAY_WIDTH - logicalStripY - logicalStripHeight;
      const int physicalWidth = logicalStripHeight;
      xByteOffset = physicalX / 8;
      yStart = 0;
      rowBytes = (physicalWidth + 7) / 8;
      rows = HalDisplay::DISPLAY_HEIGHT;
      break;
    }
    case GfxRenderer::LandscapeCounterClockwise: {
      xByteOffset = 0;
      yStart = logicalStripY;
      rowBytes = HalDisplay::DISPLAY_WIDTH_BYTES;
      rows = logicalStripHeight;
      break;
    }
    case GfxRenderer::LandscapeClockwise: {
      xByteOffset = 0;
      yStart = HalDisplay::DISPLAY_HEIGHT - logicalStripY - logicalStripHeight;
      rowBytes = HalDisplay::DISPLAY_WIDTH_BYTES;
      rows = logicalStripHeight;
      break;
    }
  }

  if (xByteOffset >= HalDisplay::DISPLAY_WIDTH_BYTES || yStart >= HalDisplay::DISPLAY_HEIGHT || rowBytes == 0 ||
      rows == 0 || xByteOffset + rowBytes > HalDisplay::DISPLAY_WIDTH_BYTES || yStart + rows > HalDisplay::DISPLAY_HEIGHT) {
    return false;
  }

  return true;
}

void HomeActivity::loop() {
  // Handle low memory warning dialog first
  if (showMemWarning) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Left) ||
        mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      memWarningSelected = false;
      updateRequired = true;
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Right) ||
               mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      memWarningSelected = true;
      updateRequired = true;
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (memWarningSelected) {
        Serial.printf("[%lu] [Home] User confirmed restart due to low memory\n", millis());
        ESP.restart();
      } else {
        showMemWarning = false;
        updateRequired = true;
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      showMemWarning = false;
      updateRequired = true;
    }
    return;
  }

  auto activateSelection = [this]() {
    int idx = 0;
    int menuSelectedIndex = selectorIndex - static_cast<int>(recentBooks.size());
    const int myLibraryIdx = idx++;
    const int recentsIdx = idx++;
    const int opdsLibraryIdx = hasOpdsUrl ? idx++ : -1;
    const int jgLibraryIdx = hasjianguoUrl ? idx++ : -1;
    const int dcLibraryIdx = hasDataCapsuleUrl ? idx++ : -1;
    const int bookmarkNotesIdx = hasBookmarkNotes ? idx++ : -1;
    const int fileTransferIdx = idx++;
    const int appsIdx = idx++;
    const int settingsIdx = idx;

    if (selectorIndex < static_cast<int>(recentBooks.size())) {
      const auto& b = recentBooks[selectorIndex];
      std::string src = b.originalSourcePath;
      if (M4ContentProvider::isHistoryUri(b.path.c_str()) &&
          (src.empty() || !SdMan.exists(src.c_str())) && b.author.find('.') != std::string::npos) {
        src = std::string("app:") + b.author;
      }
      onSelectBook(b.path, src);
    } else if (menuSelectedIndex == myLibraryIdx) {
      onMyLibraryOpen();
    } else if (menuSelectedIndex == recentsIdx) {
      onRecentsOpen();
    } else if (menuSelectedIndex == opdsLibraryIdx) {
      onOpdsBrowserOpen();
    } else if (menuSelectedIndex == jgLibraryIdx) {
      onJianGuoYunOpen();
    } else if (menuSelectedIndex == dcLibraryIdx) {
      onDataCapsuleOpen();
    } else if (menuSelectedIndex == bookmarkNotesIdx) {
      onBookmarkNotesOpen();
    } else if (menuSelectedIndex == fileTransferIdx) {
      onFileTransferOpen();
    } else if (menuSelectedIndex == appsIdx) {
      onAppsOpen();
    } else if (menuSelectedIndex == settingsIdx) {
      onSettingsOpen();
    }
  };

  // Touch: shared geometry with render() for covers and menu tiles.
  if (mappedInput.hasTouch()) {
    const auto metrics = UITheme::getInstance().getMetrics();
    const int pageWidth = renderer.getScreenWidth();
    const int pageHeight = renderer.getScreenHeight();
    const TouchHitGeometry::Rect coverRect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight};
    const TouchHitGeometry::Rect menuRect{
        0, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.verticalSpacing, pageWidth,
        pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing * 2 +
                      metrics.buttonHintsHeight)};
    const int coverCount = static_cast<int>(recentBooks.size());
    const int menuCount = getMenuItemCount();
    const int renderedMenuCount = menuCount - coverCount;
    constexpr int hPaddingInSelection = 8;

    int tx = 0;
    int ty = 0;
    if (mappedInput.wasScreenTouchDown(tx, ty)) {
      int hit = -1;
      if (coverCount > 0 &&
          TouchHitGeometry::fengyanCoverIndexFromPoint(coverRect, coverCount, metrics.contentSidePadding,
                                                       metrics.homeCoverHeight, hPaddingInSelection, tx, ty, hit)) {
        if (selectorIndex != hit) {
          selectorIndex = hit;
          updateRequired = true;
        }
        return;
      }
      if (renderedMenuCount > 0 &&
          TouchHitGeometry::fengyanMenuIndexFromPoint(menuRect, renderedMenuCount, tx, ty, hit,
                                                      -6, metrics.contentSidePadding)) {
        const int touched = coverCount + hit;
        if (selectorIndex != touched) {
          selectorIndex = touched;
          updateRequired = true;
        }
        return;
      }
    }
    if (mappedInput.wasScreenTapped(tx, ty)) {
      int hit = -1;
      if (coverCount > 0 &&
          TouchHitGeometry::fengyanCoverIndexFromPoint(coverRect, coverCount, metrics.contentSidePadding,
                                                       metrics.homeCoverHeight, hPaddingInSelection, tx, ty, hit)) {
        selectorIndex = hit;
        activateSelection();
        return;
      }
      if (renderedMenuCount > 0 &&
          TouchHitGeometry::fengyanMenuIndexFromPoint(menuRect, renderedMenuCount, tx, ty, hit,
                                                      -6, metrics.contentSidePadding)) {
        selectorIndex = coverCount + hit;
        activateSelection();
        return;
      }
    }

    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Left) {
      selectorIndex = (selectorIndex + 1) % menuCount;
      updateRequired = true;
      return;
    }
    if (swipe == MappedInputManager::SwipeDir::Down || swipe == MappedInputManager::SwipeDir::Right) {
      selectorIndex = (selectorIndex + menuCount - 1) % menuCount;
      updateRequired = true;
      return;
    }
  }

  const bool prevPressed = mappedInput.wasPressed(MappedInputManager::Button::Up) ||
                           mappedInput.wasPressed(MappedInputManager::Button::Left);
  const bool nextPressed = mappedInput.wasPressed(MappedInputManager::Button::Down) ||
                           mappedInput.wasPressed(MappedInputManager::Button::Right);

  const int menuCount = getMenuItemCount();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelection();
  } else if (prevPressed) {
    selectorIndex = (selectorIndex + menuCount - 1) % menuCount;
    updateRequired = true;
  } else if (nextPressed) {
    selectorIndex = (selectorIndex + 1) % menuCount;
    updateRequired = true;
  }
}

void HomeActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void HomeActivity::renderMemWarning() {
  renderer.clearScreen();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);

  const char* line1 = L(Str::kMemLowWarning);
  const char* line2 = L(Str::kMemLowReboot);
  const char* cancelText = L(Str::kCancel);
  const char* restartText = L(Str::kRebootNow);

  const int line1Width = M4UiText::textWidth(renderer, UI_12_FONT_ID, line1);
  const int line2Width = M4UiText::textWidth(renderer, UI_12_FONT_ID, line2);
  const int cancelWidth = M4UiText::textWidth(renderer, UI_12_FONT_ID, cancelText);
  const int restartWidth = M4UiText::textWidth(renderer, UI_12_FONT_ID, restartText);

  constexpr int padding = 20;
  constexpr int btnSpacing = 30;
  const int btnRowWidth = cancelWidth + btnSpacing + restartWidth;
  int contentWidth = line1Width;
  if (line2Width > contentWidth) contentWidth = line2Width;
  if (btnRowWidth > contentWidth) contentWidth = btnRowWidth;

  const int boxW = contentWidth + padding * 2;
  const int boxH = lineHeight + 8 + lineHeight + 8 + lineHeight + padding * 3;
  const int boxX = (pageWidth - boxW) / 2;
  const int boxY = (pageHeight - boxH) / 2;

  // Background and border
  renderer.fillRect(boxX - 3, boxY - 3, boxW + 6, boxH + 6, false);
  renderer.drawRect(boxX, boxY, boxW, boxH, true);

  // Message lines
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, boxY + padding, line1);
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, boxY + padding + lineHeight + 8, line2);

  // Buttons
  const int btnY = boxY + boxH - padding - lineHeight;
  const int cancelX = (pageWidth - btnRowWidth) / 2;
  const int restartX = cancelX + cancelWidth + btnSpacing;

  if (!memWarningSelected) {
    // "取消" selected
    renderer.fillRect(cancelX - 4, btnY - 2, cancelWidth + 8, lineHeight + 4, true);
    M4UiText::draw(renderer, UI_12_FONT_ID, cancelX, btnY, cancelText, false);
    M4UiText::draw(renderer, UI_12_FONT_ID, restartX, btnY, restartText, true);
  } else {
    // "立即重启" selected
    M4UiText::draw(renderer, UI_12_FONT_ID, cancelX, btnY, cancelText, true);
    renderer.fillRect(restartX - 4, btnY - 2, restartWidth + 8, lineHeight + 4, true);
    M4UiText::draw(renderer, UI_12_FONT_ID, restartX, btnY, restartText, false);
  }

  renderer.displayBuffer();
}

void HomeActivity::render() {
  // Show memory warning dialog if triggered
  if (showMemWarning) {
    renderMemWarning();
    return;
  }

  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Navigation surfaces use a single fast/partial frame. Reader-body page
  // pagination owns the only windowed animation path.
  const bool animateHomeEntry = animateEntry && !firstRenderDone;

  renderer.clearScreen();
  // Only restore the saved cover buffer when covers don't need re-rendering.
  // If coverRendered=false the saved buffer is stale (e.g. contains the "no cover" black block
  // from a previous render before the thumbnail was generated). Restoring it would pollute the
  // cleared white background, and since drawBitmap1Bit only draws black pixels (white pixels are
  // transparent), the old black fill would bleed through the light areas of the new thumbnail.
  const bool shouldRestoreBuffer = coverRendered && coverBufferStored;
  bool bufferRestored = shouldRestoreBuffer && restoreCoverBuffer();

  // Keep the home status bar clean; the legacy quote/custom-status-bar
  // feature has been removed, matching the original CrossLink home layout.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, nullptr);

  GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                          recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this));

  // 菜单项：一行两个的网格布局
  std::vector<const char*> menuItems = {L(Str::kFileManager), L(Str::kReadingHistory)};
  
  // 检测当前主题类型，Fengyan主题使用32x32图标
  bool isFengyanTheme = (UITheme::getInstance().getThemeType() == ThemeType::Fengyan);
  
  std::vector<UIIcon> menuIcons;
  if (isFengyanTheme) {
    menuIcons = {UIIcon::Folder32, UIIcon::History32};
  } else {
    menuIcons = {UIIcon::Library, UIIcon::Recent};
  }

  if (hasOpdsUrl) {
      menuItems.push_back(L(Str::kOPDSBrowser));
      menuIcons.push_back(isFengyanTheme ? UIIcon::Netdisk32 : UIIcon::Hotspot);
  }
  if (hasjianguoUrl) {
      menuItems.push_back(L(Str::kJianGuoDisk));
      menuIcons.push_back(isFengyanTheme ? UIIcon::Netdisk32 : UIIcon::Transfer);
  }
  if (hasDataCapsuleUrl) {
      menuItems.push_back(L(Str::kDataCapsule));
      menuIcons.push_back(isFengyanTheme ? UIIcon::Netdisk32 : UIIcon::Cog);
  }
  if (hasBookmarkNotes) {
      menuItems.push_back(L(Str::kBookmarkNotes));
      menuIcons.push_back(isFengyanTheme ? UIIcon::Shuqian32 : UIIcon::Book);
  }
  menuItems.push_back(L(Str::kNetworkManage));
  menuIcons.push_back(isFengyanTheme ? UIIcon::Wifi32 : UIIcon::Wifi);
  menuItems.push_back(L(Str::kApps));
  // A dedicated 3x3 app-grid glyph is drawn by FengyanTheme; Lyra uses the
  // library glyph instead of the old gear icon, which looked like Settings.
  menuIcons.push_back(isFengyanTheme ? UIIcon::Apps32 : UIIcon::Library);
  menuItems.push_back(L(Str::kSystemSettings));
  menuIcons.push_back(isFengyanTheme ? UIIcon::Setting32 : UIIcon::Settings);

  // 计算菜单区域
  Rect menuRect = Rect{0,
                       metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.verticalSpacing,
                       pageWidth,
                       pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing * 2 +
                                     metrics.buttonHintsHeight)};

  // 使用网格布局绘制菜单（一行两个）
  GUI.drawButtonMenu(
      renderer,
      menuRect,
      static_cast<int>(menuItems.size()),
      selectorIndex - recentBooks.size(),
      [&menuItems](int index) { return std::string(menuItems[index]); },
      [&menuIcons](int index) -> UIIcon { return menuIcons[index]; });

  const auto labels = mappedInput.mapLabels("", L(Str::kSelect), L(Str::kMoveUp), L(Str::kMoveDown));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  (void)animateHomeEntry;
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  (void)renderer.storeLastShown();

  if (!firstRenderDone) {
    firstRenderDone = true;
    updateRequired = true;
  } else if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    loadRecentCovers(metrics.homeCoverWidth, metrics.homeCoverThumbHeight);
    // 封面生成完成，重置渲染状态使下次渲染重新从 SD 卡读取新生成的缩略图
    coverRendered = false;
    coverBufferStored = false;
    freeCoverBuffer();
    updateRequired = true;
  }
}
