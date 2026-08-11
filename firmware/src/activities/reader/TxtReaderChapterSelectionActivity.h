#pragma once
#include <Txt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../Activity.h"

// System chapter list UI. Two data modes:
//  1) Library TXT: parseChapterIndexAndOffset batches via shared_ptr<Txt>
//  2) External titles: legacy JSON vector or a provider page loader
// Always draws chapter names with reader full-CJK font (not UI subset).
class TxtReaderChapterSelectionActivity final : public Activity {
  std::shared_ptr<Txt> txt;
  std::vector<std::string> externalTitles_;
  std::function<bool(int firstIndex, int count, std::vector<std::string>& titles,
                     std::vector<uint8_t>& present)> externalPageLoader_;
  int externalCount_ = 0;
  int externalCacheBegin_ = -1;
  int externalCacheCount_ = 0;
  std::vector<std::string> externalCacheTitles_;
  std::vector<uint8_t> externalCachePresent_;
  bool useExternal_ = false;
  std::string headerTitle_ = "目  录";
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  int chapternum = 0;
  int selectorIndex = 0;
  int page = 1;
  // Last batch start passed to parseChapterIndexAndOffset (multiples of 25).
  // -1 = none loaded this session. Never use (page-1)*pageItems as batch key.
  int loadedBatchStart_ = -1;
  bool updateRequired = false;
  // First paint after handoff may need HALF to clear residual plugin UI; later pages FAST.
  bool firstPaint_ = true;
  // Last batch we already attempted silent prefetch for (avoid repeat scans).
  int prefetchedBatch_ = -1;
  // True after user confirms a chapter (or back). Parent may poll after loop.
  // Display task must stop taking work when finished_ is set (select/back → exit).
  std::atomic<bool> finished_{false};
  // True while display task is materializing/drawing/refreshing (not parked).
  std::atomic<bool> displayBusy_{false};
  bool cancelled_ = true;
  int selectedIndex_ = -1;
  const std::function<void()> onGoBack;
  const std::function<void(int newChapterNum)> onSelectchapter;

  int getPageItems() const;
  int chapterCount() const;
  // Max page number (1-based). External: from title count. Library TXT: unbounded.
  int maxPage() const;
  bool chapterExists(int chapterIndex) const;
  std::string chapterTitle(int chapterIndex) const;
  static int chapterBatchStart(int chapterIndex);
  // Load 25-chapter batch covering chapterIndex (no-op in external mode).
  // outFromCache: true if SD batch hit (skip "加载中" flash).
  bool ensureChapterBatch(int chapterIndex, bool* outFromCache = nullptr);
  void skipChapters(int delta);
  // Build next batch on disk if missing, then restore current batch into RAM.
  void prefetchNextBatchQuiet();
  // Copy titles for [pagebegin, pagebegin+pageItems) into outTitles/outPresent
  // so a page that spans two 25-chapter batches never loses the first half when
  // the second batch is loaded (single-slot RAM chapterDataList).
  void materializePageTitles(int pagebegin, int pageItems, std::vector<std::string>& outTitles,
                             std::vector<uint8_t>& outPresent);
  // Draw frame only (no e-ink). Call displayBuffer outside renderingMutex.
  void drawScreen(const std::vector<std::string>& pageTitles, const std::vector<uint8_t>& pagePresent,
                  int pagebegin, int pageItems);

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();

 public:
  // Library multi-chapter TXT (scans file / batch cache).
  explicit TxtReaderChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const std::shared_ptr<Txt> txt, int chapternum,
                                             const std::function<void()>& onGoBack,
                                             const std::function<void(int newChapterNum)>& onSelectchapter)
      : Activity("TxtReaderChapterSelection", renderer, mappedInput),
        txt(txt),
        useExternal_(false),
        chapternum(chapternum),
        onGoBack(onGoBack),
        onSelectchapter(onSelectchapter) {}

  // Plugin / external titles (WeRead toc.json). Indices are 0-based.
  explicit TxtReaderChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             std::vector<std::string> titles, int currentIndex,
                                             const std::function<void()>& onGoBack,
                                             const std::function<void(int newChapterNum)>& onSelectchapter,
                                             std::string headerTitle = "目  录")
      : Activity("TxtReaderChapterSelection", renderer, mappedInput),
        externalTitles_(std::move(titles)),
        externalCount_(static_cast<int>(externalTitles_.size())),
        useExternal_(true),
        headerTitle_(std::move(headerTitle)),
        chapternum(currentIndex),
        onGoBack(onGoBack),
        onSelectchapter(onSelectchapter) {}

  // Provider-backed external list.  The system picker owns the visual layout,
  // while this callback loads only the currently visible rows.  The callback
  // is never invoked by touch handling; display-task paints populate a small
  // current-page cache, so taps remain responsive even on slow SD media.
  explicit TxtReaderChapterSelectionActivity(
      GfxRenderer& renderer, MappedInputManager& mappedInput, int chapterCount,
      std::function<bool(int firstIndex, int count, std::vector<std::string>& titles,
                         std::vector<uint8_t>& present)> pageLoader,
      int currentIndex, const std::function<void()>& onGoBack,
      const std::function<void(int newChapterNum)>& onSelectchapter,
      std::string headerTitle = "目  录")
      : Activity("TxtReaderChapterSelection", renderer, mappedInput),
        externalPageLoader_(std::move(pageLoader)),
        externalCount_(chapterCount < 0 ? 0 : chapterCount),
        useExternal_(true),
        headerTitle_(std::move(headerTitle)),
        chapternum(currentIndex),
        onGoBack(onGoBack),
        onSelectchapter(onSelectchapter) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

  bool finished() const { return finished_; }
  bool cancelled() const { return cancelled_; }
  int selectedIndex() const { return selectedIndex_; }
};
