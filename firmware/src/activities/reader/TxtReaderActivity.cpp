#include "TxtReaderActivity.h"

#include <GfxRenderer.h>
#include <EpdFontLoader.h>
#include <BluetoothHIDManager.h>
#include <SDCardManager.h>
#include <Serialization.h>
#include <Utf8.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "I18n.h"
#include "BookmarkStore.h"
#include "GbkToUtf8.h"
#include "M4TxtEncoding.h"
#include "MappedInputManager.h"
#include "ReadingStatsStore.h"
#include "util/M4PluginReaderBridge.h"
#include "apps/M4PluginReaderSession.h"
#include "util/M4PluginTocList.h"
#include "util/M4PluginTidxCodec.h"
#include "util/M4PluginReaderStatePolicy.h"
#include "util/M4ProgressiveTxtIndex.h"
#include "debug/M4WaveformLab.h"
#include <HalDisplay.h>
#include <cstring>
#include "esp_heap_caps.h"
#include "util/M4ContentProviderContract.h"
#include "util/M4HistoryReopen.h"
#include "apps/M4ContentProviderSession.h"
#include "apps/providers/M4NativeProviderManager.h"
#include "apps/providers/M4NativeWifi.h"
#include "RecentBooksStore.h"

#ifdef CROSSPOINT_X3
#include "TiltPageTurnDetector.h"
extern TiltPageTurnDetector tiltDetector;
#endif
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4UiText.h"
#include "util/TouchHitGeometry.h"

#include "EpubReaderMenuActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderSettingsActivity.h"
#include "BookmarkManagerActivity.h"
#include "TxtReaderChapterSelectionActivity.h"

#include <sys/time.h>
#include <ctime>

#ifdef CROSSPOINT_X3
#include "DS3231RTC.h"
#endif

bool TxtReaderActivity::isLandscapeDualPage() const {
  return SETTINGS.landscapeDualPageEnabled &&
         (renderer.getOrientation() == GfxRenderer::LandscapeClockwise ||
          renderer.getOrientation() == GfxRenderer::LandscapeCounterClockwise);
}

namespace {
constexpr unsigned long goHomeMs = 1000;
constexpr int statusBarMargin = 20;
constexpr int progressBarMarginTop = 1;
constexpr int progressBarBottomGap = 5;      // 进度条距屏幕底部间距（与 EPUB 一致）
constexpr int progressBarTextGap = 1;        // 文字底部距进度条顶部间距（与 EPUB 一致）
constexpr size_t CHUNK_SIZE = 8 * 1024;           // default per-call read window
constexpr size_t kMaxPageReadBytes = 48 * 1024;  // hard cap for first-page adaptive buffer (heap; no PSRAM)
constexpr size_t kLargeTxtDirectThreshold = 4 * 1024 * 1024;
constexpr size_t kLegacyProgressDataBytes = 8;
constexpr size_t kProgressDataBytes = 20;

// PSRAM-first scratch for GBK/UTF-16 decode windows (up to ~144KB). On
// internal RAM these resizes fail while the TTF face is resident (~70-130KB
// idle heap) → loadPageAtOffset false → firstFrameHasLines false → physical
// refresh skipped ("two pages per refresh"). CPU-owned storage, no DMA.
template <typename T>
struct PsramVec {
  T* data = nullptr;
  size_t cap = 0;
  size_t len = 0;
  bool resize(size_t n) {
    if (n <= cap) return true;
    T* p = nullptr;
#if defined(ARDUINO_ARCH_ESP32)
    p = static_cast<T*>(heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!p) p = static_cast<T*>(malloc(n * sizeof(T)));
#else
    p = static_cast<T*>(malloc(n * sizeof(T)));
#endif
    if (!p) return false;
    free(data);
    data = p;
    cap = n;
    return true;
  }
  ~PsramVec() { free(data); }
};

// PSRAM-first raw page-window buffer. The 8-48KB read window on internal RAM
// failed intermittently while the TTF face was resident, leaving loadPageAtOffset
// to return false and the physical refresh to be skipped (every-other-page
// refresh). SDMMC DMA on ESP32-S3 reaches PSRAM, so direct reads are safe.
inline uint8_t* PsramRawAlloc(size_t n) {
  if (n == 0) return nullptr;
#if defined(ARDUINO_ARCH_ESP32)
  uint8_t* p = static_cast<uint8_t*>(heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!p) p = static_cast<uint8_t*>(malloc(n));
  return p;
#else
  return static_cast<uint8_t*>(malloc(n));
#endif
}

// Cache file magic and version
constexpr uint32_t CACHE_MAGIC = 0x54585449;  // "TXTI"
// v10: raw-source encoding-aware page offsets + encodingType header field.
// Rebuilds any v9-or-earlier caches (may have mixed utf8-cache offsets).
constexpr uint8_t CACHE_VERSION = 10;

// ── Clock display helpers ───────────────────────────────────────────────────
constexpr time_t VALID_TIME_THRESHOLD = 1704067200;  // 2024-01-01 00:00:00 UTC
constexpr int UTC8_OFFSET = 8 * 3600;
constexpr int clockIconRadius = 9;
constexpr int clockIconSize = clockIconRadius * 2;
constexpr int clockBatterySpacing = 8;
constexpr int clockIconTextSpacing = 4;

std::string getClockTimeString() {
  time_t utcTime = 0;
#ifdef CROSSPOINT_X3
  utcTime = DS3231RTC::readTime();
  if (utcTime < VALID_TIME_THRESHOLD) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    utcTime = tv.tv_sec;
  }
#else
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  utcTime = tv.tv_sec;
#endif
  if (utcTime < VALID_TIME_THRESHOLD) {
    return "--:--";
  }
  // 设置时区后 localtime_r 会自动处理时区转换，不再需要手动 +UTC8_OFFSET
  struct tm tmInfo;
  localtime_r(&utcTime, &tmInfo);
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", tmInfo.tm_hour, tmInfo.tm_min);
  return std::string(buf);
}

void drawClockIcon(const GfxRenderer& renderer, int cx, int cy, int radius) {
  // 双层 Bresenham 圆（加粗表盘轮廓）
  for (int r = radius; r >= radius - 1; r--) {
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
      renderer.drawPixel(cx + x, cy + y);
      renderer.drawPixel(cx + y, cy + x);
      renderer.drawPixel(cx - y, cy + x);
      renderer.drawPixel(cx - x, cy + y);
      renderer.drawPixel(cx - x, cy - y);
      renderer.drawPixel(cx - y, cy - x);
      renderer.drawPixel(cx + y, cy - x);
      renderer.drawPixel(cx + x, cy - y);
      y++;
      if (err < 0) {
        err += 2 * y + 1;
      } else {
        x--;
        err += 2 * (y - x) + 1;
      }
    }
  }
  // 用 fillRect 画指针（墨水屏上填充矩形最清晰）
  // 指针整体右下偏移1px，对齐圆的视觉中心
  int pcx = cx + 1, pcy = cy + 1;
  // 时针：9点方向（向左）
  int hLen = radius * 2 / 3;
  renderer.fillRect(pcx - hLen, pcy - 1, hLen + 2, 3, true);
  // 分针：12点方向（向上）
  int mLen = radius - 2;
  renderer.fillRect(pcx - 1, pcy - mLen, 3, mLen + 2, true);
  // 中心点 3x3
  renderer.fillRect(pcx - 1, pcy - 1, 3, 3, true);
}

// 已移除 convertChinesePunctToHalfWidth 函数：
// 保持原始中文标点渲染，使换行计算与渲染使用相同文本，消除行末空白。

// ========== 中文标点全宽检测（与 EPUB ParsedText 一致） ==========
// 判断一个 Unicode 码点是否为中文全角标点（标准模式下需占用一个汉字宽度）
// 范围参考 EPUB 的 isChinesePunctForFullWidth()：
//   U+3000-U+303F: CJK 符号和标点（。、《》〈〉「」【】等）
//   U+FF00-U+FFEF: 全角形式（，！？；：（）等）
//   U+2018-U+201D: 弯引号（''""）
//   U+2014: 破折号（—）  U+2013: 短破折号（–）  U+2026: 省略号（…）
bool isChinesePunctCodepoint(uint32_t cp) {
  return (cp >= 0x3000 && cp <= 0x303F) ||
         (cp >= 0xFF00 && cp <= 0xFFEF) ||
         (cp >= 0x2018 && cp <= 0x201D) ||
         cp == 0x2014 || cp == 0x2013 ||
         cp == 0x2026;
}

// 计算标准模式下中文标点的宽度补偿值
// 标准模式（PUNCT_STANDARD）：将中文标点宽度补偿到汉字宽度
// 紧凑模式（PUNCT_COMPACT）：返回 0，不做补偿
int calculatePunctWidthAdjustment(const GfxRenderer& renderer, int fontId, const std::string& line) {
  if (SETTINGS.chinesePunctWidth != CrossPointSettings::PUNCT_STANDARD) {
    return 0;  // 紧凑模式，无需补偿
  }

  // 汉字参考宽度（与 EPUB 一致，使用"我"字）
  const int cjkCharWidth = renderer.getTextWidth(fontId, "\xe6\x88\x91");
  if (cjkCharWidth <= 0) return 0;

  int adjustment = 0;
  size_t i = 0;
  while (i < line.size()) {
    uint8_t c = static_cast<uint8_t>(line[i]);
    uint32_t cp = 0;
    size_t charLen = 1;

    // UTF-8 解码
    if (c >= 0xF0 && i + 3 < line.size()) {
      cp = ((c & 0x07) << 18) |
           ((static_cast<uint8_t>(line[i + 1]) & 0x3F) << 12) |
           ((static_cast<uint8_t>(line[i + 2]) & 0x3F) << 6) |
           (static_cast<uint8_t>(line[i + 3]) & 0x3F);
      charLen = 4;
    } else if (c >= 0xE0 && i + 2 < line.size()) {
      cp = ((c & 0x0F) << 12) |
           ((static_cast<uint8_t>(line[i + 1]) & 0x3F) << 6) |
           (static_cast<uint8_t>(line[i + 2]) & 0x3F);
      charLen = 3;
    } else if (c >= 0xC0 && i + 1 < line.size()) {
      cp = ((c & 0x1F) << 6) |
           (static_cast<uint8_t>(line[i + 1]) & 0x3F);
      charLen = 2;
    } else {
      cp = c;
      charLen = 1;
    }

    // 检测是否为中文全角标点
    if (isChinesePunctCodepoint(cp)) {
      // 获取该标点的字体自然宽度
      std::string punctStr = line.substr(i, charLen);
      int naturalWidth = renderer.getTextWidth(fontId, punctStr.c_str());
      // 仅当自然宽度小于汉字宽度时才补偿（与 EPUB 逻辑一致）
      if (naturalWidth < cjkCharWidth) {
        adjustment += (cjkCharWidth - naturalWidth);
      }
    }

    i += charLen;
  }

  return adjustment;
}

// Legacy JSON catalogs are small and remain supported as an eager vector.
// File-backed provider catalogs use openPluginPagedTitles below so the system
// chapter list never copies the complete TOC into RAM.
bool loadPluginTitles(const TxtReaderActivity::PluginSession& session,
                      std::vector<std::string>& titlesOut) {
  titlesOut.clear();
  return !session.tocAbsPath.empty() &&
         M4PluginTocList::loadTitlesFromFile(session.tocAbsPath, titlesOut) && !titlesOut.empty();
}

std::shared_ptr<M4PluginTocList::PagedTitleSource> openPluginPagedTitles(
    const TxtReaderActivity::PluginSession& session) {
  if (!session.providerManaged || session.providerId.empty() || session.bookId.empty() ||
      session.appDataRoot.empty()) {
    return {};
  }
  M4ContentProvider::ChapterCatalogSpec catalog;
  if (!M4ContentProviderSession::catalogFor(session.providerId, session.bookId,
                                            session.chapterIndex, catalog)) {
    return {};
  }
  std::string absPath;
  if (M4PluginReaderBridge::resolveUnderDataRoot(session.appDataRoot,
                                                  catalog.fileRelPath.c_str(), absPath) !=
      M4PluginReaderBridge::OpenError::Ok) {
    return {};
  }
  return M4PluginTocList::openPagedFileRows(absPath, catalog);
}

bool resolvePluginTitle(const TxtReaderActivity::PluginSession& session, int index0,
                        std::string& titleOut) {
  titleOut.clear();
  std::vector<std::string> legacyTitles;
  if (loadPluginTitles(session, legacyTitles) && index0 >= 0 &&
      index0 < static_cast<int>(legacyTitles.size())) {
    titleOut = legacyTitles[static_cast<size_t>(index0)];
    return !titleOut.empty();
  }
  const auto paged = openPluginPagedTitles(session);
  if (!paged || index0 < 0 || index0 >= static_cast<int>(paged->rowCount())) return false;
  std::vector<std::string> oneTitle;
  std::vector<uint8_t> present;
  if (!paged->loadPage(index0, 1, oneTitle, present) || oneTitle.empty() || present.empty() || !present[0]) {
    return false;
  }
  titleOut = std::move(oneTitle[0]);
  return !titleOut.empty();
}
// =================================================================

}  // namespace

void TxtReaderActivity::taskTrampoline(void* param) {
  auto* self = static_cast<TxtReaderActivity*>(param);
  self->displayTaskLoop();
}

void TxtReaderActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
 

  if (!txt) {
    return;
  }

  // TXT阅读器始终使用竖屏模式
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  renderingMutex = xSemaphoreCreateMutex();

  txt->setupCacheDir();
  largeTxtFastOpen_ = !pluginSession_.active && txt->getFileSize() >= kLargeTxtDirectThreshold;
  fastOpenChapter_ = -1;
  activeChapterBegin_ = 0;
  activeChapterEnd_ = 0;
  resumeRangeBegin_ = 0;
  resumeRangeEnd_ = 0;
  progressSavePending_ = false;
  openHistorySavePending_ = false;
  chapterDiscoveryBatch_ = largeTxtFastOpen_ ? 0 : -1;
  chapterDiscoveryDone_ = !largeTxtFastOpen_;
  // Plugin sessions: bridge raw-byte progress is authoritative — do not load
  // library progress.bin (would contradict plugin restore).
  if (!pluginSession_.active) {
    loadProgress();
  } else {
    chapter_initialized = false;
    currentPage = 0;
    chapternum = 0;
    if (pluginSession_.hasInitialByteOffset) {
      pendingRestoreByte_ = pluginSession_.initialByteOffset;
      hasPendingRestore_ = true;
      userMovedPage_ = false;
    } else {
      hasPendingRestore_ = false;
      pendingRestoreByte_ = 0;
    }
  }

  if (largeTxtFastOpen_) {
    fastOpenChapter_ = chapternum;
    Serial.printf("[%lu] [WR05] large_txt_reader size=%zu saved_chapter=%d\n", millis(),
                  txt->getFileSize(), fastOpenChapter_);
  }

  // 重新进入时强制重新初始化章节（屏幕方向/字体可能已变化）
  // 不调用 onSettingsChanged 因为它会重置 currentPage
  chapter_initialized = false;
  cachedPage = -1;

  // 处理从首页书签笔记跳转过来的情况 (library only)
  if (!pluginSession_.active && APP_STATE.pendingBookmarkPercent >= 0.0f) {
    m_pendingJumpPercent = static_cast<int>(APP_STATE.pendingBookmarkPercent * 100.0f + 0.5f);
    APP_STATE.pendingBookmarkPercent = -1.0f;
  }

  // Library path: remember last book + Recent Books. Both writes can block on
  // SD; keep only the in-memory open path on the critical path and flush after
  // the first readable page reaches the panel (or from onExit as a fallback).
  if (!pluginSession_.active || !pluginSession_.suppressOpenEpubPath) {
    APP_STATE.openEpubPath = txt->getPath();
    openHistorySavePending_ = true;
  }
  if (!pluginSession_.active || !pluginSession_.suppressRecentBooks) {
    openHistorySavePending_ = true;
  }

  firstPageReady_ = false;
  firstReadableLogged_ = false;
  // Progressive first-page for plugin and library; filled in chapter_initializeReader.
  indexComplete_ = false;
  pluginCloseRequested_ = false;
  providerOverlayMsg_.clear();
  providerOverlayState_ = M4ContentProvider::ChapterReady::Ready;
  providerPrefetchRequested_ = false;
  entryPlaceholderKind_ = EntryPlaceholderKind::None;
  // White seed replaces plugin half-flush handoff: wipe residual UI to pure
  // white, then first content page animates/FASTs from that white baseline.
  pluginNeedsClearRefresh_ = false;
  pluginPendingHalfFlush_ = false;
  armEntryWhiteSeed(EntryPlaceholderKind::Opening);

  if (pluginSession_.providerManaged && !pluginSession_.providerId.empty()) {
    M4ContentProviderSession::noteOpen(pluginSession_.providerId, pluginSession_.bookId,
                                      pluginSession_.chapterIndex, 0);
    // Mark current chapter Ready if we opened a real cache path.
    if (!pluginSession_.cacheRelPath.empty()) {
      M4ContentProvider::ChapterStatus st;
      st.providerId = pluginSession_.providerId;
      st.bookId = pluginSession_.bookId;
      st.chapterUid = pluginSession_.chapterUid;
      st.index0 = pluginSession_.chapterIndex;
      st.state = M4ContentProvider::ChapterReady::Ready;
      st.cacheRelPath = pluginSession_.cacheRelPath;
      (void)M4ContentProviderSession::setChapterStatus(st);
    }
  }

  // Trigger first update (white seed then content — see armEntryWhiteSeed).
  updateRequired = true;

  // 开始阅读统计会话
  READING_STATS.startSession();

  // 每次进入阅读器时重置自动翻页开关，需用户手动开启
  automaticPageTurnActive = false;
  rollingHalfTurned = false;
  if (SETTINGS.autoPageTurnEnabled) {
    SETTINGS.autoPageTurnEnabled = 0;
    SETTINGS.saveToFile();
  }

  xTaskCreate(&TxtReaderActivity::taskTrampoline, "TxtReaderActivityTask",
              8192,               // Stack size (increased for font loading + page indexing)
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void TxtReaderActivity::persistOpenHistory() {
  if (!openHistorySavePending_ || !txt) return;

  if (!pluginSession_.active || !pluginSession_.suppressOpenEpubPath) {
    APP_STATE.saveToFile();
  }
  if (!pluginSession_.active || !pluginSession_.suppressRecentBooks) {
    const auto filePath = txt->getPath();
    const auto fileName = filePath.substr(filePath.rfind('/') + 1);
    // Provider books use stable history URI so reopen bypasses plugin shelf.
    if (pluginSession_.providerManaged && !pluginSession_.providerId.empty()) {
      const std::string uri = M4ContentProvider::makeHistoryUri(pluginSession_.providerId.c_str(),
                                                                pluginSession_.bookId.c_str());
      // Metadata contract: RecentBook.author = m4x appId (com.weread.client), never
      // providerId ("weread"). originalSourcePath = last chapter cache abs path.
      std::string appId;
      if (!M4HistoryReopen::resolveHistoryAppId(pluginSession_.appId, pluginSession_.appDataRoot, filePath,
                                                appId)) {
        Serial.printf("[WRCP] history_skip_uri no_appId provider=%s book=%s\n",
                      pluginSession_.providerId.c_str(), pluginSession_.bookId.c_str());
        RECENT_BOOKS.addBook(filePath, fileName, "", "");
      } else if (!uri.empty()) {
        // Metadata contract: provider title wins over chapter/cache filenames.
        const auto providerHistory =
            M4ContentProviderSession::makeHistorySnapshot(pluginSession_.providerId, pluginSession_.bookId);
        const std::string historyTitle = !providerHistory.title.empty()
                                             ? providerHistory.title
                                             : (pluginSession_.titleOverride.empty() ? fileName
                                                                                       : pluginSession_.titleOverride);
        RECENT_BOOKS.addBook(uri, historyTitle, appId, "", filePath);
        M4ContentProviderSession::markHistoryRegistered(pluginSession_.providerId, pluginSession_.bookId);
        Serial.printf("[WRCP] history_uri=%s appId=%s cache=%s\n", uri.c_str(), appId.c_str(), filePath.c_str());
      } else {
        RECENT_BOOKS.addBook(filePath, fileName, "", "");
      }
    } else {
      RECENT_BOOKS.addBook(filePath, fileName, "", "");
    }
  }
  openHistorySavePending_ = false;
}

void TxtReaderActivity::waitPhysicalEpdIdle(uint32_t maxMs) {
  const uint32_t t0 = millis();
  while (physicalEpdBusy_ && (millis() - t0) < maxMs) {
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void TxtReaderActivity::onExit() {
  ActivityWithSubactivity::onExit();

  // 结束阅读统计会话并保存
  READING_STATS.endSession();

  // Stop display task from starting another AA/e-ink pass, then wait so we do
  // not vTaskDelete mid-displayGrayBuffer (left panel in gray → Home "全刷" flash).
  // Chapter switch: Lua paints loading next — short wait so UI feels responsive.
  suppressDisplay_ = true;
  updateRequired = false;
  const uint32_t epdWaitMs =
      (pluginSession_.active && pluginSwitchChapterIndex_ >= 0) ? 400u : 2500u;
  waitPhysicalEpdIdle(epdWaitMs);

  // If the reader is closed before its first physical page, preserve the
  // existing history semantics before tearing down the TXT object.
  persistOpenHistory();

  // Persist page/chapter while txt is still alive.
  saveProgress();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  // Normalize to BW mode so Home FAST does not flash residual AA red plane.
  renderer.setRenderMode(GfxRenderer::BW);

  // Wait until not rendering to delete task
  if (renderingMutex) {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
  }
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  if (renderingMutex) {
    vSemaphoreDelete(renderingMutex);
    renderingMutex = nullptr;
  }
  pageOffsets.clear();
  currentPageLines.clear();
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  txt.reset();
}

void TxtReaderActivity::requestPluginClose() {
  pluginCloseRequested_ = true;
}

bool TxtReaderActivity::lockState(TickType_t ticks) const {
  if (!renderingMutex) return false;
  return xSemaphoreTake(renderingMutex, ticks) == pdTRUE;
}

void TxtReaderActivity::unlockState() const {
  if (renderingMutex) xSemaphoreGive(renderingMutex);
}

bool TxtReaderActivity::pluginFirstPageReady() const {
  if (!lockState(portMAX_DELAY)) return false;
  const bool v = firstPageReady_;
  unlockState();
  return v;
}

bool TxtReaderActivity::pluginIndexComplete() const {
  if (!lockState(portMAX_DELAY)) return false;
  const bool v = indexComplete_;
  unlockState();
  return v;
}

TxtReaderActivity::PluginProgress TxtReaderActivity::pluginProgressSnapshot() const {
  PluginProgress p;
  p.valid = false;
  p.bookId = pluginSession_.bookId;
  p.chapterUid = pluginSession_.chapterUid;
  p.progressKey = pluginSession_.progressKey;
  p.generation = pluginSession_.generation;
  p.switchChapterIndex = pluginSwitchChapterIndex_;
  // Chapter switch must not hang forever on display-task lock — deliver switch
  // intent even if page snapshot is incomplete (Lua will reopen the new chapter).
  const TickType_t lockWait =
      (pluginSwitchChapterIndex_ >= 0) ? pdMS_TO_TICKS(400) : portMAX_DELAY;
  if (!lockState(lockWait)) {
    if (pluginSwitchChapterIndex_ >= 0) {
      p.valid = true;
      p.page = 0;
      p.total = -1;
      p.byteOffset = 0;
      p.indexComplete = false;
    }
    return p;
  }
  M4PluginReaderStatePolicy::IndexState s;
  s.pageOffsets = pageOffsets;
  s.currentPage = currentPage;
  s.totalPages = totalPages;
  s.indexComplete = indexComplete_;
  s.generation = pluginSession_.generation;
  const auto snap = M4PluginReaderStatePolicy::makeProgressSnapshot(s);
  unlockState();
  if (!snap.valid) {
    // Still deliver switch-chapter intent even if page snapshot is incomplete.
    if (pluginSwitchChapterIndex_ >= 0) {
      p.valid = true;
      p.page = 0;
      p.total = -1;
      p.byteOffset = 0;
      p.indexComplete = false;
    }
    return p;
  }
  p.valid = true;
  p.page = snap.page;
  p.total = snap.total;
  p.byteOffset = snap.byteOffset;
  p.indexComplete = snap.indexComplete;
  p.generation = snap.generation;
  return p;
}

std::string TxtReaderActivity::displayTitle() const {
  if (pluginSession_.active && !pluginSession_.titleOverride.empty()) {
    return pluginSession_.titleOverride;
  }
  return txt ? txt->getTitle() : "TXT";
}

int TxtReaderActivity::pageIndexForByteLocked(size_t byteOffset) const {
  return M4PluginReaderStatePolicy::pageForByte(pageOffsets, byteOffset);
}

void TxtReaderActivity::applyPendingRestoreIfReady() {
  // Caller holds renderingMutex.
  M4PluginReaderStatePolicy::IndexState s;
  s.pageOffsets = pageOffsets;  // share vector content via assign
  // Work on live fields without full copy of vector for apply:
  if (!hasPendingRestore_ || userMovedPage_ || pageOffsets.empty()) return;
  const size_t target = pendingRestoreByte_;
  if (!indexComplete_) {
    if (indexCursor_ <= target && pageOffsets.back() <= target) return;
  }
  const int page = pageIndexForByteLocked(target);
  if (page != currentPage) {
    currentPage = page;
    cachedPage = -1;
    updateRequired = true;
  }
  hasPendingRestore_ = false;
}

void TxtReaderActivity::providerIdlePrefetchNext() {
  if (!pluginSession_.providerManaged || pluginSession_.providerId.empty()) return;
  // Offline continue-reading must not start TLS. lwIP is uninitialized until
  // STA/ETH is up; WeRead fetchChapter then asserts in getaddrinfo.
  if (!M4NativeWifi::isReady()) {
    if (!providerPrefetchRequested_) {
      providerPrefetchRequested_ = true;
      Serial.printf("[WRCP] t=%lu idle_prefetch skip wifi_down next=%d book=%s\n",
                    static_cast<unsigned long>(millis()), pluginSession_.chapterIndex + 1,
                    pluginSession_.bookId.c_str());
    }
    return;
  }
  const int next = pluginSession_.chapterIndex + 1;
  const auto st =
      M4ContentProviderSession::chapterAt(pluginSession_.providerId, pluginSession_.bookId, next);
  // Background failures stay quiet. Retrying every few seconds while the
  // reader owns fonts/index/display memory caused TLS churn and hard resets;
  // a user chapter advance retries through the foreground loading page.
  if (st.state == M4ContentProvider::ChapterReady::Error) return;
  if (!M4ContentProvider::shouldIdlePrefetchNext(st)) return;
  if (providerPrefetchRequested_) return;
  if (M4NativeProviderManager::requestChapter(pluginSession_.providerId, pluginSession_.bookId, next,
                                               M4NativeProviderManager::LoadIntent::Prefetch)) {
    providerPrefetchRequested_ = true;
    Serial.printf("[WRCP] t=%lu idle_prefetch next=%d book=%s\n", static_cast<unsigned long>(millis()), next,
                  pluginSession_.bookId.c_str());
  }
}

bool TxtReaderActivity::switchToProviderChapter(const std::string& cacheRelPath, int index0,
                                               const std::string& chapterUid, const std::string& title) {
  if (!pluginSession_.providerManaged || cacheRelPath.empty()) return false;
  if (!M4ContentProvider::isSafeCacheRelPath(cacheRelPath.c_str())) return false;
  std::string abs;
  const auto re =
      M4PluginReaderBridge::resolveUnderDataRoot(pluginSession_.appDataRoot, cacheRelPath.c_str(), abs);
  if (re != M4PluginReaderBridge::OpenError::Ok) return false;

  auto nextTxt = std::make_unique<Txt>(abs, "/.crosspoint");
  if (!nextTxt->load() || !nextTxt->isEncodingSupported()) return false;
  // A provider chapter that downloaded but wrote an empty/partial body (e.g.
  // prefetch TLS handshake failed under low internal RAM) must NOT open as a
  // blank page with no status bar. Refuse; the caller shows the wait overlay
  // and the provider re-fetches.
  if (nextTxt->getFileSize() == 0) return false;

  // Persist outgoing chapter progress while old txt is still valid.
  saveProgress();

  txt = std::move(nextTxt);
  pluginSession_.chapterIndex = index0;
  pluginSession_.chapterUid = chapterUid;
  pluginSession_.cacheRelPath = cacheRelPath;
  // Seamless next-chapter open passes no title; resolve it from the plugin
  // TOC so the status bar below the text shows the new chapter, not the old.
  std::string resolvedTitle = title;
  if (resolvedTitle.empty()) {
    (void)resolvePluginTitle(pluginSession_, index0, resolvedTitle);
  }
  if (!resolvedTitle.empty()) pluginSession_.titleOverride = resolvedTitle;
  pluginSession_.progressKey =
      pluginSession_.providerId + ":" + pluginSession_.bookId + ":" + chapterUid;

  chapter_initialized = false;
  pageOffsets.clear();
  pageOffsets = {0};
  totalPages = 1;
  currentPage = 0;
  cachedPage = -1;
  indexComplete_ = false;
  firstPageReady_ = false;
  firstReadableLogged_ = false;
  indexCursor_ = 0;
  tidxSaved_ = false;
  largeTxtFastOpen_ = false;
  fastOpenChapter_ = -1;
  activeChapterBegin_ = 0;
  activeChapterEnd_ = 0;
  progressSavePending_ = false;
  chapterDiscoveryBatch_ = -1;
  chapterDiscoveryDone_ = true;
  hasPendingRestore_ = false;
  userMovedPage_ = false;
  pluginNeedsClearRefresh_ = false;
  pluginPendingHalfFlush_ = false;
  providerOverlayMsg_.clear();
  providerOverlayState_ = M4ContentProvider::ChapterReady::Ready;
  providerPrefetchRequested_ = false;
  armEntryWhiteSeed(EntryPlaceholderKind::NextChapter);

  M4ContentProviderSession::noteOpen(pluginSession_.providerId, pluginSession_.bookId, index0, 0);
  M4ContentProvider::ChapterStatus ready;
  ready.providerId = pluginSession_.providerId;
  ready.bookId = pluginSession_.bookId;
  ready.chapterUid = chapterUid;
  ready.index0 = index0;
  ready.state = M4ContentProvider::ChapterReady::Ready;
  ready.cacheRelPath = cacheRelPath;
  (void)M4ContentProviderSession::setChapterStatus(ready);

  // N+1 starts only after this chapter's first page/index is stable; see
  // providerIdlePrefetchNext(). This keeps TLS away from the first paint.

  // Keep reading-history originalSourcePath on the new chapter cache so cold
  // reopen lands on this chapter (not the first open of the session).
  if (!pluginSession_.suppressRecentBooks) {
    const std::string uri = M4ContentProvider::makeHistoryUri(pluginSession_.providerId.c_str(),
                                                              pluginSession_.bookId.c_str());
    std::string appId;
    if (!uri.empty() &&
        M4HistoryReopen::resolveHistoryAppId(pluginSession_.appId, pluginSession_.appDataRoot, abs, appId)) {
      const auto snap =
          M4ContentProviderSession::makeHistorySnapshot(pluginSession_.providerId, pluginSession_.bookId);
      const std::string historyTitle =
          !snap.title.empty() ? snap.title
                              : (pluginSession_.titleOverride.empty() ? pluginSession_.bookId
                                                                      : pluginSession_.titleOverride);
      RECENT_BOOKS.addBook(uri, historyTitle, appId, "", abs);
    }
  }

  updateRequired = true;
  Serial.printf("[WRCP] t=%lu switch_chapter idx=%d path=%s\n", static_cast<unsigned long>(millis()), index0,
                cacheRelPath.c_str());
  return true;
}

void TxtReaderActivity::applyPluginTocSelection(int newChapterNum) {
  if (newChapterNum == pluginSession_.chapterIndex) {
    requestExitSubActivity();
    updateRequired = true;
    return;
  }
  // Cached provider chapters can stay in this reader. History-reopen used to
  // parent TxtReader under ReaderActivity; closing for "Lua reopen" then
  // dropped the whole stack to Home (user-visible 闪退).
  if (pluginSession_.providerManaged && !pluginSession_.providerId.empty() &&
      !pluginSession_.bookId.empty()) {
    const auto st = M4ContentProviderSession::chapterAt(pluginSession_.providerId, pluginSession_.bookId,
                                                        newChapterNum);
    if (st.state == M4ContentProvider::ChapterReady::Ready && !st.cacheRelPath.empty()) {
      bool switched = false;
      if (lockState(pdMS_TO_TICKS(500))) {
        switched = switchToProviderChapter(st.cacheRelPath, newChapterNum, st.chapterUid, "");
        unlockState();
      }
      if (switched) {
        requestExitSubActivity();
        updateRequired = true;
        Serial.printf("[%lu] [TRS] toc in-place switch ch=%d\n", millis(), newChapterNum);
        return;
      }
    }
  }
  pluginSwitchChapterIndex_ = newChapterNum;
  requestExitSubActivity();
  requestPluginClose();
  pendingGoBack = true;
}

bool TxtReaderActivity::tryProviderNextChapterAdvance() {
  if (!pluginSession_.active || !pluginSession_.providerManaged) return false;
  if (pluginSession_.providerId.empty() || pluginSession_.bookId.empty()) return false;
  const int next = pluginSession_.chapterIndex + 1;
  const auto st =
      M4ContentProviderSession::chapterAt(pluginSession_.providerId, pluginSession_.bookId, next);
  const auto d = M4ContentProvider::decideNextChapter(st, true);
  if (d.shouldRequestPrefetch) {
    (void)M4NativeProviderManager::requestChapter(pluginSession_.providerId, pluginSession_.bookId, next,
                                                   M4NativeProviderManager::LoadIntent::Foreground);
  }
  if (d.action == M4ContentProvider::NextChapterDecision::Action::OpenReady) {
    // Title resolved from the plugin TOC inside switchToProviderChapter.
    if (switchToProviderChapter(st.cacheRelPath, next, st.chapterUid, /*title=*/"")) return true;
    // Cache said Ready but the file is empty/partial. Fall through to the
    // parent loading-page path, exactly like picking the chapter from the TOC.
    Serial.printf("[WR05] t=%lu next_chapter_empty → list-style open idx=%d\n", millis(), next);
  }
  if (d.action == M4ContentProvider::NextChapterDecision::Action::WaitOverlay ||
      d.action == M4ContentProvider::NextChapterDecision::Action::RequestAndWait ||
      (d.action == M4ContentProvider::NextChapterDecision::Action::OpenReady)) {
    // Next chapter is not openable from cache right now. Hand it to the plugin
    // UI exactly like a list selection: queue an open for chapter `next` and
    // leave the native reader — the plugin shows its "下载章节… / 检查缓存…"
    // flow, downloads if needed, and opens the chapter when ready. This makes
    // "next page at chapter end" behave like picking from the chapter list
    // (loading page first, never a dead tap or blank reader).
    // Publish the same switch intent as the system TOC. The parent tears down
    // reader memory, paints its loading page, and reuses the already queued
    // worker request. Previously queueOpen swallowed the intent for native
    // providers, so requestPluginClose() had no pendingGoBack and the tap died.
    pluginSwitchChapterIndex_ = next;
    pendingGoBack = true;
    Serial.printf("[WR05] t=%lu next_chapter loading_page idx=%d state=%s\n", millis(), next,
                  M4ContentProvider::stateKey(st.state));
    requestPluginClose();
    return true;
  }
  return false;
}

void TxtReaderActivity::pageTurnLocked(int delta) {
  // Never block the owner loop on the display-task lock. TTF first-paint on
  // QEMU can hold that lock for many seconds; waiting here starves m4adb poll
  // and makes every tap look frozen.
  if (!lockState(0)) {
    // Slow first-page index / loading holds the lock for seconds. Queuing
    // deltas here would replay as surprise multi-page turns after the window.
    // Drop while the first page is not ready; once ready, keep the prior
    // coalesce so mid-refresh taps still catch up.
    if (!firstPageReady_) return;
    pendingTurnDelta_.fetch_add(delta, std::memory_order_relaxed);
    return;
  }
  // First-page index may briefly release the lock between phases. Ignore
  // turns and clear any stale pending so nothing replays after the window.
  if (!firstPageReady_) {
    pendingTurnDelta_.store(0, std::memory_order_relaxed);
    unlockState();
    return;
  }
  const int totalDelta = delta + pendingTurnDelta_.exchange(0, std::memory_order_relaxed);
  if (totalDelta == 0) {
    unlockState();
    return;
  }
  const int step = totalDelta >= 0 ? (totalDelta == 0 ? 1 : totalDelta) : -totalDelta;
  const int signedStep = totalDelta >= 0 ? step : -step;
  auto fillView = [&](M4PluginReaderStatePolicy::IndexState& view) {
    // tryPageTurn only needs empty-ness of offsets, not the full vector.
    view.pageOffsets.clear();
    if (!pageOffsets.empty()) view.pageOffsets.push_back(0);
    view.currentPage = currentPage;
    view.totalPages = totalPages;
    view.indexComplete = indexComplete_;
    view.hasPendingRestore = hasPendingRestore_;
    view.userMovedPage = userMovedPage_;
  };
  bool needIndex = false;
  M4PluginReaderStatePolicy::IndexState view;
  fillView(view);
  const bool moved = M4PluginReaderStatePolicy::tryPageTurn(view, signedStep, &needIndex);
  if (moved) {
    currentPage = view.currentPage;
    userMovedPage_ = view.userMovedPage;
    hasPendingRestore_ = view.hasPendingRestore;
    dualRightPage = -1;
    dualNextLeft = true;
    // Decoupled quick page skip: rapid taps (<400ms) or taps while the panel is
    // mid-refresh only advance the target page — no load, no render. The
    // physical refresh catches up as soon as the in-flight animation finishes
    // and the panel is idle (displayTaskLoop), straight to the target page.
    // There is NO debounce delay: a slow tap (panel idle) starts the animation
    // immediately on the next display-task tick.
    // CRITICAL: never clear a queued refresh. updateRequired is execution
    // state — a tap is an event and must not cancel a pending refresh, or a
    // click gets lost (first tap builds page2, a second tap would be needed to
    // "wake" the chase).
    const uint32_t nowMs = millis();
    const bool quickTap = (lastPageTurnMs_ != 0 && (nowMs - lastPageTurnMs_ < 400)) ||
                          physicalEpdBusy_.load();
    lastPageTurnMs_ = nowMs;
    if (quickTap) {
      quickMode_ = true;
    } else {
      quickMode_ = false;
      updateRequired = true;
    }
    if (providerOverlayMsg_.size()) {
      providerOverlayMsg_.clear();
      providerOverlayState_ = M4ContentProvider::ChapterReady::Ready;
    }
    unlockState();
    return;
  }
  // Progressive index for plugin *and* library multi-chapter TXT (next-chapter
  // used to block on full buildPageIndex under this lock → multi-second stall).
  if (needIndex && !indexComplete_) {
    // Let the target run ahead of the known index. Display-task catch-up
    // indexes until the page exists. Never call continuePageIndex here: that
    // rasterizes TTF on the owner loop and freezes m4adb tap/key.
    currentPage += signedStep;
    if (currentPage < 0) currentPage = 0;
    userMovedPage_ = true;
    hasPendingRestore_ = false;
    dualRightPage = -1;
    dualNextLeft = true;
    quickMode_ = true;
    updateRequired = true;
    lastPageTurnMs_ = millis();
    unlockState();
    return;
  }
  // Provider-managed multi-chapter: last page next → seamless open or overlay wait.
  // Provider open is a hard content switch (network/cache) — keep immediate path.
  // No indexComplete_ gate: a chapter-end next tap must advance even while the
  // current chapter's progressive index is still growing (otherwise nothing
  // happens — the "dead" tap).
  if (pluginSession_.active && pluginSession_.providerManaged && signedStep > 0 &&
      currentPage >= totalPages - 1) {
    unlockState();
    if (tryProviderNextChapterAdvance()) return;
    if (!lockState(portMAX_DELAY)) return;
  }
  // Library chapter boundaries (prev/next chapter) — only when not plugin.
  // Never advance while progressive index is still growing (would skip tail pages).
  if (!pluginSession_.active) {
    // Rapid taps / panel busy: cross-chapter only advances the target; the
    // display task re-inits the new chapter when it catches up.
    const uint32_t nowMs2 = millis();
    const bool quickTap2 = (lastPageTurnMs_ != 0 && (nowMs2 - lastPageTurnMs_ < 400)) ||
                           physicalEpdBusy_.load();
    lastPageTurnMs_ = nowMs2;
    if (totalDelta < 0 && currentPage <= 0 && chapternum > 0) {
      libraryPrefetchReset();  // drop in-flight next-chapter index work
      chapternum--;
      chapter_initialized = false;
      pageOffsets.clear();
      totalPages = 0;
      cachedPage = -1;
      currentPage = -1;
      dualRightPage = -1;
      dualNextLeft = true;
      indexComplete_ = false;
      firstPageReady_ = false;
      tidxSaved_ = false;
      // New chapter = new content regardless of page number; force first physical
      // drive (same-page skip would block it when the prev chapter's last page
      // number collides, e.g. a single-page chapter).
      lastPhysicalBodyPage_ = -1;
      if (quickTap2) {
        quickMode_ = true;  // keep any queued updateRequired (no lost click)
      } else {
        quickMode_ = false;
        updateRequired = true;
      }
      Serial.printf("[%lu] [TRS] Switch to chapter %d (prev) target=%d quick=%d\n", millis(),
                    chapternum, currentPage, quickTap2 ? 1 : 0);
    } else if (totalDelta > 0 && indexComplete_ && currentPage >= totalPages - 1) {
      libraryPrefetchReset();  // drop in-flight next-chapter index work
      chapternum++;
      chapter_initialized = false;
      pageOffsets.clear();
      totalPages = 0;
      currentPage = 0;
      indexComplete_ = false;
      firstPageReady_ = false;
      tidxSaved_ = false;
      // Same guard as prev-chapter: force physical drive for the new chapter's
      // first page (skip would collide when prev chapter had a single page).
      lastPhysicalBodyPage_ = -1;
      if (quickTap2) {
        quickMode_ = true;  // keep any queued updateRequired (no lost click)
      } else {
        quickMode_ = false;
        updateRequired = true;
      }
      Serial.printf("[%lu] [TRS] Switch to chapter %d (next), start from page 0 target=%d quick=%d\n",
                    millis(), chapternum, currentPage, quickTap2 ? 1 : 0);
    }
  }
  unlockState();
}

void TxtReaderActivity::applyDeferredMenuClose() {
  if (deferredChildTransition_) {
    auto fn = std::move(deferredChildTransition_);
    deferredChildTransition_ = nullptr;
    fn();
    return;
  }
  if (!deferredMenuApply_) return;
  deferredMenuApply_ = false;
  bool needRebuild = deferredMenuNeedRebuild_;
  deferredMenuNeedRebuild_ = false;
  if (deferredMenuOrientation_ != SETTINGS.orientation) {
    SETTINGS.orientation = deferredMenuOrientation_;
    SETTINGS.saveToFile();
    needRebuild = true;
  }
  switch (SETTINGS.orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      break;
  }
  if (cachedFontId != SETTINGS.getReaderFontId() || cachedParagraphAlignment != SETTINGS.paragraphAlignment ||
      wordSpacing != SETTINGS.wordSpacing || needIndent != SETTINGS.firstlineintented) {
    needRebuild = true;
  }
  if (needRebuild) {
    EpdFontLoader::loadFontsFromSd(renderer);
    onSettingsChanged();
  }
  applyAutoPageTurnSettings();
  updateRequired = true;
}

void TxtReaderActivity::loop() {
  // Nested menu / settings child: two-phase close only after child.loop returns.
  if (subActivity) {
    const bool closed = pumpSubActivityFrame();
    if (closed) {
      applyDeferredMenuClose();
      // Menu -> settings/catalog/progress/bookmarks is still a child. Keep the
      // reader display task off the panel or its HALF white-seed races the
      // child's first paint and the device looks frozen.
      if (subActivity || pluginSwitchChapterIndex_ >= 0 || pluginCloseRequested_) {
        suppressDisplay_ = true;
        updateRequired = false;
        if (pluginSwitchChapterIndex_ >= 0 || pluginCloseRequested_) {
          waitPhysicalEpdIdle(400);
        }
        Serial.printf("[%lu] [TRS] child replace → keep suppress (sub=%d switch=%d close=%d)\n",
                      millis(), subActivity ? 1 : 0, pluginSwitchChapterIndex_,
                      pluginCloseRequested_ ? 1 : 0);
      } else {
        // Resume reader paints only after returning to the reader itself.
        // Overlay was FAST-composited on the body; return with FAST so the
        // bars vanish without a HALF/FULL invert flash.
        suppressDisplay_ = false;
        armOverlayReturnFlush();
        updateRequired = true;
      }
      // Chapter select may have deferred state if display held the lock.
      if (hasDeferredChapterSwitch_) {
        hasDeferredChapterSwitch_ = false;
        const int ch = deferredChapterSwitch_;
        if (lockState(pdMS_TO_TICKS(2000))) {
          chapternum = ch;
          chapter_initialized = false;
          pageOffsets.clear();
          totalPages = 0;
          currentPage = 0;
          indexComplete_ = false;
          firstPageReady_ = false;
          tidxSaved_ = false;
          hasPendingRestore_ = false;
          unlockState();
          Serial.printf("[%lu] [TRS] applied deferred chapter switch → %d\n", millis(), ch);
        } else {
          Serial.printf("[%lu] [TRS] deferred chapter switch still busy ch=%d\n", millis(), ch);
        }
        updateRequired = true;
      }
    }
    return;
  }

  // Handle pending go back (deferred to avoid use-after-free)
  if (pendingGoBack) {
    pendingGoBack = false;
    if (pluginSession_.active) {
      // Do NOT destroy self here — parent deletes after this loop returns.
      requestPluginClose();
    }
    if (onGoBack) {
      onGoBack();
    }
    return;  // Don't access 'this' after callback
  }

  if (pendingTurnDelta_.load(std::memory_order_relaxed) != 0) {
    pageTurnLocked(0);
  }

  // Handle pending go home (deferred to avoid use-after-free)
  if (pendingGoHome) {
    pendingGoHome = false;
    if (pluginSession_.active) {
      // Plugin path: home maps to return-to-plugin, not global home.
      requestPluginClose();
      if (onGoBack) onGoBack();
      return;
    }
    if (onGoHome) {
      onGoHome();
    }
    return;  // Don't access 'this' after callback
  }

  // 自动翻页逻辑：在所有其他处理之前检查，以便优先处理取消操作
  if (automaticPageTurnActive) {
    // 按下 Confirm 或 Back 键取消自动翻页（跳过长按后的松手事件）
    if (!skipNextButtonCheck &&
        (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
         mappedInput.wasReleased(MappedInputManager::Button::Back))) {
      automaticPageTurnActive = false;
      rollingMode = false;
      rollingHalfTurned = false;
      if (SETTINGS.autoPageTurnEnabled) {
        SETTINGS.autoPageTurnEnabled = 0;
        SETTINGS.saveToFile();
      }
      updateRequired = true;
      return;
    }

    // 在自动翻页期间也需要清除 skipNextButtonCheck
    if (skipNextButtonCheck) {
      const bool confirmCleared = !mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
                                  !mappedInput.wasReleased(MappedInputManager::Button::Confirm);
      const bool backCleared = !mappedInput.isPressed(MappedInputManager::Button::Back) &&
                               !mappedInput.wasReleased(MappedInputManager::Button::Back);
      if (confirmCleared && backCleared) {
        skipNextButtonCheck = false;
      }
      return;
    }

    // 若渲染正在进行，跳过本轮（等渲染完成后再计时）
    if (!APP_STATE.isRenderComplete) {
      return;
    }

    // 到达翻页时间间隔，执行翻页
    if ((millis() - lastAutoPageTurnTime) >= pageTurnDuration) {
      // 横屏双页模式：绕过卷帘机制，左右交替接收新页
      if (isLandscapeDualPage() && rollingMode) {
        rollingHalfTurned = false;
        if (lockState(portMAX_DELAY)) {
          if (dualNextLeft) {
            int newLeft = dualRightPage + 1;
            if (dualRightPage >= 0 && newLeft < totalPages) {
              currentPage = newLeft;
              dualNextLeft = false;
              userMovedPage_ = true;
              hasPendingRestore_ = false;
            } else if (!pluginSession_.active) {
              chapternum++;
              chapter_initialized = false;
              pageOffsets.clear();
              totalPages = 0;
              currentPage = 0;
              dualRightPage = -1;
              dualNextLeft = true;
            }
          } else {
            int newRight = currentPage + 1;
            if (newRight < totalPages) {
              dualRightPage = newRight;
              dualNextLeft = true;
            } else if (!pluginSession_.active) {
              chapternum++;
              chapter_initialized = false;
              pageOffsets.clear();
              totalPages = 0;
              currentPage = 0;
              dualRightPage = -1;
              dualNextLeft = true;
            }
          }
          unlockState();
        }
        lastAutoPageTurnTime = millis();
        updateRequired = true;
        return;
      }

      // 卷帘模式第一阶段：触发半屏翻转（显示下一页顶部 + 当前页底部）
      if (rollingMode && !rollingHalfTurned) {
        rollingHalfTurned = true;
        lastAutoPageTurnTime = millis();
        updateRequired = true;
        return;
      }

      // 完整翻页（普通模式，或卷帘模式第二阶段）— state lock via pageTurnLocked
      rollingHalfTurned = false;
      pageTurnLocked(+1);
      lastAutoPageTurnTime = millis();
      return;
    }
    return;  // 等待计时器到期
  }

  // 从设置中读取全局下一页模式状态
  globalNextPageMode = (SETTINGS.globalNextPageModeEnabled != 0);

  // 长按菜单键 (Confirm) 执行映射功能 (1 秒以上)
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= 1000) {
    if (!globalNextPageModeToggled) {
      globalNextPageModeToggled = true;
      skipNextButtonCheck = true;
      const uint8_t action = SETTINGS.longPressConfirmAction;
      if (action == 0) {
        globalNextPageMode = !globalNextPageMode;
        SETTINGS.globalNextPageModeEnabled = globalNextPageMode ? 1 : 0;
        SETTINGS.saveToFile();
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        GUI.drawPopup(renderer, globalNextPageMode ? L(Str::kGlobalNextPageModeOn) : L(Str::kGlobalNextPageModeOff));
        renderer.displayBuffer();
        xSemaphoreGive(renderingMutex);
      } else if (action == 1) {
        // 切换蓝牙
        try {
          auto& btMgr = BluetoothHIDManager::getInstance();
          if (btMgr.isEnabled()) {
            btMgr.disable();
            SETTINGS.bluetoothEnabled = 0;
            SETTINGS.saveToFile();
            xSemaphoreTake(renderingMutex, portMAX_DELAY);
            GUI.drawPopup(renderer, L(Str::kBTClosed));
            renderer.displayBuffer();
            xSemaphoreGive(renderingMutex);
          } else {
            GUI.drawPopup(renderer, L(Str::kBTConnectingEllipsis));
            renderer.displayBuffer(HalDisplay::FAST_REFRESH);
            SETTINGS.bluetoothEnabled = 1;
            SETTINGS.saveToFile();
            bool connected = false;
            if (btMgr.enable()) {
              btMgr.startScan(2000);
              std::string lastAddr, lastName;
              if (btMgr.loadLastConnectedDevice(lastAddr, lastName) && !lastAddr.empty()) {
                unsigned long btStart = millis();
                while (millis() - btStart < 3000) {
                  if (btMgr.connectToDevice(lastAddr)) { connected = true; break; }
                  delay(200);
                }
              }
              btMgr.stopScan();
              if (!connected) { btMgr.disable(); SETTINGS.bluetoothEnabled = 0; SETTINGS.saveToFile(); }
            } else { SETTINGS.bluetoothEnabled = 0; SETTINGS.saveToFile(); }
            xSemaphoreTake(renderingMutex, portMAX_DELAY);
            GUI.drawPopup(renderer, connected ? L(Str::kBTConnected) : L(Str::kBTConnectFailed));
            renderer.displayBuffer();
            xSemaphoreGive(renderingMutex);
          }
        } catch (...) {
          xSemaphoreTake(renderingMutex, portMAX_DELAY);
          GUI.drawPopup(renderer, L(Str::kBTError));
          renderer.displayBuffer();
          xSemaphoreGive(renderingMutex);
        }
      } else if (action == 2) {
        SETTINGS.autoPageTurnEnabled = SETTINGS.autoPageTurnEnabled ? 0 : 1;
        SETTINGS.saveToFile();
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        GUI.drawPopup(renderer, SETTINGS.autoPageTurnEnabled ? L(Str::kAutoPageTurnOn) : L(Str::kAutoPageTurnOff));
        renderer.displayBuffer();
        xSemaphoreGive(renderingMutex);
        if (SETTINGS.autoPageTurnEnabled) applyAutoPageTurnSettings();
      } else if (action == 3) {
        SETTINGS.textAntiAliasing = SETTINGS.textAntiAliasing ? 0 : 1;
        SETTINGS.saveToFile();
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        GUI.drawPopup(renderer, SETTINGS.textAntiAliasing ? L(Str::kAntiAliasingOn) : L(Str::kAntiAliasingOff));
        renderer.displayBuffer();
        xSemaphoreGive(renderingMutex);
        updateRequired = true;
      } else if (action == 4) {
        SETTINGS.epubDarkMode = SETTINGS.epubDarkMode ? 0 : 1;
        SETTINGS.saveToFile();
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        GUI.drawPopup(renderer, SETTINGS.epubDarkMode ? L(Str::kDarkModeOn) : L(Str::kDarkModeOff));
        renderer.displayBuffer();
        xSemaphoreGive(renderingMutex);
        updateRequired = true;
      }
    }
    return;
  }

  // 重置长按标志
  if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    globalNextPageModeToggled = false;
  }

  // 吸收长按后的松手事件
  if (skipNextButtonCheck) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
        !mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      skipNextButtonCheck = false;
    }
    return;
  }

  // 全局下一页模式：所有按键（除 Confirm 和 Power）都是下一页，Power 是上一页
  if (globalNextPageMode) {
    // Confirm 短按：打开菜单
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      openMenu();
      return;
    }

    // Power 键：上一页
    const bool longPressPageBack = mappedInput.wasReleased(MappedInputManager::Button::Power);
    if (longPressPageBack) {
      pageTurnLocked(-1);
      return;
    }

    // 其他任意按键释放：下一页
    const bool anyButtonReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                                   mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                                   mappedInput.wasReleased(MappedInputManager::Button::Left) ||
                                   mappedInput.wasReleased(MappedInputManager::Button::Right) ||
                                   mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                                   mappedInput.wasReleased(MappedInputManager::Button::PageForward);
    if (anyButtonReleased) {
      pageTurnLocked(+1);
      return;
    }
    return;  // 跳过正常翻页逻辑
  }

  // Touch: left/right page zones; center/menu gesture opens menu.
  bool touchPrev = false;
  bool touchNext = false;
  bool touchMenu = false;
  if (mappedInput.hasTouch()) {
    if (mappedInput.wasBackGesture()) {
      pendingGoBack = true;
      return;
    }
    if (mappedInput.wasMenuGesture()) {
      touchMenu = true;
    } else {
      int tx = 0;
      int ty = 0;
      if (mappedInput.wasScreenTapped(tx, ty)) {
        const auto zone =
            TouchHitGeometry::readerZoneFromPoint(tx, ty, renderer.getScreenWidth(), renderer.getScreenHeight());
        if (zone == TouchHitGeometry::ReaderZone::Prev) {
          touchPrev = true;
        } else if (zone == TouchHitGeometry::ReaderZone::Next) {
          touchNext = true;
        } else if (zone == TouchHitGeometry::ReaderZone::Menu) {
          touchMenu = true;
        }
      } else {
        const auto swipe = mappedInput.wasSwipe();
        if (swipe == MappedInputManager::SwipeDir::Right) {
          touchPrev = true;
        } else if (swipe == MappedInputManager::SwipeDir::Left) {
          touchNext = true;
        }
      }
    }
  }

  // 打开菜单（短按 Confirm）
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || touchMenu) {
    openMenu();
  }
  // Long press BACK (1s+) goes directly to home
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= goHomeMs) {
    pendingGoHome = true;
    return;
  }

  // Short press BACK goes to file selection
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    pendingGoBack = true;
    return;
  }

  // When long-press chapter skip is disabled, turn pages on press instead of release.
  const bool usePressForPageTurn = !SETTINGS.longPressChapterSkip;

#ifdef CROSSPOINT_X3
  // 自动旋转检查
  if (SETTINGS.autoRotateEnabled && tiltDetector.isReady()) {
    uint8_t detected = tiltDetector.getDetectedOrientation();
    if (detected != SETTINGS.orientation) {
      Serial.printf("[%lu] [TXT] Auto-rotate: %d -> %d\n", millis(), SETTINGS.orientation, detected);
      SETTINGS.orientation = detected;
      SETTINGS.saveToFile();
      switch (SETTINGS.orientation) {
        case CrossPointSettings::ORIENTATION::PORTRAIT:
          renderer.setOrientation(GfxRenderer::Orientation::Portrait); break;
        case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
          renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise); break;
        case CrossPointSettings::ORIENTATION::INVERTED:
          renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted); break;
        case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
          renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise); break;
        default: break;
      }
      onSettingsChanged();
      updateRequired = true;
      return;
    }
  }
#endif

  const bool prevTriggered =
      touchPrev ||
      (usePressForPageTurn ? (mappedInput.wasPressed(MappedInputManager::Button::PageBack) ||
                              mappedInput.wasPressed(MappedInputManager::Button::Left))
                           : (mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                              mappedInput.wasReleased(MappedInputManager::Button::Left)));
  const bool powerPageTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN &&
                             mappedInput.wasReleased(MappedInputManager::Button::Power);
  const bool nextTriggered =
      touchNext ||
      (usePressForPageTurn
           ? (mappedInput.wasPressed(MappedInputManager::Button::PageForward) || powerPageTurn ||
              mappedInput.wasPressed(MappedInputManager::Button::Right))
           : (mappedInput.wasReleased(MappedInputManager::Button::PageForward) || powerPageTurn ||
              mappedInput.wasReleased(MappedInputManager::Button::Right)));

  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // All page-offset / currentPage / restore mutations under the state lock.
  // UI may wait for a display-task render or index slice (correctness first).
  const int step = isLandscapeDualPage() ? 2 : 1;
  if (prevTriggered) {
    pageTurnLocked(-step);
  } else {
    pageTurnLocked(+step);
  }
}



void TxtReaderActivity::openMenu(EpubReaderMenuActivity::MenuLayer layer) {
  // Do not let reader AA/e-ink race menu/settings paints (residual overlay).
  suppressDisplay_ = true;
  updateRequired = false;
  waitPhysicalEpdIdle(2500);
  renderer.setRenderMode(GfxRenderer::BW);

  // Short coherent snapshot under the state lock; enter child unlocked.
  int bookProgressPercent = 0;
  int pageDisp = 1;
  int totalDisp = 1;
  std::string title = displayTitle();
  if (lockState(portMAX_DELAY)) {
    // Coherent snapshot of progressive fields (short critical section).
    if (!pageOffsets.empty()) {
      int p0 = currentPage >= 0 ? currentPage : 0;
      if (p0 >= (int)pageOffsets.size()) p0 = (int)pageOffsets.size() - 1;
      const size_t off = pageOffsets[static_cast<size_t>(p0)];
      const size_t fs = txt ? txt->getFileSize() : 0;
      if (fs > 0) {
        bookProgressPercent =
            static_cast<int>(static_cast<float>(off) * 100.0f / static_cast<float>(fs) + 0.5f);
        if (bookProgressPercent > 100) bookProgressPercent = 100;
        if (bookProgressPercent < 0) bookProgressPercent = 0;
      }
      pageDisp = M4PluginReaderStatePolicy::page0ToLua1(p0);
      totalDisp = totalPages > 0 ? totalPages : 1;
    }
    unlockState();
  }

  // Do not hold the state lock across child enter/destroy.
  exitActivity();
  enterNewActivity(new EpubReaderMenuActivity(
      this->renderer, this->mappedInput, title, pageDisp, totalDisp, bookProgressPercent,
      SETTINGS.orientation,
      // onBack: request deferred close; apply orientation after child loop returns.
      [this](uint8_t newOrientation) {
        deferredMenuOrientation_ = newOrientation;
        deferredMenuNeedRebuild_ = false;
        if (newOrientation != SETTINGS.orientation) deferredMenuNeedRebuild_ = true;
        if (cachedFontId != SETTINGS.getReaderFontId() ||
            cachedParagraphAlignment != SETTINGS.paragraphAlignment ||
            wordSpacing != SETTINGS.wordSpacing || needIndent != SETTINGS.firstlineintented) {
          deferredMenuNeedRebuild_ = true;
        }
        deferredMenuApply_ = true;
        requestExitSubActivity();
      },
      // onAction: may schedule deferred child replace (never exitActivity inline).
      [this](EpubReaderMenuActivity::MenuAction action) {
        handleMenuAction(action);
      },
      layer));
}

void TxtReaderActivity::enterChapterPicker() {
  // Provider sessions reuse the system TOC. Both legacy toc.json and
  // bounded file-backed catalogs are accepted; never scan a single
  // cached chapter body for headings. FileRows are paged on demand by
  // the same native system picker used for local TXT chapters.
  if (pluginSession_.active) {
    const auto pagedTitles = openPluginPagedTitles(pluginSession_);
    if (pagedTitles) {
      const int count = static_cast<int>(pagedTitles->rowCount());
      int cur = pluginSession_.chapterIndex;
      if (cur < 0) cur = 0;
      if (cur >= count) cur = count - 1;
      const std::string header =
          pluginSession_.titleOverride.empty() ? std::string("目  录") : pluginSession_.titleOverride;
      auto loader = [pagedTitles](int first, int count, std::vector<std::string>& pageTitles,
                                  std::vector<uint8_t>& pagePresent) {
        return pagedTitles->loadPage(first, count, pageTitles, pagePresent);
      };
      enterNewActivity(new TxtReaderChapterSelectionActivity(
          this->renderer, this->mappedInput, count, std::move(loader), cur,
          [this] {
            requestExitSubActivity();
            updateRequired = true;
          },
          [this](const int newChapterNum) { applyPluginTocSelection(newChapterNum); },
          header));
      return;
    }
    std::vector<std::string> titles;
    if (loadPluginTitles(pluginSession_, titles) && !titles.empty()) {
      int cur = pluginSession_.chapterIndex;
      if (cur < 0) cur = 0;
      if (cur >= static_cast<int>(titles.size())) cur = static_cast<int>(titles.size()) - 1;
      const std::string header =
          pluginSession_.titleOverride.empty() ? std::string("目  录") : pluginSession_.titleOverride;
      enterNewActivity(new TxtReaderChapterSelectionActivity(
          this->renderer, this->mappedInput, std::move(titles), cur,
          [this] {
            requestExitSubActivity();
            updateRequired = true;
          },
          [this](const int newChapterNum) { applyPluginTocSelection(newChapterNum); },
          header));
      return;
    }
  }
  // Library multi-chapter TXT path.
  enterNewActivity(new TxtReaderChapterSelectionActivity(
      this->renderer, this->mappedInput, txt, chapternum,
      [this] {
        requestExitSubActivity();
        updateRequired = true;
      },
      [this](const int newChapterNum) {
        // Never portMAX_DELAY here: display task may hold state lock during
        // layout; blocking the main loop freezes chapter-picker exit.
        if (lockState(pdMS_TO_TICKS(500))) {
          chapternum = newChapterNum;
          chapter_initialized = false;
          pageOffsets.clear();
          totalPages = 0;
          currentPage = 0;
          tidxSaved_ = false;
          hasPendingRestore_ = false;
          unlockState();
          hasDeferredChapterSwitch_ = false;
        } else {
          deferredChapterSwitch_ = newChapterNum;
          hasDeferredChapterSwitch_ = true;
          Serial.printf("[%lu] [TRS] chapter select deferred (lock busy) ch=%d\n", millis(),
                        newChapterNum);
        }
        updateRequired = true;
        requestExitSubActivity();
      }));
}

void TxtReaderActivity::handleMenuAction(EpubReaderMenuActivity::MenuAction action) {
  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      // Defer menu teardown + chapter picker until after menu.loop returns.
      deferredChildTransition_ = [this]() { enterChapterPicker(); };
      requestExitSubActivity();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      int bookProgressPercent = 0;
      if (lockState(portMAX_DELAY)) {
        const size_t fs = txt ? txt->getFileSize() : 0;
        if (fs > 0 && currentPage >= 0 && currentPage < (int)pageOffsets.size()) {
          bookProgressPercent = static_cast<int>(
              static_cast<float>(pageOffsets[static_cast<size_t>(currentPage)]) * 100.0f / static_cast<float>(fs) +
              0.5f);
        }
        if (bookProgressPercent > 100) bookProgressPercent = 100;
        if (bookProgressPercent < 0) bookProgressPercent = 0;
        unlockState();
      }

      deferredChildTransition_ = [this, bookProgressPercent]() {
        enterNewActivity(new EpubReaderPercentSelectionActivity(
            renderer, mappedInput, bookProgressPercent,
            [this](const int percent) {
              // goToPercent acquires the lock and sets userMovedPage_/restore.
              goToPercent(percent);
              requestExitSubActivity();
              updateRequired = true;
            },
            [this]() {
              requestExitSubActivity();
              updateRequired = true;
            },
            [this](int toolbarHit) {
              if (toolbarHit == 1) return;
              if (toolbarHit == 0) {
                deferredChildTransition_ = [this]() { enterChapterPicker(); };
              } else if (toolbarHit == 2) {
                deferredChildTransition_ = [this]() {
                  openMenu(EpubReaderMenuActivity::MenuLayer::STYLE);
                };
              } else if (toolbarHit == 3) {
                deferredChildTransition_ = [this]() {
                  openMenu(EpubReaderMenuActivity::MenuLayer::MORE);
                };
              }
              requestExitSubActivity();
            }));
      };
      requestExitSubActivity();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::ROTATE_SCREEN: {
      // Orientation is already applied by the menu's onBack callback.
      // If it reaches here, just apply and rebuild.
      onSettingsChanged();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TOGGLE_ANTI_ALIAS: {
      // Toggled in menu; just mark update needed for re-render.
      updateRequired = true;
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TOGGLE_DARK_MODE: {
      // Toggled in menu; just mark update needed for re-render.
      updateRequired = true;
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TOGGLE_FONT: {
      // Font toggled in menu; rebuild pagination with new font.
      onSettingsChanged();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SELECT_EXTERNAL_FONT: {
      // External font selected in menu; rebuild pagination with new font.
      onSettingsChanged();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::READER_SETTINGS: {
      deferredChildTransition_ = [this]() {
        enterNewActivity(new EpubReaderSettingsActivity(
            renderer, mappedInput, [this] {
              requestExitSubActivity();
              onSettingsChanged();
            }));
      };
      requestExitSubActivity();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::AUTO_PAGE_TURN: {
      // Apply automatic page turn settings from SETTINGS
      applyAutoPageTurnSettings();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TOGGLE_GLOBAL_NEXT_PAGE: {
      // Toggled in menu; no additional action needed.
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BLUETOOTH_SETTINGS: {
      // Defer bluetooth settings to avoid race condition with display task
      pendingBluetoothSettings = true;
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      // Cache files are consumed by the display task, so deletion must remain
      // serialized with rendering/indexing even though SD removal can be slow.
      if (lockState(portMAX_DELAY)) {
        const int backupChapter = chapternum;
        const int backupPage = currentPage;
        const std::string cachePath = txt ? txt->getCachePath() : "";
        if (!cachePath.empty()) {
          SdMan.removeDir(cachePath.c_str());
          if (txt) txt->setupCacheDir();
        }
        chapter_initialized = false;
        pageOffsets.clear();
        totalPages = 0;
        tidxSaved_ = false;
        chapternum = backupChapter;
        currentPage = backupPage;
        unlockState();
      }
      pendingGoHome = true;
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      pendingGoHome = true;
      break;
    }
    case EpubReaderMenuActivity::MenuAction::ADD_BOOKMARK: {
      // Snapshot progressive reader fields under the state lock, then do storage unlocked.
      std::string titlePrefix;
      float bookProgress = 0.0f;
      int page0 = 0;
      int chapterSnap = chapternum;
      std::string bookPath;
      std::string bookTitle;
      if (lockState(portMAX_DELAY)) {
        page0 = currentPage >= 0 ? currentPage : 0;
        if (!pageOffsets.empty() && page0 >= (int)pageOffsets.size()) {
          page0 = (int)pageOffsets.size() - 1;
        }
        chapterSnap = chapternum;
        bookPath = txt ? txt->getPath() : "";
        bookTitle = txt ? txt->getTitle() : "";
        const size_t fs = txt ? txt->getFileSize() : 0;
        if (fs > 0 && !pageOffsets.empty() && page0 >= 0 && page0 < (int)pageOffsets.size()) {
          bookProgress = static_cast<float>(pageOffsets[static_cast<size_t>(page0)]) / static_cast<float>(fs);
        }
        // Copy a short prefix from current page lines under lock.
        int charCount = 0;
        for (const auto& pageLine : currentPageLines) {
          const unsigned char* ptr = reinterpret_cast<const unsigned char*>(pageLine.c_str());
          while (*ptr && titlePrefix.size() < 30) {
            if (*ptr == ' ' || *ptr == '\t') {
              ptr++;
              continue;
            }
            if (*ptr == 0xE3 && *(ptr + 1) == 0x80 && *(ptr + 2) == 0x80) {
              ptr += 3;
              continue;
            }
            const unsigned char* charStart = ptr;
            if (*ptr >= 0xF0)
              ptr += 4;
            else if (*ptr >= 0xE0)
              ptr += 3;
            else if (*ptr >= 0xC0)
              ptr += 2;
            else
              ptr += 1;
            titlePrefix.append(reinterpret_cast<const char*>(charStart), ptr - charStart);
            charCount++;
            if (charCount >= 5) break;
          }
          if (charCount >= 5 || titlePrefix.size() >= 15) break;
        }
        unlockState();
      }

      const int percentInt = static_cast<int>(bookProgress * 100.0f + 0.5f);
      std::string bmTitle;
      if (!titlePrefix.empty()) bmTitle = titlePrefix + "...";
      bmTitle += "\xef\xbc\x88\xe7\xac\xac" + std::to_string(M4PluginReaderStatePolicy::page0ToLua1(page0)) +
                 "\xe9\xa1\xb5, " + std::to_string(percentInt) + "%\xef\xbc\x89";

      Bookmark bm;
      bm.title = bmTitle;
      bm.percentage = bookProgress;
      bm.spineIndex = chapterSnap;
      bm.page = page0;  // store 0-based native page
      bm.timestamp = static_cast<int64_t>(millis());
      bm.bookPath = bookPath;
      bm.bookTitle = bookTitle;
      {
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        if (tv.tv_sec > 1704067200) {
          bm.timestamp = static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
        }
      }

      // SD/store work outside the progressive state lock.
      const std::string bookMd5 = BookmarkStore::calculateBookMd5(bookPath);
      BookmarkStore::addBookmark(bookMd5, bm);

      if (lockState(portMAX_DELAY)) {
        // GUI drawing and e-paper transfer share renderer buffers with the
        // display task; keep the lock through displayBuffer().
        GUI.drawPopup(renderer, L(Str::kBookmarkAdded));
        renderer.displayBuffer();
        unlockState();
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARK_MANAGER: {
      const std::string bookMd5 = BookmarkStore::calculateBookMd5(txt->getPath());
      auto bookmarks = BookmarkStore::loadBookmarks(bookMd5);

      deferredChildTransition_ = [this, bookmarks = std::move(bookmarks), bookMd5]() mutable {
        enterNewActivity(new BookmarkManagerActivity(
          renderer, mappedInput, std::move(bookmarks),
          [this]() {
            requestExitSubActivity();
            updateRequired = true;
          },
          [this](float percentage) {
            // goToPercent acquires lock and cancels pending restore.
            goToPercent(static_cast<int>(percentage * 100.0f + 0.5f));
            requestExitSubActivity();
            updateRequired = true;
          },
          [this, bookMd5](int index) {
            BookmarkStore::deleteBookmark(bookMd5, index);
          }));
      };
      requestExitSubActivity();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC:
    case EpubReaderMenuActivity::MenuAction::SYNCY: {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      GUI.drawPopup(renderer, L(Str::kTxtSyncNotSupported));
      renderer.displayBuffer();
      xSemaphoreGive(renderingMutex);
      break;
    }
    default:
      break;
  }
}

void TxtReaderActivity::onSettingsChanged() {
  // Rebuild under state lock so progressive index cannot race the clear.
  const bool locked = lockState(portMAX_DELAY);
  // Preserve raw-byte position across font/layout rebuild (plugin + library).
  size_t keepByte = 0;
  bool haveByte = false;
  if (currentPage >= 0 && currentPage < (int)pageOffsets.size()) {
    keepByte = pageOffsets[static_cast<size_t>(currentPage)];
    haveByte = true;
  } else if (hasPendingRestore_) {
    keepByte = pendingRestoreByte_;
    haveByte = true;
  }

  chapter_initialized = false;
  pageOffsets.clear();
  totalPages = 0;
  currentPage = 0;
  cachedPage = -1;
  // Library multi-chapter also uses progressive first-page + background continue.
  indexComplete_ = false;
  firstPageReady_ = false;
  indexCursor_ = 0;
  tidxSaved_ = false;
  // Re-arm progressive restore from the preserved byte (do not fight user if they
  // had already cancelled — but settings rebuild is intentional re-layout).
  if (haveByte) {
    pendingRestoreByte_ = keepByte;
    hasPendingRestore_ = true;
    userMovedPage_ = false;
  }

  // Update cached typesetting parameters so the next chapter_initializeReader()
  // uses fresh values and the page-index cache is correctly invalidated.
  cachedFontId = SETTINGS.getReaderFontId();
  cachedParagraphAlignment = SETTINGS.paragraphAlignment;
  wordSpacing = SETTINGS.wordSpacing;
  needIndent = SETTINGS.firstlineintented;
  cachedScreenMargin = SETTINGS.screenMargin_Top + SETTINGS.screenMargin_Left +
                       SETTINGS.screenMargin_Right + SETTINGS.screenMargin_Bottom;

  updateRequired = true;
  if (locked) unlockState();
}

void TxtReaderActivity::goToPercent(int percent) {
  // UI/menu site: take the non-recursive state lock, then jump.
  // Never call this while renderingMutex is already held (deadlock).
  if (!txt) return;
  if (!lockState(portMAX_DELAY)) return;
  goToPercentAlreadyLocked(percent);
  unlockState();
}

void TxtReaderActivity::goToPercentAlreadyLocked(int percent) {
  // REQUIRES: renderingMutex already held by caller (display or goToPercent).
  // Must not call lockState() — FreeRTOS mutex is non-recursive.
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  if (!txt) return;

  userMovedPage_ = true;
  hasPendingRestore_ = false;

  const size_t fileSize = txt->getFileSize();
  size_t targetOffset = static_cast<size_t>(static_cast<float>(percent) / 100.0f * fileSize);
  if (fileSize > 0 && targetOffset >= fileSize) targetOffset = fileSize - 1;

  Serial.printf("[%lu] [TRS] goToPercentAlreadyLocked(%d): targetOffset=%zu, fileSize=%zu\n", millis(), percent,
                targetOffset, fileSize);

  int targetChapter = chapternum;
  if (!pluginSession_.active) {
    // Library multi-chapter: resolve chapter for byte (may touch SD chapter index).
    {
      const int page = chapternum / 25 + 1;
      const int pagebegin = (page - 1) * 25;
      txt->parseChapterIndexAndOffset(pagebegin);
    }
    for (int ch = 0; ch < 10000; ch++) {
      size_t chBegin = txt->getChapterOffsetByIndex(ch);
      size_t chEnd = txt->getChapterendOffsetByIndex(ch);
      if (chBegin == 0 && chEnd == 0 && ch > 0) break;
      if (chEnd == 0) chEnd = fileSize;
      if (chBegin == 0 && ch > 0) {
        const int pg = ch / 25 + 1;
        txt->parseChapterIndexAndOffset((pg - 1) * 25);
        chBegin = txt->getChapterOffsetByIndex(ch);
        chEnd = txt->getChapterendOffsetByIndex(ch);
        if (chBegin == 0 && chEnd == 0) break;
        if (chEnd == 0) chEnd = fileSize;
      }
      if (targetOffset >= chBegin && targetOffset < chEnd) {
        targetChapter = ch;
        break;
      }
    }
    if (targetChapter != chapternum) {
      chapternum = targetChapter;
      chapter_initialized = false;
      pageOffsets.clear();
      totalPages = 0;
      cachedPage = -1;
    }
  }

  if (!chapter_initialized) {
    chapter_initializeReader(chapternum);
  }
  if (totalPages <= 0 || pageOffsets.empty()) return;

  // Progressive index (plugin whole-file or library chapter) may not know the
  // requested page yet. Preserve raw-byte target; background index applies once.
  if (!indexComplete_ && targetOffset >= indexCursor_) {
    pendingRestoreByte_ = targetOffset;
    hasPendingRestore_ = true;
    userMovedPage_ = false;
    updateRequired = true;
    return;
  }

  int targetPage = pageIndexForByteLocked(targetOffset);
  currentPage = targetPage;
  cachedPage = -1;
  userMovedPage_ = true;
  hasPendingRestore_ = false;
  updateRequired = true;
  Serial.printf("[%lu] [TRS] goToPercent: chapter=%d, page=%d/%d\n", millis(), chapternum, currentPage, totalPages);
}

void TxtReaderActivity::applyAutoPageTurnSettings() {
  // Always reset rolling state when applying settings
  rollingMode = false;
  rollingHalfTurned = false;

  if (!SETTINGS.autoPageTurnEnabled) {
    automaticPageTurnActive = false;
    Serial.printf("[%lu] [TRS] Auto page turn disabled\n", millis());
    return;
  }

  // Set up auto page turn from SETTINGS
  lastAutoPageTurnTime = millis();
  pageTurnDuration = SETTINGS.autoPageTurnInterval * 1000UL;
  automaticPageTurnActive = true;
  rollingMode = (SETTINGS.autoPageTurnMode == 1);  // 0=full, 1=rolling

  if (rollingMode) {
    Serial.printf("[%lu] [TRS] Rolling auto-turn enabled: interval %ds\n",
                  millis(), SETTINGS.autoPageTurnInterval);
  } else {
    Serial.printf("[%lu] [TRS] Auto page turn enabled: interval %ds\n",
                  millis(), SETTINGS.autoPageTurnInterval);
  }
}

void TxtReaderActivity::displayTaskLoop() {
  bool loggedFirstPhysical = false;
  while (true) {
    // Menu / settings / chapter list own the panel — do not race e-ink SPI.
    if (suppressDisplay_ || subActivity) {
      vTaskDelay(20 / portTICK_PERIOD_MS);
      continue;
    }
    // Catch-up invariant (independent of quickMode_): whenever the panel is
    // idle, no refresh is queued, and the TARGET page differs from the PHYSICAL
    // page on the panel, we must drive it — target!=physical can never be left
    // waiting for another tap. A slow tap sets updateRequired directly; if that
    // render gets deferred (index not ready, EPD busy) this block is the safety
    // net that keeps re-arming it until target == physical.
    if (!physicalEpdBusy_.load() && !updateRequired && firstPhysicalShown_ &&
        (lastPhysicalBodyPage_ < 0 || currentPage != lastPhysicalBodyPage_)) {
      if (currentPage >= 0 && currentPage < static_cast<int>(pageOffsets.size())) {
        updateRequired = true;
        Serial.printf("[%lu] [TRS] catchup target=%d body=%d\n", millis(), currentPage,
                      lastPhysicalBodyPage_);
      } else if (!indexComplete_) {
        // Target page not indexed yet: push the progressive index until it
        // COVERS the target, then the next idle tick renders+animates straight
        // to it (one burst, not one slow slice per loop pass).
        if (lockState(0)) {
          const uint32_t tIdx = millis();
          int guard = 0;
          while (currentPage >= static_cast<int>(pageOffsets.size()) && !indexComplete_ &&
                 guard++ < 128) {
            const int added = continuePageIndex(1, 16 * 1024);
            if (added <= 0) break;  // no progress — do not spin
            if (guard >= 1) break;  // one page per lock hold; next tick continues
          }
          totalPages = static_cast<int>(pageOffsets.size());
          applyPendingRestoreIfReady();
          logPerf("catchup_index", millis() - tIdx, currentPage,
                  static_cast<uint32_t>(pageOffsets.size()));
          unlockState();
        }
      } else {
        // index complete but target out of range — nothing to chase
      }
    }
    if (updateRequired) {
      updateRequired = false;
      if (suppressDisplay_ || subActivity) continue;

      // --- Entry white seed (before any content layout) ---
      // From shelf/menu/loading the RED plane still holds foreign UI. Page-turn
      // anim would wipe from that residual. Keep the absolute HALF seed, but
      // show an explicit opening/chapter placeholder instead of bare white.
      if (entryWhiteSeedPending_) {
        physicalEpdBusy_ = true;
        const uint32_t tW = millis();
        renderer.setRenderMode(GfxRenderer::BW);
        renderer.clearScreen(0xFF);  // 1-bit white
        if (entryPlaceholderKind_ != EntryPlaceholderKind::None) {
          const auto metrics = UITheme::getInstance().getMetrics();
          GUI.drawHeader(renderer,
                         Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                         pluginSession_.providerManaged ? "在线阅读" : "阅读器");
          const char* status = entryPlaceholderKind_ == EntryPlaceholderKind::NextChapter
                                   ? "正在加载下一章"
                                   : "正在打开阅读器";
          M4UiText::drawCentered(renderer, UI_12_FONT_ID, 250, status, true,
                                 EpdFontFamily::BOLD);
          if (!pluginSession_.titleOverride.empty()) {
            M4UiText::drawCentered(renderer, UI_10_FONT_ID, 305,
                                   pluginSession_.titleOverride.c_str());
          }
          M4UiText::drawCentered(renderer, UI_10_FONT_ID, 365, "请稍候");
        }
        Serial.printf("[WR05] t=%lu open_refresh phase=entry mode=half\n",
                      static_cast<unsigned long>(millis()));
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);  // BYPASS_RED absolute
        (void)renderer.storeBwBuffer();
        (void)renderer.storeLastShown();  // animation baseline = this clean frame
        entryWhiteSeedPending_ = false;
        const bool showedPlaceholder = entryPlaceholderKind_ != EntryPlaceholderKind::None;
        entryPlaceholderKind_ = EntryPlaceholderKind::None;
        firstPhysicalShown_ = false;  // content not shown yet
        lastPhysicalBodyPage_ = -1;
        pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
        Serial.printf("[WR05] t=%lu entry_%s done ms=%lu\n",
                      static_cast<unsigned long>(millis()),
                      showedPlaceholder ? "placeholder" : "white_seed",
                      static_cast<unsigned long>(millis() - tW));
        physicalEpdBusy_ = false;
        updateRequired = true;  // immediately schedule first content frame
        APP_STATE.isRenderComplete = true;
        continue;
      }

      const uint32_t t0 = millis();
      const bool firstPluginFrame = pluginSession_.active && !loggedFirstPhysical;
      if (firstPluginFrame) {
        Serial.printf("[WR05] t=%lu first_render_start gen=%u\n", static_cast<unsigned long>(t0),
                      static_cast<unsigned>(pluginSession_.generation));
      } else {
        Serial.printf("[%lu] [TRS] === renderScreen START ===\n", millis());
      }
      APP_STATE.isRenderComplete = false;
      // Hold state lock ONLY for index/page layout into the frame buffer.
      // E-ink BUSY + grayscale AA must run unlocked — otherwise main loop cannot
      // handle keys/touch for ~1.5s per page (open/turn freezes).
      bool doHalfFlush = false;
      bool doLibraryPhysical = false;
      bool firstLibraryFrame = false;
      bool firstFrameJustReady = false;
      bool firstFrameHasLines = false;
      if (lockState(portMAX_DELAY)) {
        // Always defer e-ink + AA out of the state lock — plugin AND library.
        // finishPhysicalDisplay (PTA loops / HALF-BUSY / gray passes) owns the
        // panel for ~1s; running it under the lock froze keys/touch. The white
        // seed's first content frame also flows through this path.
        deferPhysicalEpd_ = true;
        libraryPhysicalPending_ = false;
        renderScreen();
        deferPhysicalEpd_ = false;
        firstFrameHasLines = !currentPageLines.empty();
        // Ready only when we actually laid out glyphs — empty first paint is not ready.
        if (chapter_initialized && !pageOffsets.empty() && firstFrameHasLines) {
          firstFrameJustReady = !firstReadableLogged_;
          firstPageReady_ = true;
        } else if (chapter_initialized && !pageOffsets.empty() && !firstFrameHasLines) {
          // Retry next tick instead of flushing a blank buffer (looks like "page 1 empty").
          // cachedPage was already set to currentPage → force a reload next tick, or
          // the retry spins forever on the same empty lines (busy loop, page never shows).
          firstPageReady_ = false;
          pluginPendingHalfFlush_ = false;
          cachedPage = -1;
          updateRequired = true;
        }
        doHalfFlush = pluginPendingHalfFlush_ && firstFrameHasLines;
        pluginPendingHalfFlush_ = false;
        // Content must be present before a physical drive — same guard as
        // doHalfFlush (blank first layout would flash a white page).
        doLibraryPhysical = libraryPhysicalPending_ && firstFrameHasLines;
        libraryPhysicalPending_ = false;
        // Do not enqueue TLS here. First-paint + WeRead idle prefetch raced
        // the first FAST flush; with Wi-Fi down that panicked in lwIP.
        // Post-paint idle path in this task starts N+1 once the panel is free.
        unlockState();
      }
      if (firstFrameJustReady) {
        firstReadableLogged_ = true;
        Serial.printf("[WR05] t=%lu first_page_ready kind=%s index_complete=%d\n",
                      static_cast<unsigned long>(millis()), pluginSession_.active ? "plugin" : "library",
                      indexComplete_ ? 1 : 0);
      }
      if (suppressDisplay_ || subActivity) {
        // Child opened while we were laying out — skip physical (child will paint).
        APP_STATE.isRenderComplete = true;
        continue;
      }
      if (overlayReturnFlush_ && !firstFrameHasLines) {
        updateRequired = true;
      } else if (overlayReturnFlush_ && firstFrameHasLines) {
        overlayReturnFlush_ = false;
        physicalEpdBusy_ = true;
        const uint32_t tFlush = millis();
        // Same FAST LUT as the overlay itself. HALF/FULL here is the black
        // invert flash the user sees when tapping the page to dismiss the bar.
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
        firstPhysicalShown_ = true;
        lastPhysicalBodyPage_ = (pendingPhysicalPage_ >= 0) ? pendingPhysicalPage_ : currentPage;
        (void)renderer.storeBwBuffer();
        (void)renderer.storeLastShown();
        Serial.printf("[WR05] t=%lu overlay_return_fast ms=%lu page=%d\n",
                      static_cast<unsigned long>(millis()),
                      static_cast<unsigned long>(millis() - tFlush), lastPhysicalBodyPage_);
        physicalEpdBusy_ = false;
      } else if (doHalfFlush) {
        // One absolute clean after plugin loading residual — not multi-flash FULL.
        // SSD1677: FAST is differential; HALF is BYPASS_RED single-pass (0xD7);
        // FULL is multi-inversion OTP (0xF7). Policy: exactly one guarded handoff
        // HALF; later turns use finishPhysicalDisplay (page-turn anim / FAST).
        //
        // Must seed the same PTA baseline as library firstPhysical HALF:
        // storeLastShown + firstPhysicalShown_. Without that, the first user
        // page-turn re-enters the firstPhysical HALF path (looks like a full
        // flash, no wipe), and lastShown can still hold a previous book's
        // frame → multipass RED≠truth → inverted residual / slow gray ghost.
        physicalEpdBusy_ = true;
        const uint32_t tFlush = millis();
        Serial.printf("[WR05] t=%lu first_frame_half_refresh gen=%u mode=half\n",
                      static_cast<unsigned long>(tFlush),
                      static_cast<unsigned>(pluginSession_.generation));
        const auto handoffMode = M4PluginReaderStatePolicy::pluginFirstHandoffRefreshMode();
        if (handoffMode == M4PluginReaderStatePolicy::PluginFirstHandoffRefresh::Full) {
          renderer.displayBuffer(HalDisplay::FULL_REFRESH);
        } else if (handoffMode == M4PluginReaderStatePolicy::PluginFirstHandoffRefresh::Fast) {
          renderer.displayBuffer(HalDisplay::FAST_REFRESH);
        } else {
          renderer.displayBuffer(HalDisplay::HALF_REFRESH);
        }
        // Seed PTA/old-page baseline (library path does this in finishPhysicalDisplay).
        firstPhysicalShown_ = true;
        lastPhysicalBodyPage_ = (pendingPhysicalPage_ >= 0) ? pendingPhysicalPage_ : currentPage;
        pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
        (void)renderer.storeBwBuffer();  // required for AA / legacy anim prev
        (void)renderer.storeLastShown();
        Serial.printf("[WR05] t=%lu first_frame_half_done ms=%lu gen=%u pta_seed=1\n",
                      static_cast<unsigned long>(millis()),
                      static_cast<unsigned long>(millis() - tFlush),
                      static_cast<unsigned>(pluginSession_.generation));
        physicalEpdBusy_ = false;
      } else if (doLibraryPhysical) {
        firstLibraryFrame = !firstPhysicalShown_;
        physicalEpdBusy_ = true;
        finishPhysicalDisplay();
        physicalEpdBusy_ = false;
        if (firstLibraryFrame) {
          Serial.printf("[WR05] t=%lu first_physical_done kind=library\n",
                        static_cast<unsigned long>(millis()));
        }
      }
      if (firstLibraryFrame || (firstPluginFrame && (doHalfFlush || doLibraryPhysical))) {
        // History is intentionally after the first physical page: this SD
        // write must never delay the initial readable frame.
        persistOpenHistory();
      }
      APP_STATE.isRenderComplete = true;
      // Do NOT rewrite global state JSON every page — thrashing SD (was every frame).
      // openEpubPath is saved once in onEnter.
      // Only mark first physical done after we actually flushed content (not empty retry).
      if (firstPluginFrame && (doHalfFlush || doLibraryPhysical)) {
        loggedFirstPhysical = true;
        Serial.printf("[WR05] t=%lu first_render_end ms=%lu pages=%d complete=%d\n",
                      static_cast<unsigned long>(millis()), static_cast<unsigned long>(millis() - t0), totalPages,
                      indexComplete_ ? 1 : 0);
        Serial.printf("[WR05] t=%lu first_physical_done gen=%u\n", static_cast<unsigned long>(millis()),
                      static_cast<unsigned>(pluginSession_.generation));
      } else if (!firstPluginFrame) {
        Serial.printf("[%lu] [TRS] === renderScreen END ===\n", millis());
      }
    } else if (renderingMutex && firstPageReady_) {
      // Background progressive index: plugin whole-file *and* library chapter
      // (library next-chapter no longer waits for full build under render lock).
      bool doIndex = false;
      bool needSave = false;
      bool wantRedraw = false;
      if (lockState(0)) {
        if (progressSavePending_) {
          saveProgress();
          progressSavePending_ = false;
        }
        doIndex = !indexComplete_;
        if (doIndex) {
          const bool wasComplete = indexComplete_;
          const bool hadPendingRestore = hasPendingRestore_;
          const int added = continuePageIndex(1, 16 * 1024);
          needSave = indexComplete_ && !tidxSaved_;
          const bool appliedRestore = hadPendingRestore && !hasPendingRestore_;
          // Progress/total metadata advances without changing visible glyphs.
          // With AA, re-painting that status flashes the whole page — skip unless
          // restore actually moved the current page.
          M4PluginReaderStatePolicy::BackgroundIndexUpdate indexUpdate;
          indexUpdate.pageChanged = appliedRestore;
          indexUpdate.indexAdvanced = added > 0;
          indexUpdate.indexCompleted = !wasComplete && indexComplete_;
          wantRedraw = M4PluginReaderStatePolicy::backgroundIndexNeedsRedraw(indexUpdate);
          if (!wantRedraw && (indexUpdate.indexAdvanced || indexUpdate.indexCompleted)) {
            Serial.printf("[WR05] t=%lu index_progress_no_redraw added=%d complete=%d page=%d\n",
                          static_cast<unsigned long>(millis()), added, indexComplete_ ? 1 : 0, currentPage);
          }
        }
        unlockState();
      }
      if (wantRedraw) updateRequired = true;
      // Save completed index once (re-take lock; never recursive from inside lock).
      if (needSave && lockState(pdMS_TO_TICKS(200))) {
        if (indexComplete_ && !tidxSaved_) {
          if (pluginSession_.active) {
            savePluginTidx();
          } else {
            chapter_savePageIndexCache(chapternum);
          }
          tidxSaved_ = true;
        }
        unlockState();
      }
      // Current chapter gets the first claim on SD/RAM: finish and persist its
      // page index, then enqueue N+1. Previously this trigger only lived in the
      // render branch, so a progressively-built index could complete here
      // without ever starting background chapter prefetch.
      if (pluginSession_.providerManaged && indexComplete_ && tidxSaved_ &&
          !providerPrefetchRequested_ && !physicalEpdBusy_.load() && !updateRequired) {
        providerIdlePrefetchNext();
      }
      // Next-chapter page-index prefetch (provider-like): once the current
      // chapter is fully indexed and the panel is idle, build chapter N+1's
      // pageOffsets in the background and write chapter{N+1}.bin so a
      // cross-chapter open is a cache hit (SD time-sliced, never during anim).
      if (!pluginSession_.active && indexComplete_ && !physicalEpdBusy_.load() &&
          !updateRequired && lockState(pdMS_TO_TICKS(50))) {
        libraryIdlePrefetchNextChapter();
        unlockState();
      }
      if (!pluginSession_.active && largeTxtFastOpen_ && !chapterDiscoveryDone_ &&
          !physicalEpdBusy_.load() && !updateRequired && lockState(pdMS_TO_TICKS(50))) {
        libraryIdleDiscoverChapterBatch();
        unlockState();
      }
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}


void TxtReaderActivity::chapter_initializeReader(int chapter_num) {
  if (chapter_initialized) {
    return;
  }
  const uint32_t tChInit = millis();

  // 章节重新初始化时清除页面缓存
  cachedPage = -1;

  // 校验章节索引合法性
  if (chapter_num < 0 ) {
    chapter_initialized = true;
    return;
  }

  // Store current settings for cache validation
  cachedFontId = SETTINGS.getReaderFontId();
  cachedParagraphAlignment = SETTINGS.paragraphAlignment;
  cachedScreenMargin = SETTINGS.screenMargin_Top + SETTINGS.screenMargin_Left +
                       SETTINGS.screenMargin_Right + SETTINGS.screenMargin_Bottom;

  // Calculate viewport dimensions
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);


  auto metrics = UITheme::getInstance().getMetrics();

  // Add status bar margin
  if (SETTINGS.statusBar != CrossPointSettings::STATUS_BAR_MODE::NONE) {
    const bool showProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                                 SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::ONLY_BOOK_PROGRESS_BAR ||
                                 SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
    orientedMarginBottom += statusBarMargin  +
                            (showProgressBar ? (metrics.bookProgressBarHeight + progressBarMarginTop) : 0);
  }
  orientedMarginTop += SETTINGS.screenMargin_Top;
  orientedMarginLeft += SETTINGS.screenMargin_Left;
  orientedMarginRight += SETTINGS.screenMargin_Right;
  orientedMarginBottom += SETTINGS.screenMargin_Bottom;
  
  viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  if (isLandscapeDualPage()) {
    // 展开内侧间距 = 装订线间隙 + 左页右边距 + 右页左边距（均取用户设置内侧边距值）
    viewportWidth = (viewportWidth - DUAL_PAGE_GUTTER - SETTINGS.screenMargin_Right - SETTINGS.screenMargin_Left) / 2;
  }
  const int viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
  //行距加这里？
  float lineHeight = renderer.getLineHeight(cachedFontId)* SETTINGS.getReaderLineCompression();

  linesPerPage = viewportHeight / lineHeight;
  if (linesPerPage < 1) linesPerPage = 1;

  Serial.printf("[%lu] [TRS] Viewport: %dx%d, lines per page: %d (chapter %d)\n", millis(), viewportWidth, viewportHeight,
                linesPerPage, chapter_num);

  // Plugin single-file session: whole file is one chapter, progressive index.
  if (pluginSession_.active) {
    const size_t fileSize = txt->getFileSize();
    indexRangeEnd_ = fileSize;
    activeChapterBegin_ = 0;
    activeChapterEnd_ = fileSize;
    // Validate pending restore against opened file size.
    if (hasPendingRestore_ && fileSize > 0 && pendingRestoreByte_ >= fileSize) {
      pendingRestoreByte_ = fileSize - 1;
    }
    if (fileSize == 0) {
      pageOffsets = {0};
      totalPages = 1;
      indexComplete_ = true;
      firstPageReady_ = true;
      hasPendingRestore_ = false;
      chapter_initialized = true;
      return;
    }
    if (loadPluginTidx()) {
      indexComplete_ = true;
      firstPageReady_ = true;
      // Complete index: binary-search page for raw byte before first render.
      applyPendingRestoreIfReady();
      chapter_initialized = true;
      Serial.printf("[%lu] [TRS] Plugin tidx loaded: %d pages page=%d\n", millis(), totalPages, currentPage);
      return;
    }
    if (pluginSession_.progressiveIndex) {
      // First page only — render ASAP; rest continues in displayTaskLoop.
      // Do NOT block first-page rendering by indexing from zero to a far offset.
      const uint32_t tIdx0 = millis();
      Serial.printf("[WR05] t=%lu first_page_index_begin size=%u\n", static_cast<unsigned long>(tIdx0),
                    static_cast<unsigned>(fileSize));
      buildPageIndexFirstPage(0, fileSize);
      firstPageReady_ = !pageOffsets.empty();
      // May apply immediately if target is still on page 0 / first slice.
      applyPendingRestoreIfReady();
      chapter_initialized = true;
      Serial.printf("[WR05] t=%lu first_page_index_end ms=%lu pages=%d complete=%d restore=%d\n",
                    static_cast<unsigned long>(millis()), static_cast<unsigned long>(millis() - tIdx0), totalPages,
                    (int)indexComplete_, (int)hasPendingRestore_);
      return;
    }
    // Non-progressive plugin: full index of whole file (no 100KB chapter cap).
    buildPageIndex(0, fileSize > 0 ? fileSize - 1 : 0);
    indexComplete_ = true;
    firstPageReady_ = true;
    applyPendingRestoreIfReady();
    if (!tidxSaved_) {
      savePluginTidx();
      tidxSaved_ = true;
    }
    chapter_initialized = true;
    return;
  }

  // Large local TXT: open the current raw byte range immediately. Chapter
  // metadata is deliberately cache-only here; missing 25-row batches are
  // discovered from the idle display loop after the first page is usable.
  if (largeTxtFastOpen_ && chapter_num == fastOpenChapter_) {
    const size_t fileSize = txt->getFileSize();
    indexRangeEnd_ = fileSize;
    if (fileSize == 0) {
      pageOffsets = {0};
      totalPages = 1;
      indexComplete_ = true;
      firstPageReady_ = true;
      hasPendingRestore_ = false;
      chapter_initialized = true;
      return;
    }

    const int cachedBatch = std::max(0, (chapter_num / 25) * 25);
    if (txt->hasChapterBatchCache(cachedBatch)) {
      txt->parseChapterIndexAndOffset(cachedBatch, /*allowScan=*/false);
      if (txt->isChapterExist(chapter_num)) {
        const size_t cachedBegin = txt->getChapterOffsetByIndex(chapter_num);
        size_t cachedEnd = txt->getChapterendOffsetByIndex(chapter_num);
        if (cachedEnd == 0 || cachedEnd <= cachedBegin) cachedEnd = fileSize;
        activeChapterBegin_ = cachedBegin;
        activeChapterEnd_ = cachedEnd;
        if (chapter_loadPageIndexCache(chapter_num)) {
          indexRangeEnd_ = cachedEnd;
          indexCursor_ = cachedEnd;
          indexComplete_ = true;
          firstPageReady_ = !pageOffsets.empty();
          applyPendingRestoreIfReady();
          chapter_initialized = true;
          Serial.printf("[%lu] [WR05] large_txt_fast_open cache ch=%d pages=%d\n", millis(),
                        chapter_num, totalPages);
          return;
        }
      }
    }

    size_t begin = 0;
    size_t end = fileSize;
    if (resumeRangeEnd_ > 0 && resumeRangeEnd_ <= fileSize) {
      activeChapterBegin_ = std::min(resumeRangeBegin_, fileSize);
      activeChapterEnd_ = std::max(activeChapterBegin_, resumeRangeEnd_);
      if (activeChapterEnd_ > fileSize) activeChapterEnd_ = fileSize;
      begin = hasPendingRestore_ ? pendingRestoreByte_ : activeChapterBegin_;
      if (begin < activeChapterBegin_ || begin >= activeChapterEnd_) begin = activeChapterBegin_;
      end = activeChapterEnd_;
    } else {
      activeChapterBegin_ = 0;
      activeChapterEnd_ = fileSize;
      if (hasPendingRestore_ && pendingRestoreByte_ < fileSize) begin = pendingRestoreByte_;
    }
    if (begin >= end) begin = 0;
    currentPage = 0;
    const uint32_t tIdx0 = millis();
    Serial.printf("[%lu] [WR05] large_txt_fast_open begin=%zu end=%zu saved=%d\n", tIdx0, begin, end,
                  hasPendingRestore_ ? 1 : 0);
    buildPageIndexFirstPage(begin, end);
    firstPageReady_ = !pageOffsets.empty();
    applyPendingRestoreIfReady();
    chapter_initialized = true;
    Serial.printf("[%lu] [WR05] large_txt_fast_open_ready ms=%lu pages=%d complete=%d\n",
                  static_cast<unsigned long>(millis()), static_cast<unsigned long>(millis() - tIdx0),
                  totalPages, indexComplete_ ? 1 : 0);
    return;
  }

  // Library multi-chapter TXT. Keep first open fast:
  //  - Prefer SD chapters_*_25.bin cache (no multi-second full-file scan).
  //  - Never full-scan a missing high batch from corrupt progress (was 10–30s freeze).
  auto batchStart = [](int ch) {
    if (ch < 0) ch = 0;
    return (ch / 25) * 25;
  };

  auto tryLoadChapterMeta = [&](int ch, bool allowScan) -> bool {
    const int b = batchStart(ch);
    if (!allowScan && !txt->hasChapterBatchCache(b)) return false;
    txt->parseChapterIndexAndOffset(b, allowScan);
    return txt->isChapterExist(ch);
  };

  // Highest chapter present in any on-disk batch cache (cache-only, no full scan).
  auto findLastCachedChapter = [&]() -> int {
    int last = -1;
    // Walk downward from a modest ceiling so open stays quick even with many caches.
    constexpr int kProbeMax = 2500;  // 100 batch files max
    int b = batchStart(std::min(std::max(chapter_num, 0), kProbeMax));
    // First jump to highest existing cache near progress, then scan down.
    for (int guard = 0; guard < 120 && b >= 0; ++guard, b -= 25) {
      if (!txt->hasChapterBatchCache(b)) continue;
      txt->parseChapterIndexAndOffset(b, /*allowScan=*/false);
      for (int i = 24; i >= 0; --i) {
        if (txt->isChapterExist(b + i)) {
          last = b + i;
          return last;  // first (highest) hit while walking down
        }
      }
    }
    // No cache near progress: try batch 0 from cache or one scan.
    if (txt->hasChapterBatchCache(0)) {
      txt->parseChapterIndexAndOffset(0, /*allowScan=*/false);
    } else {
      txt->parseChapterIndexAndOffset(0, /*allowScan=*/true);
    }
    for (int i = 24; i >= 0; --i) {
      if (txt->isChapterExist(i)) last = i;
    }
    return last;
  };

  bool haveChapter = false;
  const int wantBatch = batchStart(chapter_num);
  if (txt->hasChapterBatchCache(wantBatch)) {
    haveChapter = tryLoadChapterMeta(chapter_num, /*allowScan=*/false);
    if (!haveChapter) {
      // Cache present but entry missing — allow one rebuild scan for this batch only.
      haveChapter = tryLoadChapterMeta(chapter_num, /*allowScan=*/true);
    }
  } else if (chapter_num < 25) {
    // Early chapter, no cache yet: one scan of batch 0 is expected on first open.
    haveChapter = tryLoadChapterMeta(chapter_num, /*allowScan=*/true);
  } else {
    // High chapter without batch file: do NOT full-scan (corrupt progress path).
    Serial.printf("[%lu] [TRS] chapter %d batch %d uncached — skip full scan\n", millis(), chapter_num,
                  wantBatch);
    haveChapter = false;
  }

  if (!haveChapter) {
    const int fallback = findLastCachedChapter();
    Serial.printf("[%lu] [TRS] chapter %d missing — fallback=%d\n", millis(), chapter_num, fallback);
    if (fallback >= 0) {
      chapter_num = fallback;
      chapternum = fallback;
      currentPage = 0;
      tryLoadChapterMeta(chapter_num, /*allowScan=*/false);
      if (!txt->isChapterExist(chapter_num)) {
        tryLoadChapterMeta(chapter_num, /*allowScan=*/true);
      }
      saveProgress();
      Serial.printf("[%lu] [TRS] repaired progress → chapter %d page 0\n", millis(), chapternum);
    } else {
      const size_t fs = txt->getFileSize();
      Serial.printf("[%lu] [TRS] no chapters detected, open whole file size=%zu\n", millis(), fs);
      if (fs == 0) {
        pageOffsets.clear();
        totalPages = 0;
      } else {
        size_t end = fs - 1;
        constexpr size_t kMaxOpen = 200000;
        if (end > kMaxOpen) end = kMaxOpen;
        buildPageIndex(0, end);
      }
      indexComplete_ = true;
      firstPageReady_ = !pageOffsets.empty();
      chapter_initialized = true;
      return;
    }
  }

  if (!chapter_loadPageIndexCache(chapter_num)) {
    Serial.printf("[%lu] [TRS] load txtchapter: %d \n", millis(), chapter_num);
    if (!txt->isChapterExist(chapter_num)) {
      tryLoadChapterMeta(chapter_num, /*allowScan=*/true);
    }
    size_t chapterOffsetbegin = txt->getChapterOffsetByIndex(chapter_num);
    size_t chapterOffsetend = txt->getChapterendOffsetByIndex(chapter_num);

    const size_t fileSize = txt->getFileSize();
    if (chapterOffsetend == 0 || chapterOffsetend <= chapterOffsetbegin) {
      chapterOffsetend = fileSize;
    }
    // Cap runaway ranges (corrupt endOffset) but allow large real chapters.
    constexpr size_t kMaxChapterBytes = 2 * 1024 * 1024;
    if (chapterOffsetend > chapterOffsetbegin &&
        chapterOffsetend - chapterOffsetbegin > kMaxChapterBytes) {
      Serial.printf("[%lu] [TRS] chapter %d range too large (%zu), cap to %zu\n", millis(), chapter_num,
                    chapterOffsetend - chapterOffsetbegin, kMaxChapterBytes);
      chapterOffsetend = chapterOffsetbegin + kMaxChapterBytes;
    }
    if (chapterOffsetbegin >= fileSize || chapterOffsetend <= chapterOffsetbegin) {
      Serial.printf("[%lu] [TRS] invalid chapter range begin=%zu end=%zu fs=%zu\n", millis(),
                    chapterOffsetbegin, chapterOffsetend, fileSize);
      pageOffsets.clear();
      totalPages = 0;
      indexComplete_ = true;
      firstPageReady_ = false;
      tidxSaved_ = true;
      chapter_initialized = true;
      return;
    }
    activeChapterBegin_ = chapterOffsetbegin;
    activeChapterEnd_ = chapterOffsetend;
    // Fast open (esp. next-chapter): first page only, rest in displayTaskLoop.
    // Full buildPageIndex under the render lock made large chapters multi-second lag.
    const uint32_t tIdx0 = millis();
    Serial.printf("[%lu] [TRS] library first_page_index_begin ch=%d range=%zu..%zu\n", millis(),
                  chapter_num, chapterOffsetbegin, chapterOffsetend);
    buildPageIndexFirstPage(chapterOffsetbegin, chapterOffsetend);
    // Resume mid-chapter (saved page N) or prev-chapter last page: need enough
    // index before first paint. Next-chapter opens with currentPage=0 → no wait.
    if (!indexComplete_ && currentPage != 0) {
      if (currentPage < 0) {
        while (!indexComplete_) {
          if (continuePageIndex(16, 256 * 1024) == 0 && !indexComplete_) break;
        }
      } else {
        while (!indexComplete_ && totalPages <= currentPage) {
          if (continuePageIndex(16, 256 * 1024) == 0 && !indexComplete_) break;
        }
      }
    }
    Serial.printf("[%lu] [TRS] library first_page_index_end ch=%d ms=%lu pages=%d complete=%d\n",
                  millis(), chapter_num, static_cast<unsigned long>(millis() - tIdx0), totalPages,
                  indexComplete_ ? 1 : 0);
    if (indexComplete_) {
      chapter_savePageIndexCache(chapter_num);
      tidxSaved_ = true;
    } else {
      tidxSaved_ = false;
    }
  } else {
    indexComplete_ = true;
    tidxSaved_ = true;
    activeChapterBegin_ = txt->getChapterOffsetByIndex(chapter_num);
    activeChapterEnd_ = txt->getChapterendOffsetByIndex(chapter_num);
    if (activeChapterEnd_ == 0 || activeChapterEnd_ <= activeChapterBegin_) {
      activeChapterEnd_ = txt->getFileSize();
    }
  }

  firstPageReady_ = !pageOffsets.empty();
  chapter_initialized = true;
}

void TxtReaderActivity::buildPageIndex(size_t beginByte, size_t endByte) {
  pageOffsets.clear();
  
  // 1. 参数合法性校验，避免越界
  const size_t fileSize = txt->getFileSize();
  beginByte = std::min(beginByte, fileSize);  
  endByte = std::min(endByte, fileSize);    
  if (beginByte >= endByte) {
    Serial.printf("[%lu] [TRS] Invalid range: begin=%zu, end=%zu (file size=%zu)\n", 
                  millis(), beginByte, endByte, fileSize);
    totalPages = 0;
    return;
  }

  // 2. 初始页从指定的beginByte开始
  pageOffsets.push_back(beginByte);  

  size_t offset = beginByte;
  Serial.printf("[%lu] [TRS] Building page index from %zu to %zu bytes...\n", 
                millis(), beginByte, endByte);

  // ========== 优化：一次性读取整个章节到内存，避免重复 SD 卡 I/O ==========
  const size_t chapterSize = endByte - beginByte;
  uint8_t* chapterBuf = nullptr;
  bool useChapterBuf = false;
  
  // 章节小于 32KB 时一次性读入内存（ESP32-C3 有 320KB RAM）
  if (chapterSize > 0 && chapterSize <= 32 * 1024) {
    chapterBuf = static_cast<uint8_t*>(malloc(chapterSize + 1));
    if (chapterBuf) {
      if (txt->readContent(chapterBuf, beginByte, chapterSize, false)) {
        chapterBuf[chapterSize] = '\0';
        useChapterBuf = true;
        Serial.printf("[%lu] [TRS] Chapter loaded to RAM: %zu bytes\n", millis(), chapterSize);
      } else {
        free(chapterBuf);
        chapterBuf = nullptr;
      }
    }
  }

  // 页面计数器，用于控制进度更新频率
  int pageCount = 0;

  // 使用 loadPageAtOffset 构建索引（预读 buffer 避免重复 SD I/O，保证分页一致性）
  std::vector<std::string> tempLines;
  std::vector<bool> justifyScratch;  // never clobber on-screen currentPageJustify
  while (offset < endByte) {
    tempLines.clear();
    size_t nextOffset = offset;

    if (useChapterBuf) {
      if (!loadPageAtOffset(offset, endByte, tempLines, nextOffset,
                            chapterBuf, beginByte, chapterSize, 0, &justifyScratch)) break;
    } else {
      if (!loadPageAtOffset(offset, endByte, tempLines, nextOffset,
                            nullptr, 0, 0, 0, &justifyScratch)) break;
    }
    if (nextOffset <= offset) break;

    // 如果本页没有产生任何内容行（全是空行/空白），跳过不计为一页
    if (tempLines.empty()) {
      offset = nextOffset;
      continue;
    }

    offset = nextOffset;
    if (offset < endByte) {
      pageOffsets.push_back(offset);
    }
    pageCount++;
  }

  totalPages = pageOffsets.size();
  Serial.printf("[%lu] [TRS] Built page index: %d pages (range %zu-%zu bytes)\n", 
                millis(), totalPages, beginByte, endByte);
  
  // 释放预读 buffer
  if (chapterBuf) {
    free(chapterBuf);
  }
  indexComplete_ = true;
  indexCursor_ = endByte + 1;
}

size_t TxtReaderActivity::chapterContentEnd() const {
  if (!txt) return 0;
  if (pluginSession_.active) return txt->getFileSize();
  if (largeTxtFastOpen_ && chapternum == fastOpenChapter_ && activeChapterEnd_ > 0) {
    return activeChapterEnd_;
  }
  size_t endoffset = txt->getChapterendOffsetByIndex(chapternum);
  if (endoffset > 0) return endoffset;  // existing call sites subtract 1 for inclusive
  return txt->getFileSize();
}

void TxtReaderActivity::buildPageIndexFirstPage(size_t beginByte, size_t endByteExclusive) {
  // Whole-file chapter is finalized (fixed length). First physical frame only
  // needs page 0: loadPageAtOffset must *read* a growing bounded window
  // (maxReadBytes = window size), not re-read a fixed 8KiB while windowEnd grows.
  // Cap at kMaxPageReadBytes (48KiB heap). At cap without full page: stop with
  // best-effort page end — never leap maxBytes → whole fileSize.
  pageOffsets.clear();
  const size_t fileSize = txt ? txt->getFileSize() : 0;
  beginByte = std::min(beginByte, fileSize);
  endByteExclusive = std::min(endByteExclusive, fileSize);
  indexRangeEnd_ = endByteExclusive;
  if (beginByte >= endByteExclusive) {
    pageOffsets.push_back(beginByte);
    totalPages = 1;
    indexComplete_ = true;
    indexCursor_ = endByteExclusive;
    return;
  }
  pageOffsets.push_back(beginByte);

  using M4ProgressiveTxtIndex::FirstPageWindowPolicy;
  using M4ProgressiveTxtIndex::firstPageLayoutComplete;
  using M4ProgressiveTxtIndex::growFirstPageWindow;
  FirstPageWindowPolicy pol;
  pol.initialBytes = CHUNK_SIZE;
  pol.stepBytes = CHUNK_SIZE;
  pol.maxBytes = kMaxPageReadBytes;

  size_t windowEnd = beginByte + pol.initialBytes;
  if (windowEnd > endByteExclusive) windowEnd = endByteExclusive;

  std::vector<std::string> tempLines;
  std::vector<bool> justifyScratch;  // first-page index only; keep live layout flags intact
  size_t nextOffset = beginByte;
  int expansions = 0;
  size_t lastReadCap = 0;
  for (int guard = 0; guard < 32; ++guard) {
    tempLines.clear();
    nextOffset = beginByte;
    // Real read window: must match windowEnd so expansion is not a no-op re-read of 8KiB.
    const size_t readCap = windowEnd - beginByte;
    lastReadCap = readCap;
    if (!loadPageAtOffset(beginByte, windowEnd, tempLines, nextOffset, nullptr, 0, 0, readCap, &justifyScratch)) {
      if (windowEnd >= endByteExclusive) {
        totalPages = 1;
        indexComplete_ = true;
        indexCursor_ = endByteExclusive;
        return;
      }
      const size_t grown = growFirstPageWindow(beginByte, windowEnd, endByteExclusive, pol);
      if (grown <= windowEnd) {
        // At maxBytes cap with empty/unreadable content — stop (do not jump to EOF).
        totalPages = 1;
        indexComplete_ = (windowEnd >= endByteExclusive);
        indexCursor_ = windowEnd >= endByteExclusive ? endByteExclusive : beginByte + 1;
        if (indexCursor_ < endByteExclusive && pageOffsets.size() == 1) {
          pageOffsets.push_back(indexCursor_);
        }
        Serial.printf("[WR05] t=%lu first_page_cap empty expansions=%d readCap=%zu\n",
                      static_cast<unsigned long>(millis()), expansions, lastReadCap);
        totalPages = static_cast<int>(pageOffsets.size());
        return;
      }
      windowEnd = grown;
      ++expansions;
      continue;
    }
    if (nextOffset <= beginByte) nextOffset = beginByte + 1;

    const bool complete = firstPageLayoutComplete(static_cast<int>(tempLines.size()), linesPerPage, nextOffset,
                                                  windowEnd, endByteExclusive);
    if (complete) break;

    const size_t grown = growFirstPageWindow(beginByte, windowEnd, endByteExclusive, pol);
    if (grown <= windowEnd) {
      // Cap reached with partial page — keep nextOffset; continue background from there.
      Serial.printf("[WR05] t=%lu first_page_cap partial expansions=%d readCap=%zu next=%zu\n",
                    static_cast<unsigned long>(millis()), expansions, lastReadCap, nextOffset);
      break;
    }
    // Require real growth of the read budget.
    if (grown - beginByte <= lastReadCap) break;
    windowEnd = grown;
    ++expansions;
  }

  if (nextOffset >= endByteExclusive) {
    indexComplete_ = true;
    indexCursor_ = endByteExclusive;
  } else {
    pageOffsets.push_back(nextOffset);
    indexCursor_ = nextOffset;
    indexComplete_ = false;
  }
  totalPages = static_cast<int>(pageOffsets.size());
  Serial.printf("[WR05] t=%lu first_page_window expansions=%d readCap=%zu next=%zu pages=%d complete=%d\n",
                static_cast<unsigned long>(millis()), expansions, lastReadCap, indexCursor_, totalPages,
                (int)indexComplete_);
}

int TxtReaderActivity::continuePageIndex(int maxPages, size_t maxBytes) {
  // Caller holds renderingMutex (display task / UI frontier).
  if (indexComplete_ || !txt) return 0;
  int added = 0;
  size_t bytes = 0;
  std::vector<std::string> tempLines;
  // Critical: progressive index runs while a page is on screen. Writing
  // currentPageJustify here made status-bar redraws flip lines between left
  // and justified (段尾「分散开」闪烁).
  std::vector<bool> justifyScratch;
  while (added < maxPages && bytes < maxBytes && !indexComplete_) {
    if (indexCursor_ >= indexRangeEnd_) {
      indexComplete_ = true;
      break;
    }
    size_t nextOffset = indexCursor_;
    const size_t before = indexCursor_;
    if (!loadPageAtOffset(indexCursor_, indexRangeEnd_, tempLines, nextOffset,
                          nullptr, 0, 0, 0, &justifyScratch)) {
      indexComplete_ = true;
      break;
    }
    if (nextOffset <= indexCursor_) nextOffset = indexCursor_ + 1;
    bytes += (nextOffset - before);
    if (nextOffset >= indexRangeEnd_) {
      indexCursor_ = indexRangeEnd_;
      indexComplete_ = true;
      break;
    }
    pageOffsets.push_back(nextOffset);
    indexCursor_ = nextOffset;
    ++added;
  }
  totalPages = static_cast<int>(pageOffsets.size());
  // Progressive restore: once index reaches/passes target, select page once.
  applyPendingRestoreIfReady();
  return added;
}

bool TxtReaderActivity::loadPluginTidx() {
  if (!txt || !pluginSession_.active) return false;
  const std::string path = txt->getPath() + ".tidx";
  const std::string bak = path + ".bak";
  const std::string tmp = path + ".tmp";

  // Recover interrupted replace: if only .bak remains, restore it.
  if (!SdMan.exists(path.c_str()) && SdMan.exists(bak.c_str())) {
    (void)SdMan.rename(bak.c_str(), path.c_str());
  }
  // Stale tmp is never authoritative — remove so we do not accept partial writes.
  if (SdMan.exists(tmp.c_str())) (void)SdMan.remove(tmp.c_str());

  FsFile f;
  if (!SdMan.openFileForRead("TRS", path.c_str(), f)) return false;
  const size_t n = f.fileSize();
  if (n < sizeof(M4PluginReaderBridge::TidxHeader) || n > (sizeof(M4PluginReaderBridge::TidxHeader) +
                                                           M4PluginTidxCodec::kMaxPages * sizeof(uint32_t))) {
    f.close();
    return false;
  }
  std::vector<uint8_t> buf(n);
  const int got = f.read(buf.data(), n);
  f.close();
  if (got < 0 || static_cast<size_t>(got) != n) return false;

  const uint32_t layoutFp = M4PluginTidxCodec::layoutFingerprint(
      viewportWidth, linesPerPage, cachedFontId, cachedScreenMargin, txt->getEncodingType());
  const auto decoded =
      M4PluginTidxCodec::decode(buf.data(), buf.size(), static_cast<uint32_t>(txt->getFileSize()), layoutFp);
  if (!decoded.ok || decoded.offsets.empty()) {
    Serial.printf("[%lu] [TRS] Plugin tidx reject: %s\n", millis(), decoded.error);
    return false;
  }
  pageOffsets.clear();
  pageOffsets.reserve(decoded.offsets.size());
  for (uint32_t off : decoded.offsets) pageOffsets.push_back(static_cast<size_t>(off));
  totalPages = static_cast<int>(pageOffsets.size());
  indexComplete_ = true;
  indexCursor_ = txt->getFileSize();
  tidxSaved_ = true;  // live index already on disk
  // Drop stale .bak after a valid live index has been decoded.
  if (SdMan.exists(bak.c_str())) (void)SdMan.remove(bak.c_str());
  return totalPages >= 1;
}

void TxtReaderActivity::savePluginTidx() const {
  if (!txt || !pluginSession_.active || !indexComplete_ || pageOffsets.empty()) return;
  if (pageOffsets.size() > M4PluginTidxCodec::kMaxPages) return;

  const uint32_t layoutFp = M4PluginTidxCodec::layoutFingerprint(
      viewportWidth, linesPerPage, cachedFontId, cachedScreenMargin, txt->getEncodingType());
  std::vector<uint32_t> offs;
  offs.reserve(pageOffsets.size());
  for (size_t o : pageOffsets) offs.push_back(static_cast<uint32_t>(o));
  std::vector<uint8_t> encoded;
  if (!M4PluginTidxCodec::encodeComplete(static_cast<uint32_t>(txt->getFileSize()), layoutFp, offs, encoded)) {
    return;
  }

  const std::string path = txt->getPath() + ".tidx";
  const std::string tmp = path + ".tmp";
  const std::string bak = path + ".bak";

  // Atomic replace plan: write+verify tmp, then rename without deleting live first.
  FsFile f;
  if (!SdMan.openFileForWrite("TRS", tmp.c_str(), f)) return;
  const size_t wrote = f.write(encoded.data(), encoded.size());
  f.close();
  if (wrote != encoded.size()) {
    if (SdMan.exists(tmp.c_str())) (void)SdMan.remove(tmp.c_str());
    return;
  }
  // Verify tmp size.
  {
    FsFile v;
    if (!SdMan.openFileForRead("TRS", tmp.c_str(), v)) {
      if (SdMan.exists(tmp.c_str())) (void)SdMan.remove(tmp.c_str());
      return;
    }
    const size_t vsz = static_cast<size_t>(v.fileSize());
    v.close();
    if (vsz != encoded.size()) {
      if (SdMan.exists(tmp.c_str())) (void)SdMan.remove(tmp.c_str());
      return;
    }
  }

  // Move live → bak (keeps old valid index until replace succeeds).
  if (SdMan.exists(bak.c_str())) (void)SdMan.remove(bak.c_str());
  const bool hadLive = SdMan.exists(path.c_str());
  if (hadLive) {
    if (!SdMan.rename(path.c_str(), bak.c_str())) {
      // Cannot safely replace; keep live, drop tmp.
      if (SdMan.exists(tmp.c_str())) (void)SdMan.remove(tmp.c_str());
      return;
    }
  }
  if (!SdMan.rename(tmp.c_str(), path.c_str())) {
    // Restore bak if we had a live index.
    if (hadLive && SdMan.exists(bak.c_str())) {
      (void)SdMan.rename(bak.c_str(), path.c_str());
    }
    if (SdMan.exists(tmp.c_str())) (void)SdMan.remove(tmp.c_str());
    return;
  }
  if (SdMan.exists(bak.c_str())) (void)SdMan.remove(bak.c_str());
  if (SdMan.exists(tmp.c_str())) (void)SdMan.remove(tmp.c_str());
  Serial.printf("[%lu] [TRS] Plugin tidx saved pages=%zu\n", millis(), pageOffsets.size());
}

bool TxtReaderActivity::loadPageAtOffset(size_t offset, size_t endOffset, std::vector<std::string>& outLines,
                                         size_t& nextOffset, const uint8_t* preloadBuf, size_t preloadBufOffset,
                                         size_t preloadBufSize, size_t maxReadBytes, std::vector<bool>* outJustify) {
  const uint32_t tLoad0 = millis();
  outLines.clear();
  // Index builders pass outJustify scratch so progressive indexing never clears
  // currentPageJustify (live on-screen layout → mid-read left/justify flicker).
  // Display path leaves outJustify null and writes currentPageJustify.
  std::vector<bool>& justifyOut = outJustify ? *outJustify : currentPageJustify;
  justifyOut.clear();
  const size_t fileSize = txt->getFileSize();

  if (offset >= fileSize) {
    return false;
  }

  // 章节边界检查：如果已经到达或超过章节结束位置，不再读取
  if (endOffset > 0 && offset >= endOffset) {
    return false;
  }

  // Explicit read-window cap. Default CHUNK_SIZE (8KiB). First-page adaptive path
  // passes growing 8/16/.../48KiB so window expansion is a real larger SD read.
  // Hard-capped at kMaxPageReadBytes to bound heap (no PSRAM dependency).
  size_t readCap = (maxReadBytes == 0) ? CHUNK_SIZE : maxReadBytes;
  if (readCap > kMaxPageReadBytes) readCap = kMaxPageReadBytes;
  if (readCap < 1) readCap = CHUNK_SIZE;

  // 如果有预读 buffer，直接使用指针，避免 malloc
  uint8_t* buffer = nullptr;
  size_t chunkSize = 0;
  bool needFree = false;
  
  if (preloadBuf && offset >= preloadBufOffset && offset < preloadBufOffset + preloadBufSize) {
    // 直接指向预读 buffer 的切片，零拷贝
    size_t bufOff = offset - preloadBufOffset;
    chunkSize = std::min(readCap, preloadBufSize - bufOff);
    chunkSize = std::min(chunkSize, fileSize - offset);
    // 限制在章节结束位置内
    if (endOffset > offset) {
      chunkSize = std::min(chunkSize, endOffset - offset);
    }
    buffer = const_cast<uint8_t*>(preloadBuf + bufOff);
    needFree = false; // 不需要释放，指向预读 buffer
  } else {
    // 正常从 SD 卡读取 — chunkSize honors readCap (may be > 8KiB for first page).
    chunkSize = std::min(readCap, fileSize - offset);
    // 限制在章节结束位置内
    if (endOffset > offset) {
      chunkSize = std::min(chunkSize, endOffset - offset);
    }
    buffer = static_cast<uint8_t*>(
        PsramRawAlloc(chunkSize + 1));
    if (!buffer) {
      Serial.printf("[%lu] [TRS] Failed to allocate %zu bytes (readCap=%zu)\n", millis(), chunkSize, readCap);
      logPageLoadFail("raw_alloc", offset, chunkSize);
      return false;
    }
    if (!txt->readContent(buffer, offset, chunkSize, false)) {
      free(buffer);
      logPageLoadFail("raw_read", offset, chunkSize);
      return false;
    }
    buffer[chunkSize] = '\0';
    needFree = true;
  }
  
  // Non-UTF-8: stream-decode the whole raw window once with exact CpBoundary map.
  // Layout/word-wrap runs only on UTF-8; pos is mapped back to raw via the map.
  // Never per-line gbkChunkToUtf8 (breaks mid-line page splits / carry).
  const bool isGbk = txt->isGbkEncoding();
  const bool isUtf16 = txt->isUtf16Encoding();
  const bool mappedDecode = isGbk || isUtf16;
  // GBK/UTF-16 decode window (raw*3+16 ~144KB) must not live on internal RAM —
  // with the TTF face resident the resize fails and the page load is skipped.
  PsramVec<uint8_t> decodedOwned;
  PsramVec<M4TxtEncoding::CpBoundary> cpMap;
  size_t decodeWindowPos = offset;   // absolute raw base of map rawEnd
  size_t mappedExactNext = offset;   // fallback raw end of last complete CP in window
  // Raw window facts (for EOF / complete-line checks — never mix with UTF-8 indices)
  const size_t rawLimit = (endOffset > 0 && endOffset < fileSize) ? endOffset : fileSize;
  const size_t rawChunkStart = offset;
  const size_t rawChunkLen = chunkSize;  // still raw size until we reassign chunkSize
  const bool rawWindowReachesLimit = (rawChunkStart + rawChunkLen >= rawLimit);

  if (mappedDecode && buffer && rawChunkLen > 0) {
    M4TxtEncoding::TxtEnc enc = M4TxtEncoding::TxtEnc::Utf8;
    M4TxtEncoding::GbkLookupFn gbkFn = nullptr;
    if (isGbk) {
      enc = M4TxtEncoding::TxtEnc::Gbk;
      gbkFn = [](uint8_t lead, uint8_t trail) -> uint16_t {
        return ::gbkLookup(getGbkTable(), lead, trail);
      };
    } else if (txt->getEncodingType() == TXT_ENCODING_UTF16BE) {
      enc = M4TxtEncoding::TxtEnc::Utf16Be;
    } else {
      enc = M4TxtEncoding::TxtEnc::Utf16Le;
    }

    size_t bomSkipInChunk = 0;
    if (offset == 0 && txt->getContentBomSkip() > 0) {
      bomSkipInChunk = txt->getContentBomSkip();
      if (bomSkipInChunk > rawChunkLen) bomSkipInChunk = rawChunkLen;
    }
    decodeWindowPos = offset + bomSkipInChunk;
    const size_t rawPayload = rawChunkLen - bomSkipInChunk;
    const size_t outCap = rawPayload * 3 + 16;
    const size_t mapCap = rawPayload + 4;
    if (!decodedOwned.resize(outCap) || !cpMap.resize(mapCap)) {
      Serial.printf("[%lu] [TRS] decode window alloc failed raw=%zu out=%zu map=%zu\n", millis(),
                    rawPayload, outCap, mapCap);
      logPageLoadFail("decode_alloc", offset, outCap);
      if (needFree) {
        free(buffer);
        needFree = false;
      }
      return false;
    }
    size_t outLen = 0, mapLen = 0;
    M4TxtEncoding::StreamDecoder dec;
    dec.reset(enc);
    const size_t consumed =
        dec.decode(buffer + bomSkipInChunk, rawPayload, reinterpret_cast<char*>(decodedOwned.data), outCap, &outLen,
                   gbkFn, cpMap.data, mapCap, &mapLen);
    // At true raw EOF only: flush residual incomplete sequences into UTF-8.
    // Attribute flush CPs to end of raw window (not rawEnd=0) so maps stay monotonic.
    if (rawWindowReachesLimit && dec.hasResidual() && outLen < outCap) {
      const uint32_t flushRawEnd = static_cast<uint32_t>(rawPayload);
      size_t fl = 0, m2 = 0;
      const size_t beforeMap = mapLen;
      dec.flush(reinterpret_cast<char*>(decodedOwned.data) + outLen, outCap - outLen, &fl,
                cpMap.data + mapLen, mapCap > mapLen ? mapCap - mapLen : 0, &m2);
      outLen += fl;
      mapLen += m2;
      for (size_t mi = beforeMap; mi < mapLen; ++mi) {
        if (cpMap.data[mi].rawEnd == 0) cpMap.data[mi].rawEnd = flushRawEnd;
      }
      (void)consumed;
    }
    decodedOwned.len = outLen;
    cpMap.len = mapLen;
    if (rawWindowReachesLimit) {
      mappedExactNext = rawLimit;
    } else if (mapLen > 0) {
      mappedExactNext = decodeWindowPos + cpMap.data[mapLen - 1].rawEnd;
    } else {
      mappedExactNext = decodeWindowPos;
    }
    if (needFree) {
      free(buffer);
      needFree = false;
    }
    buffer = (decodedOwned.len == 0) ? nullptr : decodedOwned.data;
    chunkSize = decodedOwned.len;  // now UTF-8 byte length
  }

  // Parse lines from UTF-8 buffer (or raw UTF-8 file)
  size_t pos = 0;

  // 首行缩进控制变量
  const std::string indentStr = "\xe3\x80\x80\xe3\x80\x80"; // 两个全角空格
  const int indentWidth = renderer.getTextWidth(cachedFontId, "中")*2; // 缩进宽度
  bool needIndent = false; // 只有空行后或有前导空格的行才缩进
  bool isOriginalLine = true; // 标记是否是原生行（未拆行）
  
  // 缓存字符宽度，避免循环内重复调用
  const int avgCharWidth = renderer.getTextWidth(cachedFontId, "中");
  // 标点宽度模式：标准模式下中文标点补偿到汉字宽度
  const bool punctStandard = (SETTINGS.chinesePunctWidth == CrossPointSettings::PUNCT_STANDARD);
  const int cjkCharWidth = avgCharWidth;  // 汉字参考宽度
  // Decoded buffer is always UTF-8 when mappedDecode; never reset per-line carry.
  while (pos < chunkSize && static_cast<int>(outLines.size()) < linesPerPage) {
    // Find end of line in UTF-8 buffer
    size_t lineEnd = pos;
    while (lineEnd < chunkSize && buffer[lineEnd] != '\n') {
      lineEnd++;
    }

    // Complete line if we saw '\n' in the decoded window, OR the raw window already
    // reached file/chapter limit (no more raw bytes after this window). Never mix
    // UTF-8 indices with raw fileSize (offset + lineEnd is wrong for GBK/UTF-16).
    const bool lineComplete = (lineEnd < chunkSize) || rawWindowReachesLimit;

    if (!lineComplete && static_cast<int>(outLines.size()) > 0) {
      // Incomplete line and we already have some lines, stop here
      break;
    }

    // Calculate the actual length of line content in the buffer (excluding newline)
    size_t lineContentLen = lineEnd - pos;

    // Check for carriage return
    bool hasCR = (lineContentLen > 0 && buffer[pos + lineContentLen - 1] == '\r');
    size_t displayLen = hasCR ? lineContentLen - 1 : lineContentLen;

    // UTF-8 line content (already decoded for GBK/UTF-16)
    std::string rawLine(reinterpret_cast<char*>(buffer + pos), displayLen);

    // 直接使用行内容，不做标点转换。
    std::string line = rawLine;

    // 段落分隔检测（参考正则 \s*\n\s*\n+）：
    // 空行或仅含空白的行视为段落分隔符，下一个非空行需要缩进。
    bool isBlankLine = true;
    const size_t lineCheckLen = rawLine.size(); // 用转换后的实际长度
    for (size_t k = 0; k < lineCheckLen; k++) {
      uint8_t ch = (uint8_t)rawLine[k];
      if (ch != ' ' && ch != '\t' && ch != '\r') {
        // 检测全角空格（UTF-8: E3 80 80）
        if (ch == 0xE3 && k + 2 < lineCheckLen &&
            (uint8_t)rawLine[k+1] == 0x80 && (uint8_t)rawLine[k+2] == 0x80) {
          k += 2; // 跳过全角空格的后续字节
          continue;
        }
        isBlankLine = false;
        break;
      }
    }

    if (displayLen == 0 || isBlankLine) {
      pos = lineEnd + 1;
      needIndent = true; // 空行/空白行后，下一段需要缩进
      isOriginalLine = true;
      continue;
    }

    // ========== 段落内行合并 ==========
    // 很多 TXT 文件中，一个段落被分成多个短行（每行以 \n 结尾）。
    // 如果逐行独立渲染，行末会有大量空白。
    // 参考 splitParagraphs 的做法：空行分隔段落，段落内的多行合并为连续文本，
    // 再由排版引擎（word-wrap）重新换行。
    //
    // 实现方式：提取当前非空行后，循环读取后续非空行并追加到 line 中，
    // 同时更新 lineEnd 和 displayLen。遇到空行或累积文本超过限制时停止。
    // 合并后的长文本交给现有的 word-wrap 逻辑处理。
    //
    // mergeSegments 记录每个合并段的信息，用于部分消费时正确计算缓冲区偏移。
    // 每个元素是 (段在缓冲区中的起始位置, 段的显示字节数, 段在缓冲区中的行尾位置)。
    struct MergeSeg { size_t bufStart; size_t dispLen; size_t bufLineEnd; };
    std::vector<MergeSeg> mergeSegments;
    mergeSegments.push_back({pos, displayLen, lineEnd}); // 第一段：当前行
    {
      // 合并长度上限：viewportWidth * 3 字节，约 3 行文本量，避免内存问题
      const size_t maxMergeLen = static_cast<size_t>(viewportWidth) * 3;

      while (lineEnd + 1 < chunkSize && line.length() < maxMergeLen) {
        size_t nextLineStart = lineEnd + 1; // 跳过当前行的 \n
        size_t nextLineEnd = nextLineStart;

        // 查找下一行的 \n 位置
        while (nextLineEnd < chunkSize && buffer[nextLineEnd] != '\n') {
          nextLineEnd++;
        }

        // Complete if '\n' present in decoded window or raw window already at limit
        const bool nextComplete = (nextLineEnd < chunkSize) || rawWindowReachesLimit;
        if (!nextComplete) break; // 不完整的行不合并，留给下次读取

        // 计算下一行的显示长度（去除 \r）
        size_t nextContentLen = nextLineEnd - nextLineStart;
        bool nextHasCR = (nextContentLen > 0 &&
                          buffer[nextLineStart + nextContentLen - 1] == '\r');
        size_t nextDisplayLen = nextHasCR ? nextContentLen - 1 : nextContentLen;

        // 检查下一行是否为空行/空白行（段落分隔符）
        bool nextIsBlank = true;
        for (size_t k = 0; k < nextDisplayLen; k++) {
          uint8_t ch = buffer[nextLineStart + k];
          if (ch != ' ' && ch != '\t' && ch != '\r') {
            // 检测全角空格（UTF-8: E3 80 80）
            if (ch == 0xE3 && k + 2 < nextDisplayLen &&
                buffer[nextLineStart + k + 1] == 0x80 &&
                buffer[nextLineStart + k + 2] == 0x80) {
              k += 2; // 跳过全角空格的后续字节
              continue;
            }
            nextIsBlank = false;
            break;
          }
        }

        // 遇到空行/空白行，说明段落结束，停止合并
        if (nextDisplayLen == 0 || nextIsBlank) break;

        // 检测下一行是否有首行缩进（有缩进说明是新段落的开头，不应合并）
        bool nextHasIndent = false;
        if (nextDisplayLen >= 1) {
          uint8_t fb = buffer[nextLineStart];
          if (fb == 0x20 || fb == 0x09) {
            nextHasIndent = true;
          }
        }
        if (!nextHasIndent && nextDisplayLen >= 3) {
          if (buffer[nextLineStart] == 0xE3 &&
              buffer[nextLineStart + 1] == 0x80 &&
              buffer[nextLineStart + 2] == 0x80) {
            nextHasIndent = true;
          }
        }
        if (nextHasIndent) break; // 下一行有缩进，视为新段落，停止合并

        // 追加到当前行（UTF-8 buffer; no per-line re-decode）
        std::string nextRaw(reinterpret_cast<char*>(buffer + nextLineStart), nextDisplayLen);
        line += nextRaw;
        displayLen += nextDisplayLen; // 更新合并后的总显示字节数
        lineEnd = nextLineEnd;        // 更新 lineEnd 指向最后合并行的 \n 位置
        mergeSegments.push_back({nextLineStart, nextDisplayLen, nextLineEnd});
      }
    }
    // ========== 段落内行合并结束 ==========

    // 每个新段落开始时，isOriginalLine 设为 true
    isOriginalLine = true;

    // 检测行首是否已有缩进（支持多种格式，仅对原生行检测）
    bool hasLeadingIndent = false;
    if (isOriginalLine) {
      // 检测普通空格（0x20）或 Tab（0x09）开头
      if (line.length() >= 1) {
        uint8_t firstByte = (uint8_t)line[0];
        if (firstByte == 0x20 || firstByte == 0x09) {
          hasLeadingIndent = true;
          // 原文有前导空格 → 新段落，需要缩进（用标准缩进替代原有空格）
          if (SETTINGS.firstlineintented) {
            needIndent = true;
          }
        }
      }
      // 检测全角空格开头（UTF-8: \xe3\x80\x80，3字节）
      if (!hasLeadingIndent && line.length() >= 3) {
        if ((uint8_t)line[0] == 0xE3 && (uint8_t)line[1] == 0x80 && (uint8_t)line[2] == 0x80) {
          hasLeadingIndent = true;
          if (SETTINGS.firstlineintented) {
            needIndent = true;
          }
        }
      }
    }

    // Track position within this source line (in bytes from pos)
    size_t lineBytePos = 0;

    // 首行缩进模式下，如果原文有前导空格，先去掉再用标准缩进替代
    if (SETTINGS.firstlineintented && hasLeadingIndent) {
      // 去掉行首的空格/Tab/全角空格
      size_t trimPos = 0;
      while (trimPos < line.length()) {
        uint8_t c = (uint8_t)line[trimPos];
        if (c == 0x20 || c == 0x09) {
          trimPos++;
        } else if (c == 0xE3 && trimPos + 2 < line.length() &&
                   (uint8_t)line[trimPos+1] == 0x80 && (uint8_t)line[trimPos+2] == 0x80) {
          trimPos += 3;
        } else {
          break;
        }
      }
      if (trimPos > 0) {
        lineBytePos = trimPos; // 记录 trim 掉的字节数，部分消费时需要加回
        line = line.substr(trimPos);
        hasLeadingIndent = false;
      }
    }

    // 保存原始行标记，用于第一次迭代的缩进判断
    bool isFirstIterationOfLine = true;

    // Word wrap if needed
    while (!line.empty() && static_cast<int>(outLines.size()) < linesPerPage) {
      // 第一次迭代使用原始行标记，后续迭代标记为拆行
      bool currentIsOriginal = isFirstIterationOfLine && isOriginalLine;
      isFirstIterationOfLine = false;

      // 精确计算行宽
      int lineWidth = renderer.getTextWidth(cachedFontId, line.c_str());
      // 标准模式：加上标点宽度补偿
      if (punctStandard) {
        lineWidth += calculatePunctWidthAdjustment(renderer, cachedFontId, line);
      }
      // 缩进判断：原生行 + 需要缩进 + 无已有空格
      const bool doIndent = currentIsOriginal && needIndent && !hasLeadingIndent;
      
      if (doIndent) {
        lineWidth += indentWidth; // 仅原生行预留缩进宽度
      }

      // 字距处理：LEFT_ALIGN 和 JUSTIFIED（降级为左对齐）模式下应用 wordSpacing。
      // 仅正值会增加行宽，负值不允许超出 viewport。
      const bool isLeftAligned = cachedParagraphAlignment == CrossPointSettings::LEFT_ALIGN ||
                                 cachedParagraphAlignment == CrossPointSettings::JUSTIFIED ||
                                 cachedParagraphAlignment == CrossPointSettings::BOOK_STYLE;
      const int effectiveSpacing = isLeftAligned ? std::max(0, (int)wordSpacing) : 0;
      if (isLeftAligned) {
        lineWidth = lineWidth + effectiveSpacing;
      }

      if (lineWidth <= viewportWidth) {
        // 不满一行（段落末尾）：不需要两端对齐
        if (doIndent) {
          outLines.push_back(indentStr + line);
          justifyOut.push_back(false); // 段落末尾不对齐
          needIndent = false;
        } else {
          outLines.push_back(line);
          justifyOut.push_back(false); // 段落末尾不对齐
        }
        lineBytePos += line.length();
        line.clear();
        break;
      }

      // Find break point（拆行逻辑）——逐字符累加宽度，避免 substr 堆分配
      size_t breakPos = 0;
      // 拆行宽度：第一个子行需要考虑缩进
      int allowedWidth = viewportWidth - effectiveSpacing;
      if (doIndent) {
        allowedWidth -= indentWidth;
      }

      // 逐字符累加精确宽度，找到不超过 allowedWidth 的最大位置
      {
        int accWidth = 0;
        size_t charStart = 0;
        while (charStart < line.length()) {
          uint8_t c = (uint8_t)line[charStart];
          size_t charLen = 1;
          if (c >= 0xF0) charLen = 4;
          else if (c >= 0xE0) charLen = 3;
          else if (c >= 0xC0) charLen = 2;
          if (charStart + charLen > line.length()) break;
          
          char tmp[5] = {0};
          memcpy(tmp, line.c_str() + charStart, charLen);
          int charW = renderer.getTextWidth(cachedFontId, tmp);
          
          // 标准模式：中文标点补偿到汉字宽度
          if (punctStandard && charLen >= 3) {
            uint32_t cp = 0;
            if (charLen == 3) {
              cp = ((c & 0x0F) << 12) | (((uint8_t)tmp[1] & 0x3F) << 6) | ((uint8_t)tmp[2] & 0x3F);
            } else if (charLen == 4) {
              cp = ((c & 0x07) << 18) | (((uint8_t)tmp[1] & 0x3F) << 12) | (((uint8_t)tmp[2] & 0x3F) << 6) | ((uint8_t)tmp[3] & 0x3F);
            }
            if (isChinesePunctCodepoint(cp) && charW < cjkCharWidth) {
              charW = cjkCharWidth;
            }
          }
          
          if (accWidth + charW > allowedWidth) break;
          accWidth += charW;
          charStart += charLen;
        }
        breakPos = charStart;
      }

      // 尝试在空格处断行（英文单词不截断）
      if (breakPos < line.length() && breakPos > 0) {
        size_t spacePos = line.rfind(' ', breakPos - 1);
        if (spacePos != std::string::npos && spacePos > 0) {
          breakPos = spacePos;
        }
      }

      if (breakPos == 0) {
        breakPos = 1;
      }

      // 拆行后添加到输出（满行，需要两端对齐）
      if (doIndent) {
        outLines.push_back(indentStr + line.substr(0, breakPos));
        justifyOut.push_back(false); // 首行有缩进，不做两端对齐
        needIndent = false;
      } else {
        outLines.push_back(line.substr(0, breakPos));
        justifyOut.push_back(true); // 满行需要两端对齐
      }

      // Skip space at break point
      size_t skipChars = breakPos;
      if (breakPos < line.length() && line[breakPos] == ' ') {
        skipChars++;
      }
      lineBytePos += skipChars;
      line = line.substr(skipChars);
    }

    // Determine how much of the UTF-8 buffer we consumed
    if (line.empty()) {
      // Fully consumed this source line, move past the newline
      pos = lineEnd + 1;
      // needIndent 不在这里设置，由下一行的前导空格检测或空行检测来决定
      isOriginalLine = true; // 重置原生行标记，下一行是新的原生行
    } else {
      // Partially consumed - page is full mid-line.
      // lineBytePos is UTF-8 bytes into the merged UTF-8 string; map back into
      // the decoded buffer (mergeSegments are UTF-8 indices after window decode).
      size_t bufferPos = pos;
      size_t remaining = lineBytePos;
      for (size_t si = 0; si < mergeSegments.size(); si++) {
        if (remaining < mergeSegments[si].dispLen) {
          bufferPos = mergeSegments[si].bufStart + remaining;
          break;
        }
        remaining -= mergeSegments[si].dispLen;
        if (remaining == 0) {
          bufferPos = mergeSegments[si].bufLineEnd + 1;
          break;
        }
      }

      pos = bufferPos;
      isOriginalLine = true;
      break;
    }
  }

  // Ensure we make progress even if calculations go wrong
  if (pos == 0 && !outLines.empty()) {
    // Fallback: at minimum, consume something to avoid infinite loop
    pos = 1;
  }

  if (mappedDecode) {
    // Exact raw offset of last complete code point with utf8End <= pos
    nextOffset = M4TxtEncoding::absoluteRawEndForUtf8Prefix(decodeWindowPos, cpMap.data, cpMap.len, pos,
                                                            mappedExactNext);
  } else {
    nextOffset = offset + pos;
  }

  // Make sure we don't go past the file / chapter end
  if (endOffset > 0 && nextOffset > endOffset) {
    nextOffset = endOffset;
  }

  if (needFree) free(buffer);

  logPerf("loadPage", millis() - tLoad0, currentPage,
          static_cast<uint32_t>(outLines.size()));
  return !outLines.empty();
}


void TxtReaderActivity::renderPage(bool skipDisplay, int xOffset, bool skipInvert) {
  const uint32_t tRender0 = millis();
  // Snapshot the page being laid out — this is what the EPD will show after
  // the physical drive, regardless of how far currentPage advances meanwhile.
  pendingPhysicalPage_ = currentPage;
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin_Top;
  orientedMarginLeft += SETTINGS.screenMargin_Left;
  orientedMarginRight += SETTINGS.screenMargin_Right;
  orientedMarginBottom += SETTINGS.screenMargin_Bottom; 

  float lineHeight = renderer.getLineHeight(cachedFontId)* SETTINGS.getReaderLineCompression();
  const int contentWidth = viewportWidth;
  // 标点宽度模式
  const bool punctStandard = (SETTINGS.chinesePunctWidth == CrossPointSettings::PUNCT_STANDARD);
  const int cjkCharWidth = renderer.getTextWidth(cachedFontId, "\xe6\x88\x91"); // "我"

  // Render text lines with alignment
  // Note on JUSTIFIED / BOOK_STYLE: True two-edge justification requires per-word
  // positioning with calculated inter-word gaps (as done in the Epub renderer's
  // ParsedText). The TXT reader works with pre-composed line strings, so JUSTIFIED
  // and BOOK_STYLE gracefully degrade to left-aligned rendering. The wordSpacing
  // setting is still honoured during the layout phase (loadPageAtOffset) which
  // affects line-break decisions for LEFT_ALIGN and JUSTIFIED modes.
  // 下划线绘制辅助函数（与 EPUB PageLine::drawDashedLine 一致）
  auto drawDashedLine = [&](int x1, int y, int x2, int dashLen, int gapLen) {
    int startX = std::min(x1, x2);
    int endX = std::max(x1, x2);
    int currentX = startX;
    while (currentX < endX) {
      int segEndX = std::min(currentX + dashLen, endX);
      renderer.drawLine(currentX, y, segEndX, y, true);
      currentX = segEndX + gapLen;
    }
  };

  auto renderLines = [&]() {
    // 下划线参数（与 EPUB 一致，从 SETTINGS 读取）
    const bool showExtraLine = SETTINGS.extraline != 0;
    const int underlineOffset = SETTINGS.underlineOffset;
    int dashLength = 10, gapLength = 10;
    if (showExtraLine) {
      switch (SETTINGS.underlineStyle) {
        case 0: dashLength = 9999; gapLength = 0; break;  // 实线
        case 1: dashLength = 5;    gapLength = 5; break;  // 短虚线
        case 2: dashLength = 10;   gapLength = 10; break; // 中虚线
        case 3: dashLength = 20;   gapLength = 10; break; // 长虚线
        case 4: dashLength = 2;    gapLength = 5; break;  // 点线
      }
    }
    const int effectiveLeft = orientedMarginLeft + xOffset;
    const int lineXStart = effectiveLeft;
    const int lineXEnd = effectiveLeft + viewportWidth;
    // Underline is relative to the baseline, not to the full line advance.
    // TTF line advance includes descender/leading, so using it here places
    // the underline inside the following line when line compression is tight.
    const int underlineBase = renderer.getFontAscenderSize(cachedFontId);

    int y = orientedMarginTop;
    for (size_t lineIdx = 0; lineIdx < currentPageLines.size(); lineIdx++) {
      const auto& line = currentPageLines[lineIdx];
      if (!line.empty()) {
        int x = effectiveLeft;

        // 判断是否需要两端对齐
        const bool shouldJustify = (cachedParagraphAlignment == CrossPointSettings::JUSTIFIED ||
                                    cachedParagraphAlignment == CrossPointSettings::BOOK_STYLE) &&
                                   lineIdx < currentPageJustify.size() && currentPageJustify[lineIdx];

        if (shouldJustify) {
          // 两端对齐：检测缩进，缩进部分固定宽度，正文部分均匀分配字间距
          
          // 检测行首缩进（全角空格 \xe3\x80\x80）
          int indentPixels = 0;
          size_t textStart = 0;
          while (textStart + 2 < line.size() &&
                 (uint8_t)line[textStart] == 0xE3 &&
                 (uint8_t)line[textStart+1] == 0x80 &&
                 (uint8_t)line[textStart+2] == 0x80) {
            indentPixels += renderer.getTextWidth(cachedFontId, "\xe3\x80\x80");
            textStart += 3;
          }
          
          // 绘制缩进部分（固定位置）
          if (indentPixels > 0) {
            std::string indentPart = line.substr(0, textStart);
            renderer.drawText(cachedFontId, effectiveLeft, y, indentPart.c_str());
          }
          
          // 正文部分拆分为字符列表
          std::string textPart = line.substr(textStart);
          int justifyWidth = contentWidth - indentPixels; // 正文可用宽度
          
          std::vector<std::string> chars;
          size_t i = 0;
          while (i < textPart.size()) {
            uint8_t c = (uint8_t)textPart[i];
            size_t charLen = 1;
            if (c >= 0xF0) charLen = 4;
            else if (c >= 0xE0) charLen = 3;
            else if (c >= 0xC0) charLen = 2;
            if (i + charLen > textPart.size()) break;
            chars.push_back(textPart.substr(i, charLen));
            i += charLen;
          }

          if (chars.size() <= 1) {
            renderer.drawText(cachedFontId, effectiveLeft + indentPixels, y, textPart.c_str());
          } else {
            // 计算总文字宽度（标准模式下标点补偿到汉字宽度）
            int totalTextWidth = 0;
            for (const auto& ch : chars) {
              int w = renderer.getTextWidth(cachedFontId, ch.c_str());
              if (punctStandard && ch.size() >= 3) {
                size_t pos = 0;
                uint32_t cp = 0;
                uint8_t fb = (uint8_t)ch[0];
                if (ch.size() == 3) {
                  cp = ((fb & 0x0F) << 12) | (((uint8_t)ch[1] & 0x3F) << 6) | ((uint8_t)ch[2] & 0x3F);
                }
                if (isChinesePunctCodepoint(cp) && w < cjkCharWidth) {
                  w = cjkCharWidth;
                }
              }
              totalTextWidth += w;
            }
            // 计算字间距（在正文可用宽度内均匀分配）
            int totalGap = justifyWidth - totalTextWidth;
            int gapCount = (int)chars.size() - 1;
            // 逐字绘制
            int drawX = effectiveLeft + indentPixels;
            for (size_t ci = 0; ci < chars.size(); ci++) {
              renderer.drawText(cachedFontId, drawX, y, chars[ci].c_str());
              int charW = renderer.getTextWidth(cachedFontId, chars[ci].c_str());
              // 标准模式：标点字符 advance 补偿到汉字宽度
              if (punctStandard && chars[ci].size() >= 3) {
                uint8_t fb = (uint8_t)chars[ci][0];
                uint32_t cp = 0;
                if (chars[ci].size() == 3) {
                  cp = ((fb & 0x0F) << 12) | (((uint8_t)chars[ci][1] & 0x3F) << 6) | ((uint8_t)chars[ci][2] & 0x3F);
                }
                if (isChinesePunctCodepoint(cp) && charW < cjkCharWidth) {
                  charW = cjkCharWidth;
                }
              }
              if (ci < chars.size() - 1 && gapCount > 0) {
                int gap = totalGap / gapCount;
                drawX += charW + gap;
                totalGap -= gap;
                gapCount--;
              } else {
                drawX += charW;
              }
            }
          }
        } else {
          // 非两端对齐：按原有逻辑
          // 检测缩进行是否超出屏幕，动态调整缩进宽度
          int textWidth = renderer.getTextWidth(cachedFontId, line.c_str());
          if (punctStandard) {
            textWidth += calculatePunctWidthAdjustment(renderer, cachedFontId, line);
          }
          
          if (textWidth > contentWidth && line.size() >= 6 &&
              (uint8_t)line[0] == 0xE3 && (uint8_t)line[1] == 0x80 && (uint8_t)line[2] == 0x80) {
            // 有缩进且超出屏幕：计算缩进部分和正文部分
            size_t indentEnd = 0;
            int indentW = 0;
            while (indentEnd + 2 < line.size() &&
                   (uint8_t)line[indentEnd] == 0xE3 &&
                   (uint8_t)line[indentEnd+1] == 0x80 &&
                   (uint8_t)line[indentEnd+2] == 0x80) {
              indentW += renderer.getTextWidth(cachedFontId, "\xe3\x80\x80");
              indentEnd += 3;
            }
            std::string textPart = line.substr(indentEnd);
            int textPartW = renderer.getTextWidth(cachedFontId, textPart.c_str());
            // 缩进宽度 = 屏幕宽度 - 正文宽度（至少为0）
            int adjustedIndent = contentWidth - textPartW;
            if (adjustedIndent < 0) adjustedIndent = 0;
            // 先绘制正文（缩进后的位置）
            renderer.drawText(cachedFontId, effectiveLeft + adjustedIndent, y, textPart.c_str());
          } else {
            switch (cachedParagraphAlignment) {
              case CrossPointSettings::CENTER_ALIGN:
                x = effectiveLeft + (contentWidth - textWidth) / 2;
                break;
              case CrossPointSettings::RIGHT_ALIGN:
                x = effectiveLeft + contentWidth - textWidth;
                break;
              default:
                break;
            }
            renderer.drawText(cachedFontId, x, y, line.c_str());
          }
        }

        // 绘制行下划线（与 EPUB PageLine::render 一致）
        if (showExtraLine) {
          const int nextLineY = y + std::max(0, static_cast<int>(std::ceil(lineHeight)) - 1);
          int lineY = std::min(y + underlineBase + underlineOffset, nextLineY);
          drawDashedLine(lineXStart, lineY, lineXEnd, dashLength, gapLength);
        }
      }
      y += lineHeight;
    }
  };

  // First pass: BW rendering
  renderLines();
  if (!skipDisplay) {
    renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginTop, orientedMarginLeft);
  }

  // Apply dark mode inversion (reading area only, before displaying)
  if (SETTINGS.epubDarkMode && !skipInvert) {
    renderer.invertScreen();
  }

  if (skipDisplay) {
    return; // 只渲染到 buffer，不显示
  }

  // Legacy plugin half-flush handoff (disabled when white-seed entry is used).
  const bool pluginClearHandoff = pluginSession_.active && pluginNeedsClearRefresh_;
  if (pluginClearHandoff) {
    pluginNeedsClearRefresh_ = false;
    pluginPendingHalfFlush_ = true;
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
    Serial.printf("[WR05] t=%lu first_frame_layout_ready gen=%u lines=%u page=%d/%d\n",
                  static_cast<unsigned long>(millis()), static_cast<unsigned>(pluginSession_.generation),
                  static_cast<unsigned>(currentPageLines.size()), currentPage, totalPages);
    return;
  }

  // Physical e-ink + AA run after unlock (displayTaskLoop) for plugin AND
  // library. Also the path for first content after white seed.
  if (deferPhysicalEpd_) {
    // Same body page must not drive the panel again. Progressive index used to
    // bump totalPages in the footer and re-run FAST differentials with
    // unchanged glyphs → residual densifies over time even without page-turns.
    if (firstPhysicalShown_ && currentPage == lastPhysicalBodyPage_) {
      Serial.printf("[WR05] t=%lu skip_physical same_page=%d total=%d complete=%d\n",
                    static_cast<unsigned long>(millis()), currentPage, totalPages, indexComplete_ ? 1 : 0);
      return;
    }
    libraryPhysicalPending_ = true;
    return;
  }
  // Plugin: same body page must not drive the panel again. Fallback for the
  // non-deferring path (displayTaskLoop now defers for both plugin and library).
  if (pluginSession_.active && firstPhysicalShown_ && currentPage == lastPhysicalBodyPage_) {
    Serial.printf("[WR05] t=%lu skip_physical same_page=%d total=%d complete=%d\n",
                  static_cast<unsigned long>(millis()), currentPage, totalPages, indexComplete_ ? 1 : 0);
    return;
  }

  logPerf("renderPage", millis() - tRender0, pendingPhysicalPage_,
          static_cast<uint32_t>(pageOffsets.size()) |
              (indexComplete_ ? 0x80000000u : 0u));
  finishPhysicalDisplay();
}

void TxtReaderActivity::armEntryWhiteSeed(EntryPlaceholderKind kind) {
  entryWhiteSeedPending_ = true;
  entryPlaceholderKind_ = kind;
  firstPhysicalShown_ = false;
  lastPhysicalBodyPage_ = -1;
  overlayReturnFlush_ = false;
}

void TxtReaderActivity::armOverlayReturnFlush() {
  entryWhiteSeedPending_ = false;
  entryPlaceholderKind_ = EntryPlaceholderKind::None;
  overlayReturnFlush_ = true;
  lastPhysicalBodyPage_ = -1;
  cachedPage = -1;
}

void TxtReaderActivity::finishPhysicalDisplay() {
  // Runs WITHOUT renderingMutex. Framebuffer already has BW layout from renderPage.
  // Page-turn animation: when enabled, play a wipe between the previous page
  // and the newly rendered frame instead of a direct displayBuffer.
  // Skip when rolling auto-turn is active (half-page blend path owns the EPD)
  // or when AA is on (gray planes would overwrite the wipe result).
  //
  // Entry sequence (displayTaskLoop): pure-white absolute HALF first → storeLastShown
  // white → this function drives page-1 (anim from white, or FAST on white RED).
  // Page-turn animation driven by settings (user tunes speed/params). The
  // system path is always the sliding-window partial wipe; its walk-off drain
  // is the settle, so there is no separate tail refresh.
  const bool wantAnim = SETTINGS.pageTurnAnimationEnabled != 0 && !rollingMode &&
                        !SETTINGS.textAntiAliasing;
  const bool firstContent = !firstPhysicalShown_;
  if (firstContent) {
    firstPhysicalShown_ = true;
    lastPhysicalBodyPage_ = (pendingPhysicalPage_ >= 0) ? pendingPhysicalPage_ : currentPage;
    Serial.printf("[WR05] t=%lu open_refresh phase=content mode=%s\n",
                  static_cast<unsigned long>(millis()), wantAnim ? "animation" : "fast");
    Serial.printf("[%lu] [PTA] first content frame (after white seed) anim=%d\n", millis(),
                  wantAnim ? 1 : 0);
  }

  Serial.printf("[%lu] [PTA] finishPhysicalDisplay anim=%d rolling=%d aa=%d first=%d\n", millis(),
                SETTINGS.pageTurnAnimationEnabled != 0 ? 1 : 0, rollingMode ? 1 : 0,
                SETTINGS.textAntiAliasing ? 1 : 0, firstContent ? 1 : 0);
  if (wantAnim) {
    const uint8_t* newFrame = renderer.getFrameBuffer();
    // Prefer persistent lastShown; fall back to BW chunk assembly (legacy).
    const uint8_t* prevShown = renderer.getLastShown();
    uint8_t* oldCopy = nullptr;
    bool haveOld = false;
    if (newFrame != nullptr && prevShown != nullptr) {
      oldCopy = static_cast<uint8_t*>(
          heap_caps_malloc(HalDisplay::BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (oldCopy) {
        std::memcpy(oldCopy, prevShown, HalDisplay::BUFFER_SIZE);
        haveOld = true;
      }
    }
    if (!haveOld && newFrame != nullptr) {
      constexpr size_t chunkBytes = HalDisplay::BUFFER_SIZE / 12;
      constexpr size_t numChunks = 12;
      oldCopy = static_cast<uint8_t*>(
          heap_caps_malloc(HalDisplay::BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (oldCopy) {
        haveOld = true;
        for (size_t c = 0; c < numChunks; ++c) {
          const uint8_t* src = renderer.bwBufferChunk(c);
          if (!src) {
            haveOld = false;
            break;
          }
          std::memcpy(oldCopy + c * chunkBytes, src, chunkBytes);
        }
      }
    }
    if (haveOld && oldCopy && newFrame) {
      const int logicalDir = static_cast<int>(SETTINGS.pageTurnAnimationDir);
      const int dir = renderer.logicalToPhysicalAnimationDirection(logicalDir);
      int steps = static_cast<int>(SETTINGS.pageTurnAnimationSteps);
      if (steps < 2) steps = 2;
      if (steps > 64) steps = 64;
      int mult = static_cast<int>(SETTINGS.pageTurnAnimationMult);
      if (mult < 1) mult = 1;
      if (mult > 16) mult = 16;
      if (mult > steps) mult = steps;
      uint8_t tp = SETTINGS.pageTurnAnimationTp;
      if (tp < 1) tp = 1;
      if (tp > 16) tp = 16;
      uint8_t fr = SETTINGS.pageTurnAnimationFrameRate;
      if (fr != 0x22 && fr != 0x44 && fr != 0x88) fr = 0x88;
      Serial.printf("[%lu] [PTA] anim start mode=window logical_dir=%d physical_dir=%d steps=%d mult=%d tp=%d fr=0x%02X\n",
                    millis(), logicalDir, dir, steps, mult, tp, fr);

      extern HalDisplay display;
      M4WaveformLab::setDisplay(&display);
      uint8_t lut[110] = {};
      lut[10] = 0x80;
      lut[20] = 0x40;
      lut[50] = tp;
      lut[51] = 0x01;
      for (int i = 100; i < 105; ++i) lut[i] = fr;
      lut[105] = 0x17;
      lut[106] = 0x41;
      lut[107] = 0xA8;
      lut[108] = 0x32;
      lut[109] = 0x30;
      bool played = false;
      uint32_t ms = 0;
      if (M4WaveformLab::setLutBytes(lut, sizeof(lut))) {
        ms = M4WaveformLab::runAnimateMemWindow(oldCopy, newFrame, steps, mult, dir);
        played = (ms != 0);
      }
      free(oldCopy);
      oldCopy = nullptr;
      if (played) {
        Serial.printf("[%lu] [PTA] anim done ms=%u\n", millis(), (unsigned)ms);
        logPerf("anim", (uint32_t)ms, pendingPhysicalPage_, 0);
        pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
        // Physical page = the frame we actually animated to (snapshot), never
        // the live target — currentPage may have advanced during the wipe.
        lastPhysicalBodyPage_ = (pendingPhysicalPage_ >= 0) ? pendingPhysicalPage_ : currentPage;
        (void)renderer.storeBwBuffer();
        (void)renderer.storeLastShown();
        // Decoupled catch-up: check immediately after the animation settles —
        // if taps accumulated a different target, render straight to it on the
        // next display-task tick (no waiting for a whole loop pass).
        if (quickMode_ && currentPage != lastPhysicalBodyPage_ && !physicalEpdBusy_.load()) {
          logPerf("anim_catchup_pending", 0, currentPage, (uint32_t)lastPhysicalBodyPage_);
        }
        return;
      }
      Serial.printf("[%lu] [PTA] anim failed — normal display\n", millis());
    } else {
      if (oldCopy) free(oldCopy);
      Serial.printf("[%lu] [PTA] no prev frame yet (first page)\n", millis());
    }
  }
  if (automaticPageTurnActive && rollingMode) {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    if (pagesUntilFullRefresh > 0) pagesUntilFullRefresh--;
  } else if (pagesUntilFullRefresh <= 1) {
    if (SETTINGS.textAntiAliasing) {
      Serial.printf("[%lu] [TRS] BW display: FAST (AA on, counter reset)\n", millis());
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      Serial.printf("[%lu] [TRS] BW display: HALF_REFRESH (counter=%d)\n", millis(), pagesUntilFullRefresh);
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    Serial.printf("[%lu] [TRS] BW display: FAST (counter=%d)\n", millis(), pagesUntilFullRefresh);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    pagesUntilFullRefresh--;
  }

  const bool bwBufferStored = renderer.storeBwBuffer();
  if (bwBufferStored && SETTINGS.textAntiAliasing) {
    // Gray passes re-draw glyphs into gray planes; skipDisplay avoids nested EPD.
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderPage(/*skipDisplay=*/true, /*xOffset=*/0, /*skipInvert=*/true);
    renderer.copyGrayscaleLsbBuffers();

    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderPage(/*skipDisplay=*/true, /*xOffset=*/0, /*skipInvert=*/true);
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }
  if (bwBufferStored) {
    renderer.restoreBwBuffer();
  }
  lastPhysicalBodyPage_ = (pendingPhysicalPage_ >= 0) ? pendingPhysicalPage_ : currentPage;
  (void)renderer.storeLastShown();
}



void TxtReaderActivity::renderDualPage() {
  int mt, mr, mb, ml;
  renderer.getOrientedViewableTRBL(&mt, &mr, &mb, &ml);
  ml += SETTINGS.screenMargin_Left;
  mr += SETTINGS.screenMargin_Right;
  mt += SETTINGS.screenMargin_Top;
  mb += SETTINGS.screenMargin_Bottom;

  // 状态栏空间（与 chapter_initializeReader 保持一致）
  if (SETTINGS.statusBar != CrossPointSettings::STATUS_BAR_MODE::NONE) {
    auto metrics = UITheme::getInstance().getMetrics();
    const bool showProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                                 SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::ONLY_BOOK_PROGRESS_BAR ||
                                 SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
    mb += statusBarMargin + (showProgressBar ? (metrics.bookProgressBarHeight + progressBarMarginTop) : 0);
  }

  // 内侧间距 = 装订线间隙 + 左页右边距 + 右页左边距
  const int innerGutter = DUAL_PAGE_GUTTER + SETTINGS.screenMargin_Right + SETTINGS.screenMargin_Left;
  const int rightXOffset = viewportWidth + innerGutter;

  // 预加载右页内容（justify 写入 scratch，不碰左页 on-screen flags）
  std::vector<std::string> rightLines;
  std::vector<bool> rightJustify;
  std::vector<int> rightIndent;
  bool hasRightPage = (dualRightPage >= 0 && dualRightPage < totalPages && dualRightPage != currentPage);
  if (hasRightPage) {
    size_t endOff = txt->getChapterendOffsetByIndex(chapternum);
    if (endOff > 0) endOff -= 1;
    size_t dummy;
    loadPageAtOffset(pageOffsets[dualRightPage], endOff, rightLines, dummy,
                     nullptr, 0, 0, 0, &rightJustify);
  }

  // 渲染两页文字的辅助 lambda（BW / 灰度模式复用）
  auto renderBothTexts = [&]() {
    renderPage(true, 0, true);
    if (hasRightPage) {
      auto savedLines = currentPageLines;
      auto savedJustify = currentPageJustify;
      auto savedIndent = currentPageIndentOffsets;
      currentPageLines = rightLines;
      currentPageJustify = rightJustify;
      currentPageIndentOffsets = rightIndent;
      renderPage(true, rightXOffset, true);
      currentPageLines = savedLines;
      currentPageJustify = savedJustify;
      currentPageIndentOffsets = savedIndent;
    }
  };

  // BW pass
  renderer.clearScreen();
  renderBothTexts();

  // 竖向分隔线：居中于左页右边距结束后的 DUAL_PAGE_GUTTER 中心
  const int sepX = ml + viewportWidth + SETTINGS.screenMargin_Right + DUAL_PAGE_GUTTER / 2;
  const int lineY1 = mt;
  const int lineY2 = renderer.getScreenHeight() - mb;
  renderer.drawLine(sepX - 3, lineY1, sepX - 3, lineY2, 1, true);   // left black
  renderer.drawLine(sepX - 2, lineY1, sepX - 2, lineY2, 8, false);  // 8px white gap
  renderer.drawLine(sepX + 6, lineY1, sepX + 6, lineY2, 1, true);   // right black

  renderStatusBar(mr, mb, mt, ml);

  if (SETTINGS.epubDarkMode) {
    renderer.invertScreen();
  }

  // Plugin openText handoff: same residual-clear policy as single-page path.
  const bool pluginClearHandoff = pluginSession_.active && pluginNeedsClearRefresh_;
  if (pluginClearHandoff) {
    pluginNeedsClearRefresh_ = false;
    pluginPendingHalfFlush_ = true;
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
    Serial.printf("[WR05] t=%lu first_frame_layout_ready_dual gen=%u lines=%u\n",
                  static_cast<unsigned long>(millis()), static_cast<unsigned>(pluginSession_.generation),
                  static_cast<unsigned>(currentPageLines.size()));
    return;
  }

  // 显示 BW
  if (automaticPageTurnActive && rollingMode) {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    if (pagesUntilFullRefresh > 0) pagesUntilFullRefresh--;
  } else if (pagesUntilFullRefresh <= 1) {
    if (SETTINGS.textAntiAliasing) {
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    pagesUntilFullRefresh--;
  }

  // AA 灰度渲染
  const bool bwBufferStored = renderer.storeBwBuffer();
  if (bwBufferStored && SETTINGS.textAntiAliasing) {
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderBothTexts();
    renderer.copyGrayscaleLsbBuffers();

    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderBothTexts();
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }

  renderer.restoreBwBuffer();
}




void TxtReaderActivity::renderScreen() {
  if (!txt) {
    return;
  }

  // Initialize reader if not done
  if (!chapter_initialized) {
    chapter_initializeReader(chapternum);
  }

  // Deferred bookmark jump: we already hold the state lock (displayTaskLoop).
  // Must use the already-locked site — goToPercent() would re-take and deadlock.
  if (m_pendingJumpPercent >= 0 && totalPages > 0) {
    goToPercentAlreadyLocked(m_pendingJumpPercent);
    m_pendingJumpPercent = -1;
  }

  if (pageOffsets.empty()) {
    renderer.clearScreen();
    // Prefer a recoverable message — corrupt progress / missing chapter batch.
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, 280, "无法打开章节", true, EpdFontFamily::BOLD);
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, 320, "请打开目录重选", true, EpdFontFamily::REGULAR);
    renderer.displayBuffer();
    return;
  }

  // Target page index not ready yet (progressive index still growing): push the
  // index until it COVERS the target page, then render immediately — never
  // render an out-of-range/empty page (which made a slow tap feel dead). This
  // runs on the display task only; input/keys stay responsive on the main task.
  if (currentPage >= static_cast<int>(pageOffsets.size()) && !indexComplete_) {
    const uint32_t tIdx = millis();
    int guard = 0;
    while (currentPage >= static_cast<int>(pageOffsets.size()) && !indexComplete_ && guard++ < 4) {
      const int added = continuePageIndex(1, 16 * 1024);
      if (added <= 0) break;  // no progress (file issue?) — do not spin forever
    }
    totalPages = static_cast<int>(pageOffsets.size());
    applyPendingRestoreIfReady();
    logPerf("index_to_target", millis() - tIdx, currentPage,
            static_cast<uint32_t>(pageOffsets.size()));
    if (currentPage >= static_cast<int>(pageOffsets.size())) {
      updateRequired = true;  // still not covered — retry next tick (bounded)
      return;
    }
    // Covered → fall through and render now.
  }


  if (currentPage < -1) currentPage = 0;
  // currentPage == -1 表示需要跳到最后一页（从下一章往前翻时）
  if (currentPage == -1) currentPage = totalPages > 0 ? totalPages - 1 : 0;
  // 仅当currentPage超过总页数时修正（避免无效页码）
  if (currentPage >= totalPages) currentPage = totalPages - 1;

  // 仅当页码变化时才重新从 SD 卡加载页面内容，避免重复 I/O 和文本解析
  if (currentPage != cachedPage) {
    size_t offset = pageOffsets[currentPage];
    size_t nextOffset;
    currentPageLines.clear();
    size_t endoffset;
    if (pluginSession_.active) {
      endoffset = txt->getFileSize();  // exclusive end for whole-file plugin chapter
    } else if (largeTxtFastOpen_ && chapternum == fastOpenChapter_ && activeChapterEnd_ > 0) {
      endoffset = activeChapterEnd_;
    } else {
      endoffset = txt->getChapterendOffsetByIndex(chapternum);
      if (endoffset > 0) endoffset -= 1;  // 与 buildPageIndex 保持一致
    }
    // If next page start is known, use it as exclusive end for tighter bounds.
    if (currentPage + 1 < (int)pageOffsets.size()) {
      endoffset = pageOffsets[static_cast<size_t>(currentPage + 1)];
    }
    loadPageAtOffset(offset, endoffset, currentPageLines, nextOffset);
    // Plugin progressive: first slice end can be too tight (or only whitespace).
    // Retry with whole-file exclusive end so page 1 is not blank.
    if (pluginSession_.active && currentPageLines.empty() && txt) {
      const size_t wholeEnd = txt->getFileSize();
      if (wholeEnd > offset && wholeEnd != endoffset) {
        nextOffset = offset;
        loadPageAtOffset(offset, wholeEnd, currentPageLines, nextOffset);
        endoffset = wholeEnd;
      }
    }
    cachedPage = currentPage;
    if (pluginSession_.active && pluginNeedsClearRefresh_) {
      Serial.printf("[WR05] t=%lu first_page_lines gen=%u n=%u off=%u end=%u\n",
                    static_cast<unsigned long>(millis()), static_cast<unsigned>(pluginSession_.generation),
                    static_cast<unsigned>(currentPageLines.size()), static_cast<unsigned>(offset),
                    static_cast<unsigned>(endoffset));
    }
  }

  // 卷帘半屏翻页：显示下一页上半部分 + 当前页下半部分
  if (automaticPageTurnActive && rollingMode && rollingHalfTurned) {
    if (currentPage + 1 < totalPages) {
      const std::string halfBufPath = txt->getCachePath() + "/halfbuf.bin";
      
      // 1. 加载下一页内容（justify 进 scratch，当前页 flags 不被覆盖）
      std::vector<std::string> nextPageLines;
      std::vector<bool> nextPageJustify;
      size_t nextOff = pageOffsets[currentPage + 1];
      size_t nextEnd = chapterContentEnd();
      if (nextEnd > 0 && !(largeTxtFastOpen_ && chapternum == fastOpenChapter_)) nextEnd -= 1;
      loadPageAtOffset(nextOff, nextEnd, nextPageLines, nextOff,
                       nullptr, 0, 0, 0, &nextPageJustify);
      
      // 2. 渲染下一页，保存上半部分到 SD
      auto savedLines = currentPageLines;
      auto savedJustify = currentPageJustify;
      currentPageLines = nextPageLines;
      currentPageJustify = nextPageJustify;
      renderer.clearScreen();
      renderPage(true); // 只渲染到 buffer，不显示
      renderer.saveTopHalfToSd(halfBufPath.c_str());
      
      // 3. 渲染当前页（含状态栏，状态栏在下半部分会保留）
      currentPageLines = std::move(savedLines);
      currentPageJustify = std::move(savedJustify);
      renderer.clearScreen();
      renderPage(true); // 只渲染文字到 buffer
      {
        // 渲染状态栏（在下半部分）
        int mt, mr, mb, ml;
        renderer.getOrientedViewableTRBL(&mt, &mr, &mb, &ml);
        mt += SETTINGS.screenMargin_Top;
        ml += SETTINGS.screenMargin_Left;
        mr += SETTINGS.screenMargin_Right;
        mb += SETTINGS.screenMargin_Bottom;
        renderStatusBar(mr, mb, mt, ml);
      }
      
      // 4. 粘贴下一页上半部分
      renderer.restoreTopHalfFromSd(halfBufPath.c_str());
      
      // 5. 画分隔线
      int orientedMarginLeft2, orientedMarginRight2, dummy;
      renderer.getOrientedViewableTRBL(&dummy, &orientedMarginRight2, &dummy, &orientedMarginLeft2);
      orientedMarginLeft2 += SETTINGS.screenMargin_Left;
      orientedMarginRight2 += SETTINGS.screenMargin_Right;
      const int midY = renderer.getScreenHeight() / 2;
      const int lineX1 = orientedMarginLeft2;
      const int lineX2 = renderer.getScreenWidth() - orientedMarginRight2;
      renderer.drawLine(lineX1, midY - 3, lineX2, midY - 3, 1, true);
      renderer.drawLine(lineX1, midY - 2, lineX2, midY - 2, 8, false);
      renderer.drawLine(lineX1, midY + 6, lineX2, midY + 6, 1, true);
      
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      if (pagesUntilFullRefresh > 0) pagesUntilFullRefresh--;
      progressSavePending_ = true;
      return;
    }
  }

  if (isLandscapeDualPage()) {
    // 初始化双页状态（章节切换或首次进入时）
    if (dualRightPage < 0 || dualRightPage >= totalPages) {
      dualRightPage = (currentPage + 1 < totalPages) ? currentPage + 1 : currentPage;
      dualNextLeft = true;
    }
    renderDualPage();
  } else {
    renderer.clearScreen();
    renderPage();
  }

  // Persist after the first physical frame / next idle slice. Writing the
  // progress file here held the render lock on first open and delayed the
  // readable page by an SD sync.
  progressSavePending_ = true;
}



void TxtReaderActivity::renderStatusBar(const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginTop, const int orientedMarginLeft) const {
  auto metrics = UITheme::getInstance().getMetrics();

  // determine visible status bar elements (same rules as Epub)
  const bool showProgressPercentage = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL;
  const bool showProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                               SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::ONLY_BOOK_PROGRESS_BAR;
  const bool showChapterProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showProgressText = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL ||
                                SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR;
  const bool showBookPercentage = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showBattery = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::NO_PROGRESS ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showTitle = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::NO_PROGRESS ||
                         SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL ||
                         SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                         SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage == CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_NEVER;

  // 使用屏幕绝对坐标定位状态栏（与 EPUB 一致，不依赖 orientedMarginBottom）
  const auto screenHeight = renderer.getScreenHeight();
  const int textLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const bool hasProgressBar = showProgressBar || showChapterProgressBar;
  // 进度条顶部Y坐标：屏幕底部向上 progressBarBottomGap
  const int progressBarTopY = screenHeight - progressBarBottomGap - metrics.bookProgressBarHeight;
  // 文字顶部Y坐标：
  //   - 有进度条：进度条上方 progressBarTextGap 再向上一个行高
  //   - 无进度条：屏幕底部上方 progressBarBottomGap 再向上一个行高
  const int textY = hasProgressBar
      ? (progressBarTopY - progressBarTextGap - textLineHeight)
      : (screenHeight - progressBarBottomGap - textLineHeight);
  int progressTextWidth = 0;

  // 全书进度：当前页字节偏移 / 文件总大小
  float progress = 0;
  const size_t fileSize = txt->getFileSize();
  if (fileSize > 0 && currentPage >= 0 && currentPage < (int)pageOffsets.size()) {
    progress = static_cast<float>(pageOffsets[currentPage]) * 100.0f / static_cast<float>(fileSize);
    if (progress > 100.0f) progress = 100.0f;
  }

  if (showProgressText || showProgressPercentage || showBookPercentage) {
    char progressStr[40];
    if (!indexComplete_) {
      // Provisional total while progressive index runs (plugin + library).
      if (showProgressPercentage) {
        snprintf(progressStr, sizeof(progressStr), "%d/?  %.0f%%", currentPage + 1, progress);
      } else {
        snprintf(progressStr, sizeof(progressStr), "%d/? · %d", currentPage + 1, totalPages);
      }
    } else if (showProgressPercentage) {
      snprintf(progressStr, sizeof(progressStr), "%d/%d  %.0f%%", currentPage + 1, totalPages, progress);
    } else if (showBookPercentage) {
      snprintf(progressStr, sizeof(progressStr), "%.0f%%", progress);
    } else {
      snprintf(progressStr, sizeof(progressStr), "%d/%d", currentPage + 1, totalPages);
    }

    progressTextWidth = renderer.getTextWidth(SMALL_FONT_ID, progressStr);
    renderer.drawText(SMALL_FONT_ID, renderer.getScreenWidth() - orientedMarginRight - progressTextWidth, textY,
                      progressStr);
  }

  // Non-blocking provider overlay (next-chapter fetch) — footer strip, not full screen.
  if (!providerOverlayMsg_.empty()) {
    const int ox = orientedMarginLeft;
    const int oy = textY - textLineHeight - 4;
    M4UiText::draw(renderer, SMALL_FONT_ID, ox, oy, providerOverlayMsg_.c_str());
  }

  if (showProgressBar) {
    if (SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::ONLY_BOOK_PROGRESS_BAR) {
      // "仅进度条" mode: keep bar at hardware bottom (current style)
      GUI.drawReadingProgressBar(renderer, static_cast<size_t>(progress));
    } else {
      // "完整+进度条" mode：进度条紧贴屏幕底部（与 EPUB 一致）
      const int barMaxWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
      const int barWidth = barMaxWidth * static_cast<int>(progress) / 100;
      renderer.fillRect(orientedMarginLeft, progressBarTopY, barWidth, metrics.bookProgressBarHeight, true);
    }
  }

  if (showChapterProgressBar) {
    // "完整+章节条" mode：进度条紧贴屏幕底部（与 EPUB 一致）
    const int barMaxWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
    const int barWidth = barMaxWidth * static_cast<int>(progress) / 100;
    renderer.fillRect(orientedMarginLeft, progressBarTopY, barWidth, metrics.bookProgressBarHeight, true);
  }

  if (showBattery) {
    GUI.drawBattery(renderer, Rect{orientedMarginLeft + 1, textY, metrics.batteryWidth, metrics.batteryHeight},
                    showBatteryPercentage);
  }

  // ── Clock display (after battery, before title) ──
  int clockTotalWidth = 0;
  if (showBattery) {
    const std::string timeStr = getClockTimeString();
    const int timeTextWidth = renderer.getTextWidth(SMALL_FONT_ID, timeStr.c_str());

    int batteryRightX = orientedMarginLeft + 1 + metrics.batteryWidth;
    if (showBatteryPercentage) {
      batteryRightX += 4 + renderer.getTextWidth(SMALL_FONT_ID, "100%");
    }

    const int fontHeight = renderer.getLineHeight(SMALL_FONT_ID);
    const int clockCX = batteryRightX + clockBatterySpacing + clockIconRadius;
    const int clockCY = textY + fontHeight / 2;
    drawClockIcon(renderer, clockCX, clockCY, clockIconRadius);

    const int timeTextX = clockCX + clockIconRadius + clockIconTextSpacing;
    renderer.drawText(SMALL_FONT_ID, timeTextX, textY, timeStr.c_str());

    clockTotalWidth = clockBatterySpacing + clockIconSize + clockIconTextSpacing + timeTextWidth;
  }

  if (showTitle) {
    const int rendererableScreenWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
    const int batterySize = showBattery ? (showBatteryPercentage ? 50 : 20) : 0;
    const int titleMarginLeft = batterySize + clockTotalWidth + 30;
    const int titleMarginRight = progressTextWidth + 30;

    int titleMarginLeftAdjusted = std::max(titleMarginLeft, titleMarginRight);
    int availableTitleSpace = rendererableScreenWidth - 2 * titleMarginLeftAdjusted;

    std::string title;
    int titleWidth;
    // Chapter/book title in status bar: reader face scaled to SMALL metrics
    // so CJK titles are not "?" while geometry stays status-bar sized.
    if (automaticPageTurnActive) {
      if (rollingMode) {
        title = L(Str::kHalfScreenPaging);
      } else {
        title = L(Str::kAutoPaging);
      }
      titleWidth = M4UiText::textWidth(renderer, SMALL_FONT_ID, title.c_str());
      if (titleWidth > availableTitleSpace) {
        title = M4UiText::truncated(renderer, SMALL_FONT_ID, title.c_str(), availableTitleSpace);
        titleWidth = M4UiText::textWidth(renderer, SMALL_FONT_ID, title.c_str());
      }
    } else if (pluginSession_.active) {
      title = displayTitle();
      titleWidth = M4UiText::textWidth(renderer, SMALL_FONT_ID, title.c_str());
      if (titleWidth > availableTitleSpace) {
        availableTitleSpace = rendererableScreenWidth - titleMarginLeft - titleMarginRight;
        titleMarginLeftAdjusted = titleMarginLeft;
      }
      if (titleWidth > availableTitleSpace) {
        title = M4UiText::truncated(renderer, SMALL_FONT_ID, title.c_str(), availableTitleSpace);
        titleWidth = M4UiText::textWidth(renderer, SMALL_FONT_ID, title.c_str());
      }
    } else {
      title = txt->getChapterTitleByIndex(chapternum);
      if (title.empty() && largeTxtFastOpen_ && chapternum == fastOpenChapter_) {
        title = txt->getTitle();
        if (title.empty()) title = "TXT";
      }
      titleWidth = M4UiText::textWidth(renderer, SMALL_FONT_ID, title.c_str());
      if (titleWidth > availableTitleSpace) {
        availableTitleSpace = rendererableScreenWidth - titleMarginLeft - titleMarginRight;
        titleMarginLeftAdjusted = titleMarginLeft;
      }
      if (titleWidth > availableTitleSpace) {
        title = M4UiText::truncated(renderer, SMALL_FONT_ID, title.c_str(), availableTitleSpace);
        titleWidth = M4UiText::textWidth(renderer, SMALL_FONT_ID, title.c_str());
      }
    }

    M4UiText::draw(renderer, SMALL_FONT_ID,
                   titleMarginLeftAdjusted + orientedMarginLeft + (availableTitleSpace - titleWidth) / 2, textY,
                   title.c_str());
  }
}

void TxtReaderActivity::saveProgress() const {
  // Plugin sessions keep Lua progress.json authoritative for resume, but the
  // per-chapter progress.dat also feeds Home reading-history percentages
  // (loadBookProgress on the last chapter cache). Write it for plugin
  // chapters too, so Home 阅读历史 updates as pages turn / chapters switch.
  if (!txt) return;

  txt->setupCacheDir();

  const std::string dir = txt->getCachePath();
  // Use progress.dat — serial showed progress.bin can be written as tmp but
  // never replaced (stuck node / bad rename target). New name avoids that trap.
  const std::string path = dir + "/progress.dat";
  const std::string legacy = dir + "/progress.bin";
  const std::string tmp = dir + "/progress.tmp";

  uint8_t data[kProgressDataBytes] = {0};
  int page = currentPage;
  const int chRaw = M4ContentProvider::resolveProgressChapterIndex(
      pluginSession_.active, pluginSession_.chapterIndex, chapternum);
  int ch = chRaw;
  if (page < 0) page = 0;
  if (page > 65535) page = 65535;
  if (ch < 0) ch = 0;
  if (ch > 65535) ch = 65535;
  data[0] = static_cast<uint8_t>(page & 0xFF);
  data[1] = static_cast<uint8_t>((page >> 8) & 0xFF);
  data[4] = static_cast<uint8_t>(ch & 0xFF);
  data[5] = static_cast<uint8_t>((ch >> 8) & 0xFF);

  auto progressOffset = [](size_t offset) -> uint32_t {
    return offset > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(offset);
  };
  size_t resumeByte = 0;
  if (currentPage >= 0 && currentPage < static_cast<int>(pageOffsets.size())) {
    resumeByte = pageOffsets[static_cast<size_t>(currentPage)];
  } else if (hasPendingRestore_) {
    resumeByte = pendingRestoreByte_;
  }
  const uint32_t resumeByte32 = progressOffset(resumeByte);
  const uint32_t rangeBegin32 = progressOffset(activeChapterBegin_);
  const uint32_t rangeEnd32 = progressOffset(activeChapterEnd_ > 0 ? activeChapterEnd_ : txt->getFileSize());
  std::memcpy(data + 8, &resumeByte32, sizeof(resumeByte32));
  std::memcpy(data + 12, &rangeBegin32, sizeof(rangeBegin32));
  std::memcpy(data + 16, &rangeEnd32, sizeof(rangeEnd32));

  auto forceRemove = [](const char* p) {
    if (!SdMan.exists(p)) return;
    if (SdMan.remove(p)) return;
    // Stuck directory or busy node — try rmdir then remove again.
    (void)SdMan.rmdir(p);
    (void)SdMan.removeDir(p);
    (void)SdMan.remove(p);
  };

  auto tryWrite = [&](const char* p) -> bool {
    forceRemove(p);
    FsFile f;
    if (!SdMan.openFileForWrite("TRS", p, f)) return false;
    const size_t n = f.write(data, sizeof(data));
    f.sync();
    f.close();
    return n == sizeof(data);
  };

  forceRemove(tmp.c_str());
  if (!tryWrite(tmp.c_str())) {
    Serial.printf("[%lu] [TRS] progress tmp WRITE FAIL ch=%d page=%d dir=%s\n", millis(), ch, page,
                  dir.c_str());
    // Direct to progress.dat
    if (!tryWrite(path.c_str())) {
      Serial.printf("[%lu] [TRS] progress.dat WRITE FAIL ch=%d page=%d\n", millis(), ch, page);
      return;
    }
  } else {
    forceRemove(path.c_str());
    if (!SdMan.rename(tmp.c_str(), path.c_str())) {
      forceRemove(path.c_str());
      if (!tryWrite(path.c_str())) {
        Serial.printf("[%lu] [TRS] progress.dat RENAME/WRITE FAIL ch=%d page=%d\n", millis(), ch, page);
        return;
      }
    }
  }
  // Drop legacy stuck progress.bin so load is not confused.
  forceRemove(legacy.c_str());
  Serial.printf("[%lu] [TRS] saved progress: page %d/%d chapter %d → %s\n", millis(), page, totalPages, ch,
                path.c_str());
}

void TxtReaderActivity::loadProgress() {
  chapter_initialized = false;
  currentPage = 0;
  chapternum = 0;
  hasPendingRestore_ = false;
  pendingRestoreByte_ = 0;
  resumeRangeBegin_ = 0;
  resumeRangeEnd_ = 0;

  if (!txt) return;
  txt->setupCacheDir();

  const std::string dir = txt->getCachePath();
  const std::string pathDat = dir + "/progress.dat";
  const std::string pathBin = dir + "/progress.bin";
  const std::string pathTmp = dir + "/progress.tmp";

  auto readProgress = [](const char* p, uint8_t* data, size_t& bytesRead) -> bool {
    bytesRead = 0;
    FsFile f;
    if (!SdMan.openFileForRead("TRS", p, f)) return false;
    const size_t available = static_cast<size_t>(f.fileSize());
    if (available < kLegacyProgressDataBytes) {
      f.close();
      return false;
    }
    const size_t wanted = std::min(available, kProgressDataBytes);
    bytesRead = f.read(data, wanted);
    f.close();
    return bytesRead >= kLegacyProgressDataBytes;
  };

  uint8_t data[kProgressDataBytes] = {0};
  size_t bytesRead = 0;
  const char* used = nullptr;
  if (readProgress(pathDat.c_str(), data, bytesRead)) {
    used = "progress.dat";
  } else if (readProgress(pathTmp.c_str(), data, bytesRead)) {
    used = "progress.tmp";  // interrupted rename recovery
  } else if (readProgress(pathBin.c_str(), data, bytesRead)) {
    used = "progress.bin";  // legacy
  } else {
    Serial.printf("[%lu] [TRS] no progress file yet in %s\n", millis(), dir.c_str());
    return;
  }

  int page = data[0] + (data[1] << 8);
  int ch = data[4] + (data[5] << 8);
  constexpr int kMaxChapter = 5000;
  constexpr int kMaxPage = 10000;
  bool repaired = false;
  if (ch < 0 || ch > kMaxChapter) {
    Serial.printf("[%lu] [TRS] progress chapter %d invalid → 0 (from %s)\n", millis(), ch, used);
    ch = 0;
    page = 0;
    repaired = true;
  }
  if (page < 0 || page > kMaxPage) {
    Serial.printf("[%lu] [TRS] progress page %d invalid → 0 (from %s)\n", millis(), page, used);
    page = 0;
    repaired = true;
  }
  currentPage = page;
  chapternum = ch;
  if (largeTxtFastOpen_ && bytesRead >= kProgressDataBytes) {
    uint32_t resumeByte = 0;
    uint32_t rangeBegin = 0;
    uint32_t rangeEnd = 0;
    std::memcpy(&resumeByte, data + 8, sizeof(resumeByte));
    std::memcpy(&rangeBegin, data + 12, sizeof(rangeBegin));
    std::memcpy(&rangeEnd, data + 16, sizeof(rangeEnd));
    const size_t fileSize = txt->getFileSize();
    if (fileSize > 0 && resumeByte < fileSize) {
      pendingRestoreByte_ = resumeByte;
      hasPendingRestore_ = true;
      resumeRangeBegin_ = std::min<size_t>(rangeBegin, fileSize);
      resumeRangeEnd_ = rangeEnd > 0 ? std::min<size_t>(rangeEnd, fileSize) : fileSize;
      if (resumeRangeEnd_ <= resumeRangeBegin_) resumeRangeEnd_ = fileSize;
      Serial.printf("[%lu] [WR05] large_txt_resume byte=%u range=%u..%u\n", millis(), resumeByte,
                    static_cast<unsigned>(resumeRangeBegin_), static_cast<unsigned>(resumeRangeEnd_));
    }
  }
  Serial.printf("[%lu] [TRS] Loaded progress: page %d chapter %d bytes=%zu (from %s)\n", millis(), currentPage,
                chapternum, bytesRead, used);
  // Migrate legacy/tmp → progress.dat; rewrite if we clamped garbage.
  // Do not overwrite an 8-byte legacy record with a zero-offset extended
  // record before the first fast page has been laid out.
  if ((repaired || std::strcmp(used, "progress.dat") != 0) &&
      (!largeTxtFastOpen_ || bytesRead >= kProgressDataBytes)) {
    saveProgress();
  }
}




void TxtReaderActivity::libraryPrefetchReset() {
  prefetchChapter_ = -1;
  prefetchOffsets_.clear();
  prefetchCursor_ = 0;
  prefetchRangeEnd_ = 0;
  prefetchComplete_ = false;
  prefetchSkipped_ = false;
}

void TxtReaderActivity::libraryIdleDiscoverChapterBatch() {
  // REQUIRES: state lock held. One batch per idle pass keeps the first page
  // independent from chapter parsing and lets the picker consume cache files
  // progressively instead of waiting for a full TOC copy.
  if (pluginSession_.active || !largeTxtFastOpen_ || !txt || chapterDiscoveryDone_) return;
  const int batch = chapterDiscoveryBatch_;
  if (batch < 0 || batch > 50000) {
    chapterDiscoveryDone_ = true;
    return;
  }

  const uint32_t t0 = millis();
  const bool fromCache = txt->hasChapterBatchCache(batch);
  const bool loaded = txt->parseChapterIndexAndOffset(batch, /*allowScan=*/!fromCache);
  bool any = false;
  for (int i = 0; i < 25; ++i) {
    if (txt->isChapterExist(batch + i)) {
      any = true;
      break;
    }
  }
  if (!loaded || !any) {
    chapterDiscoveryDone_ = true;
    Serial.printf("[%lu] [WR05] large_txt_chapter_batch_done batch=%d any=%d ms=%lu\n", millis(), batch,
                  any ? 1 : 0, static_cast<unsigned long>(millis() - t0));
    return;
  }

  chapterDiscoveryBatch_ = batch + 25;
  Serial.printf("[%lu] [WR05] large_txt_chapter_batch batch=%d cache=%d next=%d ms=%lu\n", millis(), batch,
                fromCache ? 1 : 0, chapterDiscoveryBatch_, static_cast<unsigned long>(millis() - t0));
}

bool TxtReaderActivity::chapter_pageIndexCacheExists(int ch) const {
  if (!txt || ch < 0) return false;
  std::string cachePath = txt->getCachePath() + "/chapter" + std::to_string(ch) + ".bin";
  return SdMan.exists(cachePath.c_str());
}

void TxtReaderActivity::chapter_savePageIndexCacheOffsets(int ch,
                                                          const std::vector<size_t>& offsets) const {
  if (!txt || ch < 0 || offsets.empty()) return;
  std::string cachePath = txt->getCachePath() + "/chapter" + std::to_string(ch) + ".bin";
  FsFile f;
  if (!SdMan.openFileForWrite("TRS", cachePath, f)) {
    Serial.printf("[%lu] [TRS] prefetch: failed to save chapter %d cache\n", millis(), ch);
    return;
  }
  serialization::writePod(f, CACHE_MAGIC);
  serialization::writePod(f, CACHE_VERSION);
  serialization::writePod(f, static_cast<uint8_t>(txt->getEncodingType()));
  serialization::writePod(f, static_cast<uint32_t>(txt->getFileSize()));
  serialization::writePod(f, static_cast<int32_t>(viewportWidth));
  serialization::writePod(f, static_cast<int32_t>(linesPerPage));
  serialization::writePod(f, static_cast<int32_t>(cachedFontId));
  serialization::writePod(f, wordSpacing);
  serialization::writePod(f, SETTINGS.customLineSpacing);
  serialization::writePod(f, SETTINGS.firstlineintented);
  serialization::writePod(f, static_cast<int32_t>(cachedScreenMargin));
  serialization::writePod(f, cachedParagraphAlignment);
  serialization::writePod(f, SETTINGS.chinesePunctWidth);
  serialization::writePod(f, static_cast<uint32_t>(offsets.size()));
  for (size_t offset : offsets) {
    serialization::writePod(f, static_cast<uint32_t>(offset));
  }
  f.close();
  Serial.printf("[%lu] [TRS] prefetch: saved chapter %d cache pages=%zu\n", millis(), ch,
                offsets.size());
}

void TxtReaderActivity::libraryIdlePrefetchNextChapter() {
  // REQUIRES: state lock held. Library only. Current chapter fully indexed,
  // panel not mid-anim. Builds chapter N+1 pageOffsets into prefetchOffsets_
  // and writes chapter{N+1}.bin so the next cross-chapter open is a cache hit
  // (same idea as providerIdlePrefetchNext for network chapters).
  if (pluginSession_.active || !txt || !indexComplete_ || prefetchSkipped_) return;
  if (viewportWidth <= 0 || linesPerPage <= 0) return;

  const int next = chapternum + 1;
  if (next < 0) return;

  // Already have a finished cache for next chapter — nothing to do.
  if (chapter_pageIndexCacheExists(next)) {
    if (prefetchChapter_ == next) libraryPrefetchReset();
    prefetchSkipped_ = true;  // until chapter changes (reset on switch)
    return;
  }

  // Start or resume a progressive build for `next`.
  if (prefetchChapter_ != next || prefetchOffsets_.empty()) {
    auto batchStart = [](int ch) {
      if (ch < 0) ch = 0;
      return (ch / 25) * 25;
    };
    const int curBatch = batchStart(chapternum);
    const int nextBatch = batchStart(next);

    size_t begin = 0, end = 0;
    bool have = false;
    if (nextBatch == curBatch) {
      if (!txt->isChapterExist(next)) {
        if (txt->hasChapterBatchCache(nextBatch)) {
          txt->parseChapterIndexAndOffset(nextBatch, /*allowScan=*/false);
        }
      }
      have = txt->isChapterExist(next);
      if (have) {
        begin = txt->getChapterOffsetByIndex(next);
        end = txt->getChapterendOffsetByIndex(next);
      }
    } else {
      if (!txt->hasChapterBatchCache(nextBatch)) {
        Serial.printf("[%lu] [TRS] prefetch: next ch=%d batch %d uncached — skip\n", millis(), next,
                      nextBatch);
        prefetchSkipped_ = true;
        return;
      }
      txt->parseChapterIndexAndOffset(nextBatch, /*allowScan=*/false);
      have = txt->isChapterExist(next);
      if (have) {
        begin = txt->getChapterOffsetByIndex(next);
        end = txt->getChapterendOffsetByIndex(next);
      }
      // Restore current batch so live chapter end offsets stay valid.
      if (txt->hasChapterBatchCache(curBatch)) {
        txt->parseChapterIndexAndOffset(curBatch, /*allowScan=*/false);
      } else {
        txt->parseChapterIndexAndOffset(curBatch, /*allowScan=*/true);
      }
    }
    if (!have) {
      Serial.printf("[%lu] [TRS] prefetch: next ch=%d missing — skip\n", millis(), next);
      prefetchSkipped_ = true;
      return;
    }
    const size_t fileSize = txt->getFileSize();
    if (end == 0 || end <= begin) end = fileSize;
    constexpr size_t kMaxChapterBytes = 2 * 1024 * 1024;
    if (end > begin && end - begin > kMaxChapterBytes) end = begin + kMaxChapterBytes;
    if (begin >= fileSize || end <= begin) {
      prefetchSkipped_ = true;
      return;
    }
    prefetchChapter_ = next;
    prefetchOffsets_.clear();
    prefetchOffsets_.push_back(begin);
    prefetchCursor_ = begin;
    prefetchRangeEnd_ = end;
    prefetchComplete_ = false;
    Serial.printf("[%lu] [TRS] prefetch: start ch=%d range=%zu..%zu\n", millis(), next, begin, end);
  }

  if (prefetchComplete_ || prefetchOffsets_.empty()) return;

  // Progress a few pages (same budget shape as background current-chapter index).
  std::vector<std::string> tempLines;
  std::vector<bool> justifyScratch;
  int added = 0;
  size_t bytes = 0;
  constexpr int kMaxPages = 4;
  constexpr size_t kMaxBytes = 64 * 1024;
  while (added < kMaxPages && bytes < kMaxBytes && !prefetchComplete_) {
    if (prefetchCursor_ >= prefetchRangeEnd_) {
      prefetchComplete_ = true;
      break;
    }
    size_t nextOffset = prefetchCursor_;
    const size_t before = prefetchCursor_;
    if (!loadPageAtOffset(prefetchCursor_, prefetchRangeEnd_, tempLines, nextOffset, nullptr, 0, 0, 0,
                          &justifyScratch)) {
      prefetchComplete_ = true;
      break;
    }
    if (nextOffset <= prefetchCursor_) nextOffset = prefetchCursor_ + 1;
    bytes += (nextOffset - before);
    if (nextOffset >= prefetchRangeEnd_) {
      prefetchCursor_ = prefetchRangeEnd_;
      prefetchComplete_ = true;
      break;
    }
    prefetchOffsets_.push_back(nextOffset);
    prefetchCursor_ = nextOffset;
    ++added;
  }

  if (prefetchComplete_ && !prefetchOffsets_.empty()) {
    chapter_savePageIndexCacheOffsets(prefetchChapter_, prefetchOffsets_);
    Serial.printf("[%lu] [TRS] prefetch: complete ch=%d pages=%zu\n", millis(), prefetchChapter_,
                  prefetchOffsets_.size());
    prefetchSkipped_ = true;
    prefetchOffsets_.clear();  // free RAM; cache is on SD
  } else if (added > 0) {
    Serial.printf("[%lu] [TRS] prefetch: progress ch=%d pages=%zu cursor=%zu\n", millis(),
                  prefetchChapter_, prefetchOffsets_.size(), prefetchCursor_);
  }
}

void TxtReaderActivity::logPageLoadFail(const char* why, size_t offset, size_t bytes) const {
  if (!why) return;
  std::string path = "apps_data/com.jjwxc.client/logs/reader_pageload.log";
  FsFile f = SdMan.open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND);
  if (!f) return;
  char line[160];
  const int n = snprintf(line, sizeof(line), "[%lu] fail why=%s ch=%d page=%d off=%zu bytes=%zu\n",
                         (unsigned long)millis(), why, chapternum, currentPage, offset, bytes);
  if (n > 0) f.write(reinterpret_cast<const uint8_t*>(line), (size_t)n);
  f.close();
}

void TxtReaderActivity::logPerf(const char* step, uint32_t ms, int page, uint32_t extra) const {
  if (!step) return;
  std::string path = "apps_data/com.jjwxc.client/logs/reader_perf.log";
  FsFile f = SdMan.open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND);
  if (!f) return;
  char line[160];
  const int n = snprintf(line, sizeof(line), "[%lu] perf step=%s ch=%d page=%d ms=%u extra=%u\n",
                         (unsigned long)millis(), step, chapternum, page, ms, extra);
  if (n > 0) f.write(reinterpret_cast<const uint8_t*>(line), (size_t)n);
  f.close();
}

int TxtReaderActivity::statusBarLogicalTopY() const {
  if (SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::NONE) {
    return renderer.getScreenHeight();  // empty strip
  }
  auto metrics = UITheme::getInstance().getMetrics();
  const int screenHeight = renderer.getScreenHeight();
  const int textLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const bool showProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                               SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::ONLY_BOOK_PROGRESS_BAR ||
                               SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const int progressBarTopY = screenHeight - progressBarBottomGap - metrics.bookProgressBarHeight;
  const int textY = showProgressBar ? (progressBarTopY - progressBarTextGap - textLineHeight)
                                    : (screenHeight - progressBarBottomGap - textLineHeight);
  int top = textY;
  if (!providerOverlayMsg_.empty()) {
    top = textY - textLineHeight - 4;
  }
  if (top > 2) top -= 2;
  if (top < 0) top = 0;
  return top;
}

bool TxtReaderActivity::computeStatusBarPhysicalWindow(uint16_t& x, uint16_t& y, uint16_t& w,
                                                       uint16_t& h) const {
  const int logW = renderer.getScreenWidth();
  const int logH = renderer.getScreenHeight();
  const int top = statusBarLogicalTopY();
  if (top >= logH || logW <= 0 || logH <= 0) return false;

  const auto orient = renderer.getOrientation();
  auto toPhy = [&](int lx, int ly, int& px, int& py) {
    switch (orient) {
      case GfxRenderer::Portrait:
        px = ly;
        py = HalDisplay::DISPLAY_HEIGHT - 1 - lx;
        break;
      case GfxRenderer::PortraitInverted:
        px = HalDisplay::DISPLAY_WIDTH - 1 - ly;
        py = lx;
        break;
      case GfxRenderer::LandscapeClockwise:
        px = HalDisplay::DISPLAY_WIDTH - 1 - lx;
        py = HalDisplay::DISPLAY_HEIGHT - 1 - ly;
        break;
      case GfxRenderer::LandscapeCounterClockwise:
      default:
        px = lx;
        py = ly;
        break;
    }
  };

  int minX = HalDisplay::DISPLAY_WIDTH, minY = HalDisplay::DISPLAY_HEIGHT;
  int maxX = -1, maxY = -1;
  auto consider = [&](int lx, int ly) {
    int px = 0, py = 0;
    toPhy(lx, ly, px, py);
    if (px < minX) minX = px;
    if (py < minY) minY = py;
    if (px > maxX) maxX = px;
    if (py > maxY) maxY = py;
  };
  consider(0, top);
  consider(logW - 1, top);
  consider(0, logH - 1);
  consider(logW - 1, logH - 1);
  consider(logW / 2, top);
  consider(logW / 2, logH - 1);
  consider(0, (top + logH - 1) / 2);
  consider(logW - 1, (top + logH - 1) / 2);
  if (maxX < minX || maxY < minY) return false;

  int x0 = minX & ~7;
  int x1 = (maxX + 8) & ~7;
  if (x1 > HalDisplay::DISPLAY_WIDTH) x1 = HalDisplay::DISPLAY_WIDTH;
  if (x0 < 0) x0 = 0;
  int y0 = minY;
  int y1 = maxY + 1;
  if (y0 < 0) y0 = 0;
  if (y1 > HalDisplay::DISPLAY_HEIGHT) y1 = HalDisplay::DISPLAY_HEIGHT;
  if (x1 <= x0 || y1 <= y0) return false;

  x = static_cast<uint16_t>(x0);
  y = static_cast<uint16_t>(y0);
  w = static_cast<uint16_t>(x1 - x0);
  h = static_cast<uint16_t>(y1 - y0);
  return (w % 8 == 0) && (x % 8 == 0) && w > 0 && h > 0;
}

bool TxtReaderActivity::computeBodyPhysicalWindow(uint16_t& x, uint16_t& y, uint16_t& w,
                                                  uint16_t& h) const {
  const uint16_t PW = HalDisplay::DISPLAY_WIDTH;
  const uint16_t PH = HalDisplay::DISPLAY_HEIGHT;
  if (SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::NONE) {
    x = 0;
    y = 0;
    w = PW;
    h = PH;
    return (w % 8 == 0) && w > 0 && h > 0;
  }
  uint16_t sx = 0, sy = 0, sw = 0, sh = 0;
  if (!computeStatusBarPhysicalWindow(sx, sy, sw, sh)) {
    x = 0;
    y = 0;
    w = PW;
    h = PH;
    return true;
  }
  if (sy == 0 && sh == PH) {
    if (sx == 0) {
      x = sw;
      y = 0;
      w = static_cast<uint16_t>(PW - x);
      h = PH;
    } else {
      x = 0;
      y = 0;
      w = sx;
      h = PH;
    }
  } else if (sx == 0 && sw == PW) {
    if (sy == 0) {
      x = 0;
      y = sh;
      w = PW;
      h = static_cast<uint16_t>(PH - y);
    } else {
      x = 0;
      y = 0;
      w = PW;
      h = sy;
    }
  } else {
    x = 0;
    y = 0;
    w = PW;
    h = PH;
  }
  if (w == 0 || h == 0) return false;
  const uint16_t xAligned = static_cast<uint16_t>(x & ~7u);
  const uint16_t xEnd = static_cast<uint16_t>((static_cast<uint32_t>(x) + w) & ~7u);
  if (xEnd <= xAligned) return false;
  const uint16_t yAligned = y;
  const uint16_t yEnd = y + h;
  x = xAligned;
  y = yAligned;
  w = static_cast<uint16_t>(xEnd - xAligned);
  h = static_cast<uint16_t>(yEnd - yAligned);
  return w > 0 && h > 0 && (w % 8 == 0);
}

bool TxtReaderActivity::chapter_loadPageIndexCache(int chapternum) {
  // Cache file format (using serialization module):
  // - uint32_t: magic "TXTI"
  // - uint8_t: cache version
  // Header (v10): magic, version, encodingType, fileSize, viewport, lines, fontId,
  // word/line/indent, margin, alignment, punctWidth, numPages, offsets...
  // Offsets are raw original-file bytes for the detected encoding.

  std::string cachePath = txt->getCachePath() +"/chapter"+ std::to_string(chapternum) + ".bin";
  FsFile f;
  if (!SdMan.openFileForRead("TRS", cachePath, f)) {
    Serial.printf("[%lu] [TRS] No page index cache found\n", millis());
    return false;
  }

  // Read and validate header using serialization module
  uint32_t magic;
  serialization::readPod(f, magic);
  if (magic != CACHE_MAGIC) {
    Serial.printf("[%lu] [TRS] Cache magic mismatch, rebuilding\n", millis());
    f.close();
    return false;
  }

  uint8_t version;
  serialization::readPod(f, version);
  if (version != CACHE_VERSION) {
    Serial.printf("[%lu] [TRS] Cache version mismatch (%d != %d), rebuilding\n", millis(), version, CACHE_VERSION);
    f.close();
    return false;
  }

  uint8_t cachedEnc = 0;
  serialization::readPod(f, cachedEnc);
  if (cachedEnc != static_cast<uint8_t>(txt->getEncodingType())) {
    Serial.printf("[%lu] [TRS] Cache encoding mismatch (%u != %d), rebuilding\n", millis(),
                  static_cast<unsigned>(cachedEnc), txt->getEncodingType());
    f.close();
    return false;
  }

  uint32_t fileSize;
  serialization::readPod(f, fileSize);
  if (fileSize != txt->getFileSize()) {
    Serial.printf("[%lu] [TRS] Cache file size mismatch, rebuilding\n", millis());
    f.close();
    return false;
  }

  int32_t cachedWidth;
  serialization::readPod(f, cachedWidth);
  if (cachedWidth != viewportWidth) {
    Serial.printf("[%lu] [TRS] Cache viewport width mismatch, rebuilding\n", millis());
    f.close();
    return false;
  }

  int32_t cachedLines;
  serialization::readPod(f, cachedLines);
  if (cachedLines != linesPerPage) {
    Serial.printf("[%lu] [TRS] Cache lines per page mismatch, rebuilding\n", millis());
    f.close();
    return false;
  }

  int32_t fontId;
  serialization::readPod(f, fontId);
  if (fontId != cachedFontId) {
    Serial.printf("[%lu] [TRS] Cache font ID mismatch (%d != %d), rebuilding\n", millis(), fontId, cachedFontId);
    f.close();
    return false;
  }
  //把字距行间距首行缩进记录进去
  int8_t wordSpacing;
  serialization::readPod(f, wordSpacing);
  if (wordSpacing != this->wordSpacing) {
    Serial.printf("[%lu] [TRS] Cache word spacing mismatch, rebuilding\n", millis());
    f.close();
    return false;
  }

  uint8_t lineSpacing;
  serialization::readPod(f, lineSpacing);
  if (lineSpacing != SETTINGS.customLineSpacing) {
    Serial.printf("[%lu] [TRS] Cache line spacing mismatch, rebuilding\n", millis());
    f.close();
    return false;
  }

  bool needIndent;
  Serial.printf("[%lu] [TRS] first line indent: %d\n", millis(), needIndent);
  serialization::readPod(f, needIndent);
  if (needIndent != SETTINGS.firstlineintented) {
    Serial.printf("[%lu] [TRS] Cache first line indent mismatch, rebuilding\n", millis());
    f.close();
    return false;
  }
//结束
  int32_t margin;
  serialization::readPod(f, margin);
  if (margin != cachedScreenMargin) {
    Serial.printf("[%lu] [TRS] Cache screen margin mismatch, rebuilding\n", millis());
    f.close();
    return false;
  }

  uint8_t alignment;
  serialization::readPod(f, alignment);
  if (alignment != cachedParagraphAlignment) {
    Serial.printf("[%lu] [TRS] Cache paragraph alignment mismatch, rebuilding\n", millis());
    f.close();
    return false;
  }

  // 标点宽度模式校验（紧凑/标准），设置变更时缓存失效
  uint8_t punctWidth;
  serialization::readPod(f, punctWidth);
  if (punctWidth != SETTINGS.chinesePunctWidth) {
    Serial.printf("[%lu] [TRS] Cache punct width mismatch (%d != %d), rebuilding\n",
                  millis(), punctWidth, SETTINGS.chinesePunctWidth);
    f.close();
    return false;
  }

  uint32_t numPages;
  serialization::readPod(f, numPages);

  // Read page offsets
  pageOffsets.clear();
  pageOffsets.reserve(numPages);

  for (uint32_t i = 0; i < numPages; i++) {
    uint32_t offset;
    serialization::readPod(f, offset);
    pageOffsets.push_back(offset);
  }

  f.close();
  totalPages = pageOffsets.size();
  Serial.printf("[%lu] [TRS] Loaded page index cache: %d pages\n", millis(), totalPages);
  return true;
}

void TxtReaderActivity::chapter_savePageIndexCache(int chapternum) const {
  std::string cachePath = txt->getCachePath() +"/chapter"+ std::to_string(chapternum) + ".bin";
  FsFile f;
  if (!SdMan.openFileForWrite("TRS", cachePath, f)) {
    Serial.printf("[%lu] [TRS] Failed to save page index cache\n", millis());
    return;
  }

  // Write header using serialization module (v10: encodingType after version)
  serialization::writePod(f, CACHE_MAGIC);
  serialization::writePod(f, CACHE_VERSION);
  serialization::writePod(f, static_cast<uint8_t>(txt->getEncodingType()));
  serialization::writePod(f, static_cast<uint32_t>(txt->getFileSize()));
  serialization::writePod(f, static_cast<int32_t>(viewportWidth));
  serialization::writePod(f, static_cast<int32_t>(linesPerPage));
  serialization::writePod(f, static_cast<int32_t>(cachedFontId));
  //把字距行间距首行缩进记录进去
  serialization::writePod(f, wordSpacing);
  serialization::writePod(f, SETTINGS.customLineSpacing);
  serialization::writePod(f, SETTINGS.firstlineintented);
  //结束
  serialization::writePod(f, static_cast<int32_t>(cachedScreenMargin));
  serialization::writePod(f, cachedParagraphAlignment);
  // 标点宽度模式（紧凑/标准），设置变更时缓存失效
  serialization::writePod(f, SETTINGS.chinesePunctWidth);
  serialization::writePod(f, static_cast<uint32_t>(pageOffsets.size()));

  // Write page offsets
  for (size_t offset : pageOffsets) {
    serialization::writePod(f, static_cast<uint32_t>(offset));
  }

  f.close();
  Serial.printf("[%lu] [TRS] Saved page index cache: %d pages\n", millis(), totalPages);
}
