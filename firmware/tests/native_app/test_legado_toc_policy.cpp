// Host/source-contract tests for the Legado TOC consistency policy
// (M4LegadoTocPolicy.h — pure helpers, no SD / network).
//
// Audit findings pinned here:
//   1. registered/persisted chapterCount > actual toc_rows.txt rows is
//      detected and clamped (hollow blank entries must not render);
//   2. an empty/stale Legado getChapterList outcome (200 {"data":[]}, 404,
//      json_path_not_found with 0 records) classifies as a stale shelf and
//      must not be presented as silent Ready-with-placeholders;
//   3. a stale shelf totalHint never placeholder-overwrites a cached TOC
//      that already holds readable rows.
#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>

#include "apps/providers/M4LegadoTocPolicy.h"

using namespace M4LegadoTocPolicy;

namespace {

void level1_clamp_count_to_rows() {
  // Registered count larger than actual rows → clamp to readable rows.
  assert(clampedChapterCount(2968, 1200) == 1200);
  assert(clampedChapterCount(65, 64) == 64);

  // Consistent or smaller-than-registered counts pass through unchanged.
  assert(clampedChapterCount(100, 100) == 100);
  assert(clampedChapterCount(50, 100) == 50);

  // Zero readable rows forces re-bootstrap (0), never a hollow full count.
  assert(clampedChapterCount(2968, 0) == 0);
  assert(clampedChapterCount(1, 0) == 0);

  // Divergence detection used by the manager gate (clamped != registered).
  assert(clampedChapterCount(2968, 1200) != 2968);
  assert(clampedChapterCount(100, 100) == 100);
  std::cout << "L1 clamp OK\n";
}

void level2_stale_shelf_classification() {
  // HTTP 200 transfer succeeded but zero records extracted → stale shelf,
  // regardless of whether the parser reported nothing ("catalog_empty"),
  // a missing path ("json_path_not_found"), or no error at all ({"data":[]}).
  assert(isStaleShelfFetch(true, "catalog_empty", 0));
  assert(isStaleShelfFetch(true, "json_path_not_found", 0));
  assert(isStaleShelfFetch(true, "", 0));

  // A real parser failure with zero records is NOT silently reclassified:
  // truncated/syntax errors stay actionable as data errors.
  assert(!isStaleShelfFetch(true, "json_truncated", 0));
  assert(!isStaleShelfFetch(true, "json_syntax", 0));

  // Any real row count means the catalog streamed fine — never stale.
  assert(!isStaleShelfFetch(true, "catalog_empty", 64));
  assert(!isStaleShelfFetch(true, "", 1));

  // Transport failures keep their network error identity; only 404 means the
  // locator vanished from the phone service (stale shelf).
  assert(isStaleShelfFetch(false, "http_404", 0));
  assert(!isStaleShelfFetch(false, "http_500", 0));
  assert(!isStaleShelfFetch(false, "http_request_failed", 0));
  assert(!isStaleShelfFetch(false, "wifi_not_connected", 0));
  assert(!isStaleShelfFetch(false, "", 0));

  // A transport-level outcome is classified on its own; the recordCount
  // argument exists for the 200-with-body path and must not flip 404 logic.
  std::cout << "L2 stale-shelf classification OK\n";
}

void level3_placeholder_guard() {
  // Fresh open with a known total: placeholders allowed.
  assert(mayWritePlaceholderSkeleton(2968, 0));
  assert(mayWritePlaceholderSkeleton(1, 0));

  // Cached TOC already holds readable rows: a (stale) hint must never
  // placeholder-overwrite them; refill goes through atomic full-stream only.
  assert(!mayWritePlaceholderSkeleton(2968, 64));
  assert(!mayWritePlaceholderSkeleton(2968, 1));

  // No hint, no cache: nothing to do either way.
  assert(!mayWritePlaceholderSkeleton(0, 0));
  std::cout << "L3 placeholder guard OK\n";
}

}  // namespace

int main() {
  level1_clamp_count_to_rows();
  level2_stale_shelf_classification();
  level3_placeholder_guard();
  std::cout << "legado toc policy tests passed\n";
  return 0;
}
