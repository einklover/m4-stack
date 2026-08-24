#pragma once

// Large-TXT reader residuals: pure contracts for the fixes audited on the
// streaming-first-open path. Host-testable (no renderer / SD / FreeRTOS deps).
//
// Scope (see agent/fix-large-txt-residuals):
//   1) Bounded pre-paint resume catch-up in chapter_initializeReader.
//   2) A generic page-load failure during progressive indexing is NOT EOF.
//   3) Picker skip/ensure and goToPercent resolve chapters cache-only; any
//      missing batch is discovered later by idle/display-task paths.
//   4) logPerf("loadPage") SD writes are rate-limited on hot indexing paths
//      while slow-page evidence stays unconditional.

#include <cstddef>
#include <cstdint>

namespace M4TxtIndexPolicy {

// --- 1) Saved mid-page resume catch-up --------------------------------------
// chapter_initializeReader may run ONE bounded continuePageIndex slice so a
// saved page number usually exists before first paint. The former unbounded
// while(!indexComplete_) loops (whole-range for currentPage<0 prev-chapter
// opens, grow-until-covered for saved pages) serialized the entire chapter
// index under the render lock. Anything past this budget defers to the
// renderScreen / displayTaskLoop progressive index + pending-restore path.
constexpr int kResumeCatchupMaxPages = 8;
constexpr size_t kResumeCatchupMaxBytes = 256 * 1024;

inline bool resumeCatchupNeeded(bool indexComplete, int currentPage, int totalPages) {
  // Page 0 / prev-chapter sentinel (currentPage < 0) never waits: first paint
  // proceeds immediately and later pages arrive via background discovery.
  return !indexComplete && currentPage > 0 && totalPages <= currentPage;
}

// --- 2) Page-load failure vs chapter EOF ------------------------------------
// continuePageIndex treats cursor >= rangeEnd as completion. A loadPageAtOffset
// failure at any earlier cursor is a device/allocation/decode problem: the
// index must stay incomplete so the slice can resume later and truncated
// offsets are never persisted (.tidx / chapterN.bin) as complete indexes.
inline bool loadFailureIsChapterEnd(size_t cursor, size_t rangeEnd) {
  return cursor >= rangeEnd;
}

// --- 3) UI-thread chapter resolution is cache-only --------------------------
// Contract values consumed indirectly by call sites (kept here so the host
// test pins them; a future flip must be a deliberate contract change).
constexpr bool kPickerBatchAllowScan = false;
constexpr bool kGoToPercentBatchAllowScan = false;

// Model of the picker skip fallback: walk BATCH STARTS downward from the
// target and answer with the highest existing chapter inside the first cached
// batch. Two separate probes keep units straight:
//   - loadBatchFromCache(ctx, batchStart): cache/RAM load of one 25-row batch
//     (false ⇒ not discoverable without a scan → caller defers).
//   - chapterExistsInRam(ctx, chapter): TXT::isChapterExist AFTER a successful
//     batch load. Never called with a batch start.
// A negative result means "defer discovery", never "scan".
inline int skipFallbackFromCachedBatches(int target, int minBatch,
                                         int (*batchStartOf)(int chapter),
                                         bool (*loadBatchFromCache)(void* ctx, int batchStart),
                                         bool (*chapterExistsInRam)(void* ctx, int chapter),
                                         void* ctx) {
  if (!batchStartOf || !loadBatchFromCache || !chapterExistsInRam || target < 0) return -1;
  constexpr int kBatch = 25;
  int batch = batchStartOf(target);
  while (batch >= minBatch) {
    if (loadBatchFromCache(ctx, batch)) {
      for (int i = kBatch - 1; i >= 0; --i) {
        if (chapterExistsInRam(ctx, batch + i)) return batch + i;
      }
    }
    if (batch == minBatch) break;
    batch -= kBatch;
    if (batch < minBatch) batch = minBatch;
  }
  return -1;
}

// --- 4) loadPage perf-log rate gate -----------------------------------------
// Hot indexing calls loadPageAtOffset hundreds of times; an unconditional SD
// append per call stalls the very path it measures. Slow pages are always
// logged; fast pages are logged at most once per interval.
constexpr uint32_t kLoadPageLogMinIntervalMs = 2000;
constexpr uint32_t kLoadPageLogSlowThresholdMs = 150;

struct LoadPageLogGate {
  uint32_t lastLogMs = 0;
  uint32_t skippedFast = 0;

  bool shouldLog(uint32_t nowMs, uint32_t elapsedMs) {
    if (elapsedMs >= kLoadPageLogSlowThresholdMs) {
      lastLogMs = nowMs;
      skippedFast = 0;
      return true;
    }
    if (lastLogMs != 0 &&
        static_cast<int32_t>(nowMs - lastLogMs) < static_cast<int32_t>(kLoadPageLogMinIntervalMs)) {
      ++skippedFast;
      return false;
    }
    lastLogMs = nowMs;
    skippedFast = 0;
    return true;
  }
};

}  // namespace M4TxtIndexPolicy
