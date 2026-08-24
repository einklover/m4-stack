#pragma once

// Legado TOC consistency policy (pure, SD-free, host-testable).
//
// The Legado phone web service derives catalog totals from the local shelf
// (totalChapterNum) while /getChapterList reflects the phone's current parse
// state. When the shelf is stale, three divergences show up on M4:
//   1. registered/persisted chapterCount > rows actually in toc_rows.txt
//      (hollow blank TOC entries),
//   2. a stale shelf totalHint tries to placeholder-overwrite a good cached
//      TOC file,
//   3. getChapterList answers 200 with {"data":[]} (or 404 / json_path_not_
//      found) while cached detail/body remain available — a stale shelf, not
//      a transient network failure.
//
// Legado-only by contract: other providers must not route their error strings
// through isStaleShelfFetch.

#include <algorithm>
#include <cstddef>
#include <string>

namespace M4LegadoTocPolicy {

// Clamp a registered/persisted chapter count down to the number of rows the
// TOC file can actually serve. Consistent or larger-than-registered counts
// pass through unchanged; zero actual rows forces a re-bootstrap instead of a
// fully hollow TOC.
inline size_t clampedChapterCount(size_t registeredCount, size_t actualRows) {
  if (actualRows == 0) return 0;
  return std::min(registeredCount, actualRows);
}

// True when a finished Legado catalog fetch points at a stale phone shelf:
//   - HTTP 200 transfer with zero extracted records ({"data":[]} — the
//     shelf no longer parses this book), reported as "", "catalog_empty" or
//     "json_path_not_found";
//   - HTTP 404 — the locator is gone from the phone service.
inline bool isStaleShelfFetch(bool transferOk, const std::string& error, size_t recordCount) {
  if (!transferOk) return error == "http_404";
  if (recordCount != 0) return false;
  return error.empty() || error == "catalog_empty" || error == "json_path_not_found";
}

// A stale shelf totalHint must never placeholder-overwrite a TOC file that
// already holds readable rows. Placeholders are only for the fresh-open path;
// an existing cache is seeded directly and refilled atomically on success.
inline bool mayWritePlaceholderSkeleton(size_t totalHint, size_t existingRows) {
  return totalHint > 0 && existingRows == 0;
}

}  // namespace M4LegadoTocPolicy
