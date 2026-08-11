#pragma once

// Progressive page-offset index for a single whole-file TXT chapter.
// Host-testable: layout is injected via a callback (no renderer/SD deps).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace M4ProgressiveTxtIndex {

// Lay out one page starting at `from` (file-absolute byte).
// Returns the exclusive end of this page (= start of next page, or fileSize at EOF).
// Must return > from when from < fileSize (forward progress).
using LayoutPageFn = size_t (*)(void* ctx, size_t from, size_t fileSize);

struct State {
  size_t fileSize = 0;
  std::vector<size_t> pageOffsets;  // start of each known page
  bool complete = false;
  uint32_t generation = 0;
  uint32_t layoutFp = 0;

  // Bytes fully covered by completed page layouts (end of last laid-out page).
  size_t indexedThrough = 0;

  void reset(size_t fs, uint32_t gen, uint32_t layout) {
    fileSize = fs;
    pageOffsets.clear();
    pageOffsets.push_back(0);
    indexedThrough = 0;
    complete = (fs == 0);
    generation = gen;
    layoutFp = layout;
  }

  int pageCount() const { return static_cast<int>(pageOffsets.size()); }
  bool hasPage(int page) const { return page >= 0 && page < pageCount(); }
  bool firstPageReady() const { return !pageOffsets.empty(); }

  // Provisional total for UI: known pages, or -1 if incomplete.
  int totalOrProvisional() const { return complete ? pageCount() : -1; }
};

struct StepBudget {
  int maxPages = 8;
  size_t maxBytes = 64 * 1024;
};

// Advance indexing by budget. Returns number of newly closed pages.
// Stale generation → no-op.
inline int step(State& s, uint32_t generation, LayoutPageFn layout, void* ctx, StepBudget budget) {
  if (generation != s.generation || s.complete || !layout) return 0;
  if (s.fileSize == 0) {
    s.complete = true;
    s.indexedThrough = 0;
    return 0;
  }
  if (s.pageOffsets.empty()) s.pageOffsets.push_back(0);

  int closed = 0;
  size_t bytes = 0;

  while (closed < budget.maxPages && bytes < budget.maxBytes) {
    if (generation != s.generation) return closed;
    if (s.indexedThrough >= s.fileSize) {
      s.complete = true;
      break;
    }
    // Layout the page that starts at indexedThrough (for page 0, indexedThrough is 0
    // and pageOffsets[0]==0; after closing page k, indexedThrough is start of page k+1
    // which we push when not EOF).
    const size_t pageStart = s.indexedThrough;
    // Ensure pageStart is recorded.
    if (s.pageOffsets.empty() || s.pageOffsets.back() != pageStart) {
      if (s.pageOffsets.empty() || pageStart > s.pageOffsets.back()) {
        s.pageOffsets.push_back(pageStart);
      }
    }

    size_t pageEnd = layout(ctx, pageStart, s.fileSize);
    if (pageEnd <= pageStart) pageEnd = pageStart + 1;
    if (pageEnd > s.fileSize) pageEnd = s.fileSize;
    bytes += (pageEnd - pageStart);
    ++closed;

    s.indexedThrough = pageEnd;
    if (pageEnd >= s.fileSize) {
      s.complete = true;
      break;
    }
    // Next page starts at pageEnd.
    s.pageOffsets.push_back(pageEnd);
  }
  return closed;
}

// Sync catch-up until needPage exists or complete/cancel.
inline bool ensurePage(State& s, uint32_t generation, LayoutPageFn layout, void* ctx, int needPage,
                       int maxSyncPages = 128) {
  if (needPage < 0) return false;
  StepBudget b;
  b.maxPages = 4;
  b.maxBytes = 128 * 1024;
  int guard = 0;
  while (!s.hasPage(needPage) && !s.complete && generation == s.generation && guard++ < maxSyncPages) {
    if (step(s, generation, layout, ctx, b) == 0) break;
  }
  return s.hasPage(needPage);
}

// --- First-page adaptive window (production: buildPageIndexFirstPage + loadPageAtOffset maxRead) ---
// Chapter file is finalized and fixed-length. First physical frame only needs page 0:
// start with a bounded read window; layout must actually *read* that many bytes
// (production maxReadBytes). If the page is incomplete at the window wall, grow by
// a bounded step (never past maxBytes) and retry. Never jump to whole fileSize to
// "force" progress. After page 0 closes, remaining pages use step() in background.
struct FirstPageWindowPolicy {
  size_t initialBytes = 8 * 1024;
  size_t stepBytes = 8 * 1024;
  size_t maxBytes = 48 * 1024;  // hard cap: single buffer alloc on M4 heap (no PSRAM required)
};

// True when first-page layout may stop expanding the window.
// - linesFilled >= linesPerPage: full page
// - nextOffset >= fileSize: EOF
// - nextOffset < windowEnd: natural page end before the wall
// False when nextOffset == windowEnd with partial page and more file remains (need grow).
// NOTE: nextOffset == windowEnd alone is NOT complete (that was the c42dd7d bug).
inline bool firstPageLayoutComplete(int linesFilled, int linesPerPage, size_t nextOffset, size_t windowEnd,
                                    size_t fileSize) {
  if (linesPerPage < 1) linesPerPage = 1;
  if (linesFilled >= linesPerPage) return true;
  if (fileSize == 0 || nextOffset >= fileSize) return true;
  if (nextOffset < windowEnd) return true;  // natural end mid-window
  // Hit the wall with incomplete page → must grow (if policy allows).
  return false;
}

// Grow exclusive window end. Never exceeds pageBegin+maxBytes or fileSize.
// If already at cap (and cap < fileSize), returns windowEnd unchanged — caller must
// stop (partial first page) rather than leap to whole-file size.
inline size_t growFirstPageWindow(size_t pageBegin, size_t windowEnd, size_t fileSize,
                                  const FirstPageWindowPolicy& pol) {
  if (windowEnd >= fileSize) return windowEnd;
  const size_t cap = pageBegin + pol.maxBytes;
  if (windowEnd >= cap) return windowEnd;  // at maxBytes — do not jump to fileSize
  size_t next = windowEnd + pol.stepBytes;
  if (next < windowEnd) return windowEnd;  // overflow
  if (next > cap) next = cap;
  if (next > fileSize) next = fileSize;
  return next;
}

// Production-equivalent adapter result.
struct FirstPageIndexResult {
  size_t page0End = 0;           // exclusive end of page 0 / start of page 1
  int expansions = 0;
  size_t lastReadBytes = 0;      // bytes the layout was allowed to read on last attempt
  bool hitCapIncomplete = false; // stopped at maxBytes without full page / EOF
};

// layout(ctx, from, exclusiveReadEnd): must only consume bytes in [from, exclusiveReadEnd).
// exclusiveReadEnd is the real read window (same as production maxReadBytes from from).
// Returns exclusive end of the laid-out page (or exclusiveReadEnd if incomplete at wall).
using WindowedLayoutFn = size_t (*)(void* ctx, size_t from, size_t exclusiveReadEnd);

// Mirrors buildPageIndexFirstPage + loadPageAtOffset(maxRead = window size).
inline FirstPageIndexResult indexFirstPageGrowingRead(size_t fileSize, int linesPerPage, WindowedLayoutFn layout,
                                                      void* ctx, FirstPageWindowPolicy pol) {
  FirstPageIndexResult r;
  if (fileSize == 0 || !layout) return r;
  if (linesPerPage < 1) linesPerPage = 1;
  size_t winEnd = pol.initialBytes;
  if (winEnd > fileSize) winEnd = fileSize;
  for (int guard = 0; guard < 32; ++guard) {
    // Production: maxReadBytes = winEnd - begin (begin=0 here).
    r.lastReadBytes = winEnd;
    size_t next = layout(ctx, 0, winEnd);
    if (next > winEnd) next = winEnd;
    if (next == 0 && fileSize > 0) next = 1;

    // Full page if layout closed before wall, or filled line budget (ctx-specific).
    // Callers encode "full page" as next < winEnd OR next - 0 covers one page unit.
    // We also accept linesFilled via: natural end (next < winEnd) or EOF.
    const int linesFilled = (next < winEnd || next >= fileSize) ? linesPerPage : 0;
    if (firstPageLayoutComplete(linesFilled, linesPerPage, next, winEnd, fileSize)) {
      r.page0End = (next >= fileSize) ? fileSize : next;
      return r;
    }
    const size_t grown = growFirstPageWindow(0, winEnd, fileSize, pol);
    if (grown <= winEnd) {
      r.hitCapIncomplete = (winEnd < fileSize);
      r.page0End = next;
      return r;
    }
    // Real expansion: next attempt must read more bytes than lastReadBytes.
    winEnd = grown;
    ++r.expansions;
  }
  r.page0End = winEnd >= fileSize ? fileSize : winEnd;
  return r;
}

// Legacy name used by older tests — delegates to growing-read path with LayoutPageFn
// clamped to window (layout may ignore window; we clamp return).
inline size_t indexFirstPageAdaptive(size_t fileSize, int linesPerPage, LayoutPageFn layout, void* ctx,
                                     FirstPageWindowPolicy pol, int* outExpansions) {
  struct Box {
    LayoutPageFn fn;
    void* c;
  } box{layout, ctx};
  auto wrap = [](void* p, size_t from, size_t exclusiveReadEnd) -> size_t {
    auto* b = static_cast<Box*>(p);
    size_t e = b->fn(b->c, from, exclusiveReadEnd);
    if (e > exclusiveReadEnd) e = exclusiveReadEnd;
    return e;
  };
  const FirstPageIndexResult r = indexFirstPageGrowingRead(fileSize, linesPerPage, wrap, &box, pol);
  if (outExpansions) *outExpansions = r.expansions;
  return r.page0End;
}

}  // namespace M4ProgressiveTxtIndex
