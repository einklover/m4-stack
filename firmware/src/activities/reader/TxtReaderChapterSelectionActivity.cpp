#include "TxtReaderChapterSelectionActivity.h"

#include <EpdFontLoader.h>
#include <GfxRenderer.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4ListTouchPolicy.h"
#include "util/M4TouchListMetrics.h"
#include "util/M4UiText.h"
#include "util/TouchHitGeometry.h"

#include <cstring>

namespace {
constexpr int SKIP_PAGE_MS = 700;
// TXT stores chapter metadata in batches of 25 (see MAX_SAVE_CHAPTER / parseChapterIndexAndOffset).
constexpr int CHAPTER_BATCH = 25;
constexpr int ITEM_SKIP_100_BACK = -2;
constexpr int ITEM_SKIP_100_FORWARD = -1;
}  // namespace

int TxtReaderChapterSelectionActivity::getPageItems() const {
  const bool touch = mappedInput.hasTouch();
  const int lineHeight = M4TouchListMetrics::chapterLineHeight(touch);
  // Keep the chapter picker on the same header/list/footer rhythm as the
  // system library and reader menus. The former skip-chip row made this
  // screen visually inconsistent and consumed almost 100 px of touch space.
  const int startY = M4TouchListMetrics::chapterListTop(touch);

  const int screenHeight = renderer.getScreenHeight();
  const int availableHeight = screenHeight - startY - M4TouchListMetrics::chapterFooterReserve(touch);
  int items = availableHeight / lineHeight;
  if (items < 1) {
    items = 1;
  }
  return items;
}

int TxtReaderChapterSelectionActivity::chapterCount() const {
  if (useExternal_) {
    return externalPageLoader_ ? externalCount_ : static_cast<int>(externalTitles_.size());
  }
  return 0;  // library mode uses isChapterExist stream, no total required
}

int TxtReaderChapterSelectionActivity::maxPage() const {
  if (!useExternal_) return 100000;  // library TXT has no total; clamp lightly
  const int n = chapterCount();
  const int pi = getPageItems();
  if (n <= 0 || pi <= 0) return 1;
  return (n + pi - 1) / pi;
}

bool TxtReaderChapterSelectionActivity::chapterExists(int chapterIndex) const {
  if (chapterIndex < 0) return false;
  if (useExternal_) return chapterIndex < chapterCount();
  return txt && txt->isChapterExist(chapterIndex);
}

std::string TxtReaderChapterSelectionActivity::chapterTitle(int chapterIndex) const {
  if (useExternal_) {
    if (chapterIndex < 0 || chapterIndex >= chapterCount()) return {};
    if (externalPageLoader_) {
      char fallback[32];
      snprintf(fallback, sizeof(fallback), "第%d章", chapterIndex + 1);
      return fallback;
    }
    return externalTitles_[static_cast<size_t>(chapterIndex)];
  }
  if (!txt) return {};
  return txt->getChapterTitleByIndex(chapterIndex);
}

int TxtReaderChapterSelectionActivity::chapterBatchStart(int chapterIndex) {
  if (chapterIndex < 0) chapterIndex = 0;
  return (chapterIndex / CHAPTER_BATCH) * CHAPTER_BATCH;
}

bool TxtReaderChapterSelectionActivity::ensureChapterBatch(int chapterIndex, bool* outFromCache) {
  if (outFromCache) *outFromCache = true;
  if (useExternal_) return chapterExists(chapterIndex);
  if (!txt || finished_) return false;

  // Main-loop / touch path: serialize against display-task materialize/prefetch.
  const bool locked =
      renderingMutex && (xSemaphoreTake(renderingMutex, pdMS_TO_TICKS(500)) == pdTRUE);
  if (!locked && renderingMutex) {
    // Display busy painting; answer from whatever batch is currently in RAM.
    return txt->isChapterExist(chapterIndex);
  }

  const int batch = chapterBatchStart(chapterIndex);
  if (loadedBatchStart_ != batch) {
    const bool cacheHit = txt->parseChapterIndexAndOffset(batch);
    loadedBatchStart_ = batch;
    if (outFromCache) *outFromCache = cacheHit;
  } else if (outFromCache) {
    *outFromCache = true;  // already in RAM
  }
  const bool ok = txt->isChapterExist(chapterIndex);
  if (locked) xSemaphoreGive(renderingMutex);
  return ok;
}

void TxtReaderChapterSelectionActivity::prefetchNextBatchQuiet() {
  if (useExternal_ || !txt || loadedBatchStart_ < 0 || finished_) return;
  // Also rebuild current batch if paint path left it empty (cache purged / miss).
  const int cur = loadedBatchStart_;
  if (cur >= 0 && !txt->isChapterExist(cur) && !txt->hasChapterBatchCache(cur)) {
    Serial.printf("[ChapterPrefetch] rebuild empty current batch %d\n", cur);
    txt->parseChapterIndexAndOffset(cur, /*allowScan=*/true);
    loadedBatchStart_ = cur;
    if (txt->isChapterExist(cur)) {
      updateRequired = true;  // repaint with real titles
    }
  }
  if (finished_) return;
  const int next = loadedBatchStart_ + CHAPTER_BATCH;
  if (next < 0 || next > 50000) return;
  if (prefetchedBatch_ == next) return;
  if (txt->hasChapterBatchCache(next)) {
    prefetchedBatch_ = next;
    return;
  }
  // Scan next batch to SD (clobbers RAM), then restore current batch from cache.
  // If next is past EOF, parse returns immediately (m_emptyFromBatch_).
  Serial.printf("[ChapterPrefetch] building batch %d (then restore %d)\n", next, cur);
  const bool nextOk = txt->parseChapterIndexAndOffset(next, /*allowScan=*/true);
  if (finished_) return;
  // Only mark prefetched if we got data or known empty — avoid retry storms.
  prefetchedBatch_ = next;
  if (cur >= 0) {
    txt->parseChapterIndexAndOffset(cur, /*allowScan=*/true);
    loadedBatchStart_ = cur;
  }
  (void)nextOk;
}

void TxtReaderChapterSelectionActivity::materializePageTitles(int pagebegin, int pageItems,
                                                             std::vector<std::string>& outTitles,
                                                             std::vector<uint8_t>& outPresent) {
  outTitles.assign(static_cast<size_t>(pageItems), {});
  outPresent.assign(static_cast<size_t>(pageItems), 0);
  if (pageItems <= 0 || finished_) return;

  if (useExternal_) {
    const int n = chapterCount();
    if (externalPageLoader_) {
      if (externalCacheBegin_ == pagebegin && externalCacheCount_ == pageItems &&
          externalCacheTitles_.size() == static_cast<size_t>(pageItems) &&
          externalCachePresent_.size() == static_cast<size_t>(pageItems)) {
        outTitles = externalCacheTitles_;
        outPresent = externalCachePresent_;
        return;
      }
      for (int r = 0; r < pageItems; ++r) {
        const int i = pagebegin + r;
        if (i >= 0 && i < n) outPresent[static_cast<size_t>(r)] = 1;
      }
      if (!externalPageLoader_(pagebegin, pageItems, outTitles, outPresent)) {
        // Preserve selectable rows even if a transient SD read fails.  The
        // renderer supplies an index-aligned fallback title for empty cells.
        for (int r = 0; r < pageItems; ++r) {
          const int i = pagebegin + r;
          if (i >= 0 && i < n) outPresent[static_cast<size_t>(r)] = 1;
        }
      }
      externalCacheBegin_ = pagebegin;
      externalCacheCount_ = pageItems;
      externalCacheTitles_ = outTitles;
      externalCachePresent_ = outPresent;
      return;
    }
    for (int r = 0; r < pageItems; ++r) {
      const int i = pagebegin + r;
      if (i >= 0 && i < n) {
        outTitles[static_cast<size_t>(r)] = externalTitles_[static_cast<size_t>(i)];
        outPresent[static_cast<size_t>(r)] = 1;
      }
    }
    return;
  }

  if (!txt) return;

  // Walk the visible range in order. When the batch changes, load it (clobbers
  // RAM) but titles for earlier rows were already copied into outTitles.
  int prevBatch = -999;
  for (int r = 0; r < pageItems; ++r) {
    if (finished_) return;
    const int i = pagebegin + r;
    if (i < 0) continue;
    const int batch = chapterBatchStart(i);
    if (batch != prevBatch) {
      if (loadedBatchStart_ != batch) {
        Serial.printf("[ChapterSel] load batch %d for page row %d\n", batch, r);
        // Paint path: cache only. Full rebuild of purged/incomplete batches is
        // done in idle prefetch so selecting a chapter cannot soft-lock the UI.
        txt->parseChapterIndexAndOffset(batch, /*allowScan=*/false);
        loadedBatchStart_ = batch;
      }
      prevBatch = batch;
    }
    if (txt->isChapterExist(i)) {
      outTitles[static_cast<size_t>(r)] = txt->getChapterTitleByIndex(i);
      outPresent[static_cast<size_t>(r)] = 1;
    }
  }
}

void TxtReaderChapterSelectionActivity::skipChapters(int delta) {
  const int pageItems = getPageItems();
  int first = (page - 1) * pageItems;
  if (selectorIndex >= 0) first = selectorIndex;
  int target = first + delta;
  if (target < 0) target = 0;

  if (useExternal_) {
    const int n = chapterCount();
    if (n <= 0) {
      target = 0;
    } else if (target >= n) {
      target = n - 1;
    }
  } else if (!ensureChapterBatch(target) || !chapterExists(target)) {
    int batch = chapterBatchStart(target);
    int found = -1;
    while (batch >= 0) {
      txt->parseChapterIndexAndOffset(batch);
      loadedBatchStart_ = batch;
      for (int i = 0; i < CHAPTER_BATCH; ++i) {
        const int ch = batch + i;
        if (chapterExists(ch)) found = ch;
      }
      if (found >= 0) {
        target = found;
        break;
      }
      if (batch == 0) {
        target = 0;
        break;
      }
      batch -= CHAPTER_BATCH;
    }
  }

  page = target / pageItems + 1;
  if (page < 1) page = 1;
  selectorIndex = target;
  updateRequired = true;
  Serial.printf("[ChapterSkip] delta=%d target=%d page=%d external=%d\n", delta, target, page,
                useExternal_ ? 1 : 0);
}

void TxtReaderChapterSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<TxtReaderChapterSelectionActivity*>(param);
  self->displayTaskLoop();
}

void TxtReaderChapterSelectionActivity::onEnter() {
  Activity::onEnter();

  // Full-CJK titles need reader IDs promoted from SD epdfont (not UI subset → '?').
  // ensure*: skip if boot/settings already loaded; full scan only when never done.
  EpdFontLoader::ensureFontsFromSd(renderer);

  renderingMutex = xSemaphoreCreateMutex();
  loadedBatchStart_ = -1;
  firstPaint_ = true;
  prefetchedBatch_ = -1;
  externalCacheBegin_ = -1;
  externalCacheCount_ = 0;
  externalCacheTitles_.clear();
  externalCachePresent_.clear();
  finished_ = false;
  cancelled_ = true;
  selectedIndex_ = -1;

  if (useExternal_ && chapternum >= chapterCount()) {
    chapternum = chapterCount() > 0 ? chapterCount() - 1 : 0;
  }
  if (chapternum < 0) chapternum = 0;

  page = chapternum / getPageItems() + 1;
  selectorIndex = chapternum;
  if (selectorIndex < 0) selectorIndex = (page - 1) * getPageItems();

  updateRequired = true;
  // 8KB: materialize + scaled CJK draw + optional batch parse need more than 4KB.
  xTaskCreate(&TxtReaderChapterSelectionActivity::taskTrampoline, "TxtReaderChapterSelectionActivityTask",
              8192, this, 1, &displayTaskHandle);
}

void TxtReaderChapterSelectionActivity::onExit() {
  Activity::onExit();

  // Stop display task work first. The task owns temporary vectors and may be
  // inside SD/render code; deleting it from here can leave a C++ object or
  // heap allocation half-destroyed. Let the task observe finished_ and delete
  // itself after it has left all critical sections.
  finished_ = true;
  updateRequired = false;
  Serial.printf("[ChapterSel] onExit begin busy=%d\n", displayBusy_ ? 1 : 0);

  // displayTaskLoop() clears displayTaskHandle immediately before its
  // self-delete. Do not use a timeout here: destroying renderingMutex or the
  // activity while the task is still alive is worse than waiting for one
  // in-flight eink/SD operation to finish.
  while (displayTaskHandle) {
    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
  if (renderingMutex) {
    vSemaphoreDelete(renderingMutex);
    renderingMutex = nullptr;
  }
  Serial.printf("[ChapterSel] onExit done\n");
}

void TxtReaderChapterSelectionActivity::loop() {
  const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);

  const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;
  const int pageItems = getPageItems();
  const int total = pageItems;

  auto finishBack = [&]() {
    if (finished_) return;
    finished_ = true;
    cancelled_ = true;
    selectedIndex_ = -1;
    updateRequired = false;  // do not start another paint under exit
    onGoBack();
  };
  auto finishSelect = [&](int idx) {
    if (finished_) return;
    finished_ = true;
    cancelled_ = false;
    selectedIndex_ = idx;
    updateRequired = false;  // critical: avoid paint+exit mutex deadlock
    onSelectchapter(idx);
  };

  if (mappedInput.hasTouch()) {
    const int BASE_Y_CHAPTER = M4TouchListMetrics::chapterListTop(true);
    const int FIX_LINE_HEIGHT = M4TouchListMetrics::chapterLineHeight(true);
    const int pagebegin = (page - 1) * pageItems;
    const int pagerTop = M4TouchListMetrics::chapterPagerTop(renderer.getScreenHeight(), true);
    const int pagerHeight = M4TouchListMetrics::chapterPagerButtonHeight(true);
    const int pagerWidth = M4TouchListMetrics::chapterPagerButtonWidth(renderer.getScreenWidth(), true);
    const int pagerRightX = M4TouchListMetrics::chapterPagerRightX(renderer.getScreenWidth(), true);

    auto activateSelection = [&]() {
      finishSelect(selectorIndex);
    };

    if (mappedInput.wasBackGesture()) {
      finishBack();
      return;
    }
    const auto sw = mappedInput.wasSwipe();
    if (sw == MappedInputManager::SwipeDir::Up || sw == MappedInputManager::SwipeDir::Left) {
      page += 1;
      if (page > maxPage()) page = maxPage();
      selectorIndex = page * total - 1;
      if (useExternal_ && selectorIndex >= chapterCount()) {
        selectorIndex = chapterCount() > 0 ? chapterCount() - 1 : 0;
      }
      updateRequired = true;
      return;
    }
    if (sw == MappedInputManager::SwipeDir::Down || sw == MappedInputManager::SwipeDir::Right) {
      page -= 1;
      if (page < 1) page = 1;
      selectorIndex = (page - 1) * total;
      updateRequired = true;
      return;
    }

    int tx = 0, ty = 0;
    const bool down = mappedInput.wasScreenTouchDown(tx, ty);
    const bool tap = mappedInput.wasScreenTapped(tx, ty);
    if (down || tap) {
      // Explicit footer buttons are the reliable paging path on eink touch
      // panels; swipes remain supported but are not required for navigation.
      if (ty >= pagerTop && ty < pagerTop + pagerHeight) {
        if (tap && tx >= M4TouchListMetrics::chapterPagerLeftX(true) &&
            tx < M4TouchListMetrics::chapterPagerLeftX(true) + pagerWidth) {
          if (page > 1) {
            --page;
            selectorIndex = (page - 1) * total;
            updateRequired = true;
          }
        } else if (tap && tx >= pagerRightX && tx < pagerRightX + pagerWidth) {
          if (page < maxPage()) {
            ++page;
            selectorIndex = page * total - 1;
            if (useExternal_ && selectorIndex >= chapterCount()) {
              selectorIndex = chapterCount() > 0 ? chapterCount() - 1 : 0;
            }
            updateRequired = true;
          }
        }
        return;
      }
      if (ty >= BASE_Y_CHAPTER) {
        const int row = (ty - BASE_Y_CHAPTER) / FIX_LINE_HEIGHT;
        if (row >= 0 && row < pageItems) {
          const int chapterIdx = pagebegin + row;
          if (ensureChapterBatch(chapterIdx)) {
            if (selectorIndex != chapterIdx) {
              selectorIndex = chapterIdx;
              updateRequired = true;
            }
            if (tap) activateSelection();
          }
        }
        return;
      }
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectorIndex == ITEM_SKIP_100_BACK) {
      skipChapters(-100);
    } else if (selectorIndex == ITEM_SKIP_100_FORWARD) {
      skipChapters(+100);
    } else {
      finishSelect(selectorIndex);
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finishBack();
  } else if (prevReleased) {
    bool isUpKey = mappedInput.wasReleased(MappedInputManager::Button::Up);
    if (skipPage || isUpKey) {
      page -= 1;
      if (page < 1) page = 1;
      selectorIndex = (page - 1) * total;
    } else {
      if (selectorIndex == (page - 1) * total) {
        selectorIndex = ITEM_SKIP_100_FORWARD;
      } else if (selectorIndex == ITEM_SKIP_100_FORWARD) {
        selectorIndex = ITEM_SKIP_100_BACK;
      } else if (selectorIndex == ITEM_SKIP_100_BACK) {
        selectorIndex = page * total - 1;
      } else {
        selectorIndex = (selectorIndex + total - 1) % total + (page - 1) * total;
      }
    }
    updateRequired = true;
  } else if (nextReleased) {
    bool isDownKey = mappedInput.wasReleased(MappedInputManager::Button::Down);
    if (skipPage || isDownKey) {
      page += 1;
      if (page > maxPage()) page = maxPage();
      selectorIndex = page * total - 1;
      if (useExternal_ && selectorIndex >= chapterCount()) {
        selectorIndex = chapterCount() > 0 ? chapterCount() - 1 : 0;
      }
    } else {
      if (selectorIndex == ITEM_SKIP_100_BACK) {
        selectorIndex = ITEM_SKIP_100_FORWARD;
      } else if (selectorIndex == ITEM_SKIP_100_FORWARD) {
        selectorIndex = (page - 1) * total;
      } else if (selectorIndex == page * total - 1) {
        selectorIndex = ITEM_SKIP_100_BACK;
      } else {
        selectorIndex = (selectorIndex + 1) % total + (page - 1) * total;
      }
    }
    updateRequired = true;
  }
}

void TxtReaderChapterSelectionActivity::displayTaskLoop() {
  while (true) {
    // After select/back, leave all temporary objects and critical sections
    // normally, then terminate from the task itself. The owner waits for the
    // handle to clear before destroying the activity/mutex.
    if (finished_) {
      displayBusy_ = false;
      displayTaskHandle = nullptr;
      vTaskDelete(nullptr);
      for (;;) vTaskDelay(portMAX_DELAY);
    }
    if (updateRequired) {
      updateRequired = false;
      if (finished_) continue;
      displayBusy_ = true;

      const int pageItems = getPageItems();
      if (page > maxPage()) page = maxPage();
      if (page < 1) page = 1;
      const int pagebegin = (page - 1) * pageItems;

      // Phase 1: cache-only materialize (no multi-second scan on paint path).
      std::vector<std::string> pageTitles;
      std::vector<uint8_t> pagePresent;
      materializePageTitles(pagebegin, pageItems, pageTitles, pagePresent);
      if (finished_) {
        displayBusy_ = false;
        continue;
      }

      // Phase 2: draw into frame buffer under brief mutex (no e-ink wait).
      if (renderingMutex) {
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        if (!finished_) {
          drawScreen(pageTitles, pagePresent, pagebegin, pageItems);
        }
        xSemaphoreGive(renderingMutex);
      } else if (!finished_) {
        drawScreen(pageTitles, pagePresent, pagebegin, pageItems);
      }
      if (finished_) {
        displayBusy_ = false;
        continue;
      }

      // Phase 3: e-ink refresh outside mutex so onExit can proceed.
      if (firstPaint_) {
        firstPaint_ = false;
        renderer.displayBuffer(useExternal_ ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
      } else {
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      }
      displayBusy_ = false;
    } else if (!useExternal_ && txt && loadedBatchStart_ >= 0 && !finished_) {
      // Idle: rebuild purged batches + prefetch next (may scan seconds).
      displayBusy_ = true;
      if (renderingMutex) {
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        if (!finished_) {
          prefetchNextBatchQuiet();
        }
        xSemaphoreGive(renderingMutex);
      }
      displayBusy_ = false;
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void TxtReaderChapterSelectionActivity::drawScreen(const std::vector<std::string>& pageTitles,
                                                   const std::vector<uint8_t>& pagePresent,
                                                   int pagebegin, int pageItems) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const char* header = headerTitle_.empty() ? "目  录" : headerTitle_.c_str();
  const auto metrics = UITheme::getInstance().getMetrics();
  const bool touch = mappedInput.hasTouch();
  const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int footerReserve = touch ? M4TouchListMetrics::chapterFooterReserve(true)
                                  : metrics.buttonHintsHeight + metrics.verticalSpacing;
  const int listHeight = std::max(1, pageHeight - listTop - footerReserve);
  const int themePageItems = std::max(1, listHeight / std::max(1, metrics.listRowHeight));
  const int listPageBegin = (selectorIndex / themePageItems) * themePageItems;
  int drawCount = chapterCount();
  if (!useExternal_) {
    // Library TXT discovers its end lazily. Draw only rows materialized for
    // this page instead of passing the GUI theme a fake 100000-item count.
    drawCount = pagebegin;
    for (int row = 0; row < pageItems && row < static_cast<int>(pagePresent.size()); ++row) {
      if (pagePresent[static_cast<size_t>(row)]) drawCount = pagebegin + row + 1;
    }
  }

  // Use the production theme components for the chrome and selected-row
  // treatment. This removes the old hand-drawn chip/box layout and keeps the
  // picker visually aligned with library, settings, and reader menus.
  const int themeSelected = (selectorIndex >= 0 && selectorIndex < drawCount) ? selectorIndex : -1;
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, header);
  GUI.drawList(
      renderer, Rect{0, listTop, pageWidth, listHeight}, drawCount, themeSelected,
      [&](int index) {
        const int row = index - listPageBegin;
        if (row < 0 || row >= pageItems || row >= static_cast<int>(pageTitles.size()) ||
            row >= static_cast<int>(pagePresent.size()) || !pagePresent[static_cast<size_t>(row)]) {
          char fallback[32];
          snprintf(fallback, sizeof(fallback), "第%d章", index + 1);
          return std::string(fallback);
        }
        if (!pageTitles[static_cast<size_t>(row)].empty()) return pageTitles[static_cast<size_t>(row)];
        char fallback[32];
        snprintf(fallback, sizeof(fallback), "第%d章", index + 1);
        return std::string(fallback);
      },
      nullptr, nullptr, nullptr);

  char pageLabel[64];
  if (useExternal_) {
    const int totalPages = std::max(1, maxPage());
    snprintf(pageLabel, sizeof(pageLabel), "%d/%d", page, totalPages);
  } else {
    snprintf(pageLabel, sizeof(pageLabel), "%d", page);
  }
  if (touch) {
    const int pagerTop = M4TouchListMetrics::chapterPagerTop(pageHeight, true);
    const int pagerHeight = M4TouchListMetrics::chapterPagerButtonHeight(true);
    const int pagerWidth = M4TouchListMetrics::chapterPagerButtonWidth(pageWidth, true);
    const int leftX = M4TouchListMetrics::chapterPagerLeftX(true);
    const int rightX = M4TouchListMetrics::chapterPagerRightX(pageWidth, true);
    const bool canPrev = page > 1;
    const bool canNext = page < maxPage();
    const auto drawPagerButton = [&](int x, const char* label, bool enabled) {
      renderer.fillRoundedRect(x, pagerTop, pagerWidth, pagerHeight, 8,
                               enabled ? Color::LightGray : Color::White);
      renderer.drawRoundedRect(x, pagerTop, pagerWidth, pagerHeight, 1, 8, true);
      M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, x, pagerTop, pagerWidth, pagerHeight, label, true);
    };
    drawPagerButton(leftX, "‹ 上一页", canPrev);
    drawPagerButton(rightX, "下一页 ›", canNext);
    M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, 0,
                                M4TouchListMetrics::chapterPagerLabelTop(pageHeight, true), pageWidth,
                                M4TouchListMetrics::chapterPagerLabelHeight(true), pageLabel, true,
                                EpdFontFamily::REGULAR, 8);
  } else {
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, pageHeight - footerReserve - 4, pageLabel, true);
    const auto labels = mappedInput.mapLabels("« 返回", "选择", "向前100", "向后100");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
}
