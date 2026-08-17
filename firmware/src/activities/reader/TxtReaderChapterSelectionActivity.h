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
  // Explicit label complements the existing 56px header-back hit target.
  std::string headerTitle_ = "返回  目  录";
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  int chapternum = 0;
  int selectorIndex = 0;
  int page = 1;
  int loadedBatchStart_ = -1;
  bool updateRequired = false;
  bool firstPaint_ = true;
  int prefetchedBatch_ = -1;
  std::atomic<bool> finished_{false};
  std::atomic<bool> displayBusy_{false};
  bool cancelled_ = true;
  int selectedIndex_ = -1;
  const std::function<void()> onGoBack;
  const std::function<void(int newChapterNum)> onSelectchapter;

  int getPageItems() const;
  int chapterCount() const;
  int maxPage() const;
  bool chapterExists(int chapterIndex) const;
  std::string chapterTitle(int chapterIndex) const;
  static int chapterBatchStart(int chapterIndex);
  bool ensureChapterBatch(int chapterIndex, bool* outFromCache = nullptr);
  void skipChapters(int delta);
  void prefetchNextBatchQuiet();
  void materializePageTitles(int pagebegin, int pageItems, std::vector<std::string>& outTitles,
                             std::vector<uint8_t>& outPresent);
  void drawScreen(const std::vector<std::string>& pageTitles, const std::vector<uint8_t>& pagePresent,
                  int pagebegin, int pageItems);

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();

 public:
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

  explicit TxtReaderChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             std::vector<std::string> titles, int currentIndex,
                                             const std::function<void()>& onGoBack,
                                             const std::function<void(int newChapterNum)>& onSelectchapter,
                                             std::string headerTitle = "返回  目  录")
      : Activity("TxtReaderChapterSelection", renderer, mappedInput),
        externalTitles_(std::move(titles)),
        externalCount_(static_cast<int>(externalTitles_.size())),
        useExternal_(true),
        headerTitle_(std::move(headerTitle)),
        chapternum(currentIndex),
        onGoBack(onGoBack),
        onSelectchapter(onSelectchapter) {}

  explicit TxtReaderChapterSelectionActivity(
      GfxRenderer& renderer, MappedInputManager& mappedInput, int chapterCount,
      std::function<bool(int firstIndex, int count, std::vector<std::string>& titles,
                         std::vector<uint8_t>& present)> pageLoader,
      int currentIndex, const std::function<void()>& onGoBack,
      const std::function<void(int newChapterNum)>& onSelectchapter,
      std::string headerTitle = "返回  目  录")
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
