#include "util/M4TxtIndexPolicy.h"

#include <cassert>
#include <iostream>
#include <vector>

namespace {

// Model of continuePageIndex's failure branch: a generic loadPageAtOffset
// failure must leave the index incomplete (retryable) unless the cursor is at
// the exclusive range end (true EOF).
bool failureMarksComplete(bool loadOk, size_t cursor, size_t rangeEnd) {
  if (loadOk) return false;
  if (!M4TxtIndexPolicy::loadFailureIsChapterEnd(cursor, rangeEnd)) return false;
  return true;
}

void testResumeCatchupBounded() {
  using M4TxtIndexPolicy::kResumeCatchupMaxBytes;
  using M4TxtIndexPolicy::kResumeCatchupMaxPages;
  using M4TxtIndexPolicy::resumeCatchupNeeded;

  // Saved mid-page: one bounded slice is allowed.
  assert(resumeCatchupNeeded(/*indexComplete=*/false, /*currentPage=*/42, /*totalPages=*/3));
  // Prev-chapter sentinel (currentPage<0) and page-0 opens never wait:
  // first paint proceeds; the rest is discovered in background.
  assert(!resumeCatchupNeeded(false, -1, 0));
  assert(!resumeCatchupNeeded(false, 0, 1));
  // Index already covers the saved page.
  assert(!resumeCatchupNeeded(false, 2, 5));
  // Index already complete.
  assert(!resumeCatchupNeeded(true, 9000, 3));

  // The budget itself is bounded — this is the whole-range residual guard.
  assert(kResumeCatchupMaxPages <= 16);
  assert(kResumeCatchupMaxBytes <= 256 * 1024);

  // Simulate the old unbounded loop condition: it would have kept calling
  // while !indexComplete_ regardless of coverage. The new gate must stop once
  // totalPages exceeds currentPage even while incomplete.
  assert(!resumeCatchupNeeded(false, 4, 5));
}

void testLoadFailureIsNotEof() {
  // Failure before the range end → incomplete (deferred retry), not EOF.
  assert(!failureMarksComplete(false, 1024, 200000));
  // Cursor at/after the exclusive range end → real chapter end.
  assert(failureMarksComplete(false, 200000, 200000));
  assert(failureMarksComplete(false, 200001, 200000));
  // Successful loads never complete an index by themselves.
  assert(!failureMarksComplete(true, 1024, 200000));

  // Empty range edge: cursor==end==0 completes immediately.
  assert(M4TxtIndexPolicy::loadFailureIsChapterEnd(0, 0));
}

void testPickerSkipFallbackCacheOnly() {
  struct Model {
    std::vector<int> cachedBatches;   // batch starts with any known chapter
    std::vector<int> chaptersInBatch; // highest chapter index of the last load
    int loadedBatchStart = -1;
  };

  auto batchStartOf = [](int ch) { return ch < 0 ? 0 : (ch / 25) * 25; };
  auto loadBatch = [](void* ctx, int batchStartValue) -> bool {
    auto* m = static_cast<Model*>(ctx);
    for (int b : m->cachedBatches) {
      if (b == batchStartValue) {
        m->loadedBatchStart = batchStartValue;
        return true;
      }
    }
    return false;  // cache miss → defer, never scan
  };
  auto existsInRam = [](void* ctx, int chapter) -> bool {
    auto* m = static_cast<Model*>(ctx);
    if (chapter / 25 * 25 != m->loadedBatchStart) return false;
    const int top = m->chaptersInBatch[static_cast<size_t>(m->loadedBatchStart / 25)];
    return chapter >= 0 && chapter <= top;
  };

  Model m;
  m.cachedBatches = {0};
  m.chaptersInBatch.assign(4, -1);
  m.chaptersInBatch[0] = 24;  // batch 0 fully populated

  // Highest existing cached chapter below target wins.
  assert(M4TxtIndexPolicy::skipFallbackFromCachedBatches(30, 0, batchStartOf, loadBatch,
                                                         existsInRam, &m) == 24);
  assert(m.loadedBatchStart == 0);

  // Beyond all discovered caches: walk-down still answers with the highest
  // cached chapter below the target (batch 0 here) — never a forward scan.
  assert(M4TxtIndexPolicy::skipFallbackFromCachedBatches(500, 0, batchStartOf, loadBatch,
                                                         existsInRam, &m) == 24);
  assert(m.loadedBatchStart == 0);

  // Discovery advanced to batch 50 (highest chapter there: 51).
  m.cachedBatches.push_back(50);
  m.chaptersInBatch[2] = 51;
  assert(M4TxtIndexPolicy::skipFallbackFromCachedBatches(60, 0, batchStartOf, loadBatch,
                                                         existsInRam, &m) == 51);
  assert(M4TxtIndexPolicy::skipFallbackFromCachedBatches(40, 0, batchStartOf, loadBatch,
                                                         existsInRam, &m) == 24);

  // Partial batch: only chapter 25 exists in batch 25.
  m.cachedBatches.push_back(25);
  m.chaptersInBatch[1] = 25;
  assert(M4TxtIndexPolicy::skipFallbackFromCachedBatches(40, 0, batchStartOf, loadBatch,
                                                         existsInRam, &m) == 25);

  // Nothing cached at all → defer.
  Model empty;
  empty.chaptersInBatch.assign(1, -1);
  assert(M4TxtIndexPolicy::skipFallbackFromCachedBatches(10, 0, batchStartOf, loadBatch,
                                                         existsInRam, &empty) == -1);

  // Guard clauses.
  assert(M4TxtIndexPolicy::skipFallbackFromCachedBatches(-1, 0, batchStartOf, loadBatch,
                                                         existsInRam, &m) == -1);
}

void testLoadPageLogGate() {
  using M4TxtIndexPolicy::kLoadPageLogMinIntervalMs;
  using M4TxtIndexPolicy::kLoadPageLogSlowThresholdMs;

  M4TxtIndexPolicy::LoadPageLogGate gate;
  uint32_t skipped = 0;
  uint32_t logged = 0;

  // Hot indexing burst: fast pages are dropped after the first log.
  assert(gate.shouldLog(1000, 5));   // first fast page logs (baseline)
  for (uint32_t t = 1100; t < 2900; t += 10) {
    if (gate.shouldLog(t, 6)) {
      ++logged;
    } else {
      ++skipped;
    }
  }
  assert(logged == 0);               // suppressed inside the window
  assert(skipped > 100);             // hot path actually gated
  assert(gate.skippedFast == skipped);

  // Interval elapsed → next fast page logs again.
  assert(gate.shouldLog(3100, 4));

  // Slow page always logs and re-arms the window immediately.
  assert(gate.shouldLog(3200, kLoadPageLogSlowThresholdMs));
  assert(!gate.shouldLog(3250, 1));
  assert(gate.shouldLog(5200, 1));   // past min interval since 3200

  // A slow page resets skip accounting.
  assert(!gate.shouldLog(5210, 2));
  assert(gate.skippedFast == 1);
  assert(gate.shouldLog(5300, 999));
  assert(gate.skippedFast == 0);
}

void testAllowScanContracts() {
  // Pinned contract values consumed by picker ensure/skip and goToPercent.
  assert(!M4TxtIndexPolicy::kPickerBatchAllowScan);
  assert(!M4TxtIndexPolicy::kGoToPercentBatchAllowScan);
}

}  // namespace

int main() {
  testResumeCatchupBounded();
  testLoadFailureIsNotEof();
  testPickerSkipFallbackCacheOnly();
  testLoadPageLogGate();
  testAllowScanContracts();
  std::cout << "txt index policy tests passed\n";
  return 0;
}
