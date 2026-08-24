#pragma once

#include <Txt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "EpubReaderMenuActivity.h"
#include "activities/ActivityWithSubactivity.h"
#include "apps/M4PluginReaderSession.h"
#include "util/M4ContentProviderContract.h"
#include "util/M4TxtIndexPolicy.h"

class TxtReaderActivity final : public ActivityWithSubactivity {
 public:
  // Plugin / WeRead transient session: same native UI as library TXT reader,
  // without polluting Recent Books or treating the file as a multi-chapter book.
  struct PluginSession {
    bool active = false;
    bool suppressRecentBooks = true;
    bool suppressOpenEpubPath = true;
    bool progressiveIndex = true;
    std::string bookId;
    std::string chapterUid;
    std::string progressKey;
    std::string titleOverride;
    uint32_t generation = 0;
    size_t initialByteOffset = 0;
    bool hasInitialByteOffset = false;
    // App-data relative toc.json + current 0-based chapter for system TOC reuse.
    std::string tocRelPath;
    std::string tocAbsPath;
    int chapterIndex = 0;
    // ContentProvider-managed book (provider-agnostic; not a filesystem path).
    bool providerManaged = false;
    std::string providerId;    // e.g. "weread" (URI segment, NOT app id)
    std::string appId;         // installed m4x id e.g. "com.weread.client" — history author field
    std::string appDataRoot;   // /apps_data/<appId>
    std::string cacheRelPath;  // current chapter relative .txt
  };

  struct PluginProgress {
    // valid=false ⇒ lock not acquired or no coherent index; do NOT persist.
    bool valid = false;
    int page = 0;     // 0-based
    int total = -1;   // -1 while index incomplete
    size_t byteOffset = 0;
    bool indexComplete = false;
    uint32_t generation = 0;  // exact session generation (not live counter)
    std::string bookId;
    std::string chapterUid;
    std::string progressKey;
    // >=0: user selected another chapter from system TOC (0-based).
    int switchChapterIndex = -1;
  };

  explicit TxtReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Txt> txt,
                             const std::function<void()>& onGoBack, const std::function<void()>& onGoHome)
      : ActivityWithSubactivity("TxtReader", renderer, mappedInput),
        txt(std::move(txt)),
        onGoBack(onGoBack),
        onGoHome(onGoHome) {}

  explicit TxtReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Txt> txt,
                             const std::function<void()>& onGoBack, const std::function<void()>& onGoHome,
                             PluginSession pluginSession)
      : ActivityWithSubactivity("TxtReader", renderer, mappedInput),
        txt(std::move(txt)),
        onGoBack(onGoBack),
        onGoHome(onGoHome),
        pluginSession_(std::move(pluginSession)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool preventAutoSleep() override { return automaticPageTurnActive; }
  bool isReaderActivity() const override { return true; }
  bool readerMenuSyncSupported() const override { return false; }
  void onReaderMenuStyleChanged() override { onSettingsChanged(); }

  // Parent observes after child loop returns (do not delete from onGoBack).
  bool pluginCloseRequested() const { return pluginCloseRequested_; }
  // Blocks until a coherent snapshot is available (may wait on e-paper/index).
  // Never returns a fabricated page-0 default on timeout — valid=false if impossible.
  PluginProgress pluginProgressSnapshot() const;
  bool pluginFirstPageReady() const;
  bool pluginIndexComplete() const;

 private:
  std::shared_ptr<Txt> txt;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  int currentPage = 0;
  int totalPages = 1;
  int pagesUntilFullRefresh = 0;
  bool updateRequired = false;
  bool pendingGoBack = false;
  bool pendingGoHome = false;
  const std::function<void()> onGoBack;
  const std::function<void()> onGoHome;

  std::vector<size_t> pageOffsets;
  std::vector<std::string> currentPageLines;
  std::vector<int> currentPageIndentOffsets;
  std::vector<bool> currentPageJustify;
  int linesPerPage = 0;
  int viewportWidth = 0;
  bool initialized = false;

  int cachedFontId = 0;
  int cachedScreenMargin = 0;
  uint8_t cachedParagraphAlignment = CrossPointSettings::LEFT_ALIGN;

  static constexpr int DUAL_PAGE_GUTTER = 8;
  bool isLandscapeDualPage() const;
  int dualRightPage = -1;
  bool dualNextLeft = true;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();
  void renderDualPage();
  void renderPage(bool skipDisplay = false, int xOffset = 0, bool skipInvert = false);
  void renderStatusBar(int orientedMarginRight, int orientedMarginBottom, int orientedMarginTop,
                       int orientedMarginLeft) const;

  // maxReadBytes: exclusive read-window size from offset (0 = default CHUNK_SIZE 8KiB).
  // First-page adaptive path passes growing 8/16/.../48KiB so expansion is real, not a re-read of 8KiB.
  // outJustify: if non-null, write per-line justify flags there; if null, write currentPageJustify.
  // Index builders MUST pass a scratch vector so progressive indexing does not clobber the
  // on-screen page's justify flags (causes mid-read left/justify flicker).
  bool loadPageAtOffset(size_t offset, size_t endoffset, std::vector<std::string>& outLines, size_t& nextOffset,
                        const uint8_t* preloadBuf = nullptr, size_t preloadBufOffset = 0, size_t preloadBufSize = 0,
                        size_t maxReadBytes = 0, std::vector<bool>* outJustify = nullptr);
  void buildPageIndex(size_t beginByte, size_t endByte);

  // Progressive indexing (plugin whole-file mode).
  void buildPageIndexFirstPage(size_t beginByte, size_t endByteExclusive);
  int continuePageIndex(int maxPages, size_t maxBytes);
  size_t chapterContentEnd() const;  // exclusive end for loadPageAtOffset

  void saveProgress() const;
  void loadProgress();
  void persistOpenHistory();
  int chapternum = 0;
  bool chapter_loadPageIndexCache(int chapternum);
  void chapter_savePageIndexCache(int chapternum) const;
  // Next-chapter page-index prefetch (provider-like). While the current chapter
  // is fully indexed and the panel idle, chapter N+1's pageOffsets are built in
  // the background and saved to chapter{N+1}.bin so a cross-chapter open is a
  // cache hit. SD is time-sliced; never runs during an animation.
  void libraryPrefetchReset();
  bool chapter_pageIndexCacheExists(int ch) const;
  void chapter_savePageIndexCacheOffsets(int ch, const std::vector<size_t>& offsets) const;
  void libraryIdlePrefetchNextChapter();
  // Large local TXT: discover one 25-chapter metadata batch per idle slice.
  // The picker remains cache-only while these batches are materialized.
  void libraryIdleDiscoverChapterBatch();
  // Diagnostic: append a page-load failure reason to the SD debug log (serial
  // channel is unreliable on M4) so "every-other-page refresh" root causes are
  // readable from the host.
  void logPageLoadFail(const char* why, size_t offset, size_t bytes) const;
  // Perf diagnostic: append a stage timing line to the SD perf log (serial
  // channel is unreliable on M4). Used to find which render step is slow
  // (TTF glyph rasterization vs SD read/decode vs physical refresh).
  void logPerf(const char* step, uint32_t ms, int page, uint32_t extra = 0) const;
  // Rate gate for the per-page perf line: hot indexing calls loadPageAtOffset
  // hundreds of times and an unconditional SD append stalls that path. State is
  // touched from the display task only; mutable because logPerf callers are const.
  mutable M4TxtIndexPolicy::LoadPageLogGate loadPageLogGate_;
  // Physical body rectangle (panel-native 800x480, byte-aligned) — the page-turn
  // wipe window covers exactly this so status/other regions are off-panel.
  bool computeBodyPhysicalWindow(uint16_t& x, uint16_t& y, uint16_t& w, uint16_t& h) const;
  int statusBarLogicalTopY() const;
  bool computeStatusBarPhysicalWindow(uint16_t& x, uint16_t& y, uint16_t& w, uint16_t& h) const;
  void chapter_initializeReader(int chapternum);
  bool chapter_initialized = false;
  bool needIndent = SETTINGS.firstlineintented;
  int8_t wordSpacing = SETTINGS.wordSpacing;

  void openMenu(EpubReaderMenuActivity::MenuLayer layer = EpubReaderMenuActivity::MenuLayer::QUICK);
  // A menu/settings handoff must not replay page taps that arrived while the
  // physical page-turn animation was busy. Completed page progress remains;
  // only unapplied input/quick-turn scheduling is discarded.
  void cancelPendingPageTurnForChild();
  void enterChapterPicker();
  void handleMenuAction(EpubReaderMenuActivity::MenuAction action);
  void onSettingsChanged();
  // UI/menu path: acquires the state lock then jumps.
  void goToPercent(int percent);
  // Display/render path: REQUIRES state lock already held (non-recursive mutex).
  // Never call from unlocked code; never call goToPercent while holding the lock.
  void goToPercentAlreadyLocked(int percent);

  unsigned long lastAutoPageTurnTime = 0;
  bool automaticPageTurnActive = false;
  unsigned long pageTurnDuration = 0;
  bool rollingMode = false;
  bool rollingHalfTurned = false;
  void applyAutoPageTurnSettings();

  bool pendingBluetoothSettings = false;
  bool globalNextPageMode = false;
  bool globalNextPageModeToggled = false;
  bool skipNextButtonCheck = false;
  int cachedPage = -1;
  int m_pendingJumpPercent = -1;

  // --- Plugin session ---
  PluginSession pluginSession_{};
  // Set when plugin TOC selects another chapter; published in pluginProgressSnapshot.
  int pluginSwitchChapterIndex_ = -1;

  // bool-like close flag with one narrowly scoped side effect: if the provider
  // bridge has converted an empty-path next-chapter open into a list-style
  // fallback intent, the first requestPluginClose() copies that chapter index
  // into the existing reader progress field and schedules the normal onGoBack
  // callback. Thus automatic chapter-end fallback follows exactly the same
  // tested close → Lua loading page → download → reopen path as manual TOC.
  struct PluginCloseFlag {
    bool value = false;
    bool* pendingGoBack = nullptr;
    int* switchChapterIndex = nullptr;

    PluginCloseFlag(bool* goBack, int* switchIndex)
        : value(false), pendingGoBack(goBack), switchChapterIndex(switchIndex) {}

    PluginCloseFlag& operator=(bool v) {
      value = v;
      if (v && pendingGoBack && switchChapterIndex && *switchChapterIndex < 0) {
        const int fallback = M4PluginReaderSession::pendingFallbackSwitchChapterIndex();
        if (fallback >= 0) {
          *switchChapterIndex = fallback;
          *pendingGoBack = true;
        }
      }
      return *this;
    }
    operator bool() const { return value; }
  };
  PluginCloseFlag pluginCloseRequested_{&pendingGoBack, &pluginSwitchChapterIndex_};

  bool firstPageReady_ = false;
  bool firstReadableLogged_ = false;
  bool indexComplete_ = true;
  size_t indexRangeEnd_ = 0;   // exclusive file end for progressive index
  size_t indexCursor_ = 0;     // next page-start to discover
  uint32_t lastStatusRefreshMs_ = 0;
  size_t pendingRestoreByte_ = 0;
  bool hasPendingRestore_ = false;
  bool userMovedPage_ = false;
  bool tidxSaved_ = false;  // save completed .tidx once per layout generation
  // Large local TXT opens directly into the progressive whole-file index. The
  // fast path is limited to the chapter that was active at open; later chapter
  // switches use the normal chapter/cache path.
  bool largeTxtFastOpen_ = false;
  int fastOpenChapter_ = -1;
  size_t activeChapterBegin_ = 0;
  size_t activeChapterEnd_ = 0;
  size_t resumeRangeBegin_ = 0;
  size_t resumeRangeEnd_ = 0;
  bool progressSavePending_ = false;
  bool openHistorySavePending_ = false;
  int chapterDiscoveryBatch_ = -1;
  bool chapterDiscoveryDone_ = true;
  // High-density local TXT files can have tens of thousands of headings.  Do
  // not let the first idle pass scan a batch before the first content frame is
  // on the panel, and leave a short gap between SD/cache batches so UI input
  // and the reader's own page/index work keep their time slices.
  uint32_t chapterDiscoveryNotBeforeMs_ = 0;
  uint32_t chapterDiscoveryNextMs_ = 0;
  // Next-chapter prefetch state (library). See libraryIdlePrefetchNextChapter.
  int prefetchChapter_ = -1;
  std::vector<size_t> prefetchOffsets_;
  size_t prefetchCursor_ = 0;
  size_t prefetchRangeEnd_ = 0;
  bool prefetchComplete_ = false;
  bool prefetchSkipped_ = false;
  // First physical paint after openText handoff: layout under lock, then a
  // fast refresh outside the lock.  Strong/full waveforms are globally
  // forbidden outside the explicit reader-body cleanup cadence.
  bool pluginNeedsClearRefresh_ = false;
  bool pluginPendingHalfFlush_ = false;
  // Provider next-chapter overlay (footer/status); empty when idle.
  std::string providerOverlayMsg_;
  // Last overlay state that drove a physical refresh. Only a state transition
  // (Missing→Fetching→Error/Ready) repaints the panel; pct-only churn updates
  // providerOverlayMsg_ in memory without a full-frame differential.
  M4ContentProvider::ChapterReady providerOverlayState_ = M4ContentProvider::ChapterReady::Ready;
  bool providerPrefetchRequested_ = false;
  bool tryProviderNextChapterAdvance();  // last-page next / seamless open
  void providerIdlePrefetchNext();
  bool switchToProviderChapter(const std::string& cacheRelPath, int index0, const std::string& chapterUid,
                               const std::string& title);
  // System TOC pick. Cached provider chapters stay in this reader; uncached
  // chapters still close so a parent that owns fetch can reopen.
  void applyPluginTocSelection(int newChapterNum);
  // Library path: layout under lock only; e-ink + AA must run unlocked so the
  // main loop is not frozen for ~1.5s per page (see open hang serial analysis).
  bool deferPhysicalEpd_ = false;
  bool libraryPhysicalPending_ = false;
  // True while finishPhysicalDisplay / plugin half is on the panel (SPI busy).
  // Display task vs UI task: atomic, not volatile (ordering + visibility).
  std::atomic<bool> physicalEpdBusy_{false};
  std::atomic<bool> firstPhysicalShown_{false};  // first content page has been driven to panel
  // Enter/return to reader: flush pure white first so page-turn anim and FAST
  // never diff against the previous activity (shelf/menu/loading residual).
  bool entryWhiteSeedPending_ = false;
  enum class EntryPlaceholderKind : uint8_t { None, Opening, NextChapter };
  // Show a visible state while replacing the previous activity/chapter frame;
  // a bare white seed looks indistinguishable from a hung reader.
  EntryPlaceholderKind entryPlaceholderKind_ = EntryPlaceholderKind::None;
  // Last body page that received a physical EPD drive. Same-page buffer updates
  // (status "1/?"→"1/20", footer overlay churn) must NOT FAST-diff again —
  // that was the residual/ghost buildup while progressive index ran.
  int lastPhysicalBodyPage_ = -1;
  // Decoupled quick page skip: rapid taps advance currentPage (user target)
  // without loading/rendering; the physical refresh catches up once the
  // in-flight animation finishes and the panel is idle (one fast refresh
  // straight to the target, intermediate pages skipped). No debounce — a slow
  // tap (panel idle) starts the animation immediately.
  bool quickMode_ = false;
  uint32_t lastPageTurnMs_ = 0;
  // Taps that arrived while the display task holds the state lock. Applied on
  // the next unlocked UI tick so poll() never waits on TTF layout.
  std::atomic<int> pendingTurnDelta_{0};
  // Physical frame snapshot: the page actually laid into the framebuffer and
  // submitted to the EPD. Updated when render starts; consumed by
  // finishPhysicalDisplay AFTER the animation settles. NEVER assign
  // lastPhysicalBodyPage_ from the live currentPage — it keeps advancing
  // during the animation and would falsely mark the target as already shown
  // (lost click: catch-up never fires because currentPage==lastPhysicalBodyPage_).
  int pendingPhysicalPage_ = -1;
  // Set on onExit / openMenu so display task stops starting new frames.
  std::atomic<bool> suppressDisplay_{false};
  void finishPhysicalDisplay();  // displayBuffer + optional AA (no state lock)
  void waitPhysicalEpdIdle(uint32_t maxMs = 2500);
  void armEntryWhiteSeed(EntryPlaceholderKind kind = EntryPlaceholderKind::Opening);
  // Overlay dismiss: FAST the current page over the menu (same LUT as the
  // bar). Do not re-arm the "正在打开阅读器" placeholder.
  void armOverlayReturnFlush();
  bool overlayReturnFlush_ = false;

  // Deferred nested-menu teardown (requestExitSubActivity + apply after pump).
  bool deferredMenuApply_ = false;
  uint8_t deferredMenuOrientation_ = 0;
  bool deferredMenuNeedRebuild_ = false;
  std::function<void()> deferredChildTransition_;
  // Chapter picker selected while state lock was busy (never block forever).
  bool hasDeferredChapterSwitch_ = false;
  int deferredChapterSwitch_ = 0;

  bool loadPluginTidx();
  void savePluginTidx() const;
  void requestPluginClose();
  void applyPendingRestoreIfReady();  // under renderingMutex
  int pageIndexForByteLocked(size_t byteOffset) const;
  void applyDeferredMenuClose();
  std::string displayTitle() const;

  // State lock helpers (non-recursive). UI may block waiting for display/index.
  bool lockState(TickType_t ticks = portMAX_DELAY) const;
  void unlockState() const;
  // Coherent page turn under lock (plugin and library share pageOffsets).
  void pageTurnLocked(int delta);
};
