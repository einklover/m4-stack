#pragma once

// Host-owned list scene policy: pagination math + input mapping shared by the
// M4xLuaHost "ui.list" module and the AppRuntime event router (host-testable,
// no UI / FreeRTOS). Plugins hand the host a row set; the host renders rows,
// page buttons and footer, and maps touches/keys to row/page actions.

#include <algorithm>
#include <cstddef>

namespace M4xUiList {

// 480x800 portrait geometry (same family as the Lua Layout.metrics()).
struct Geometry {
  int titleY = 16;
  int statusY = 64;
  int rowsTop = 96;
  int rowHeight = 58;
  int footerTop = 744;  // screenHeight - 56
  int footerY = 764;    // footer text baseline
  int pageBtnY = 746;   // button glyph baseline
  int leftBtnX = 40;
  int rightBtnX = 440;
  int rowsPerPage = 12;
};

inline const Geometry& geom() {
  static const Geometry g;
  return g;
}

// Max rows that fit above the footer band — the REAL per-screen capacity.
inline int rowsPerScreen() {
  const Geometry& g = geom();
  const int n = (g.footerTop - g.rowsTop) / g.rowHeight;
  return n > 0 ? n : 1;
}

// Effective page size: the plugin's requested page_size clamped to what the
// screen can actually show. All pagination/render math must use this.
inline int effectivePageSize(int pageSize) {
  if (pageSize < 1) pageSize = geom().rowsPerPage;
  const int cap = rowsPerScreen();
  return pageSize > cap ? cap : pageSize;
}

// Runtime-theme variant.  The legacy overload above remains for callers that
// do not own a renderer; host scenes use the active M4UiStyle visible-row
// count so drawing and pagination cannot drift when the panel/theme changes.
inline int effectivePageSize(int pageSize, int visibleRows) {
  if (visibleRows < 1) visibleRows = 1;
  if (pageSize < 1) pageSize = geom().rowsPerPage;
  return std::min(pageSize, visibleRows);
}

inline int totalPages(size_t n, int pageSize) {
  pageSize = effectivePageSize(pageSize);
  if (n == 0) return 1;
  return static_cast<int>((n + static_cast<size_t>(pageSize) - 1) / static_cast<size_t>(pageSize));
}

inline int totalPages(size_t n, int pageSize, int visibleRows) {
  pageSize = effectivePageSize(pageSize, visibleRows);
  if (n == 0) return 1;
  return static_cast<int>((n + static_cast<size_t>(pageSize) - 1) / static_cast<size_t>(pageSize));
}

inline int clampPage(int page, int total) {
  if (total < 1) total = 1;
  if (page < 1) return 1;
  if (page > total) return total;
  return page;
}

inline size_t rowStart(int page, int pageSize) {
  pageSize = effectivePageSize(pageSize);
  return static_cast<size_t>(std::max(0, page - 1)) * static_cast<size_t>(pageSize);
}

inline size_t rowStart(int page, int pageSize, int visibleRows) {
  pageSize = effectivePageSize(pageSize, visibleRows);
  return static_cast<size_t>(std::max(0, page - 1)) * static_cast<size_t>(pageSize);
}

// Visible row count on `page` for `n` rows.
inline size_t visibleCount(size_t n, int page, int pageSize) {
  pageSize = effectivePageSize(pageSize);
  const size_t start = rowStart(page, pageSize);
  if (start >= n) return 0;
  return std::min<size_t>(static_cast<size_t>(pageSize), n - start);
}

inline size_t visibleCount(size_t n, int page, int pageSize, int visibleRows) {
  pageSize = effectivePageSize(pageSize, visibleRows);
  const size_t start = rowStart(page, pageSize, visibleRows);
  if (start >= n) return 0;
  return std::min<size_t>(static_cast<size_t>(pageSize), n - start);
}

// 1-based row index for a touch y inside the rows band; 0 when outside.
inline int rowIndexForY(int y) {
  const Geometry& g = geom();
  if (y < g.rowsTop || y >= g.footerTop) return 0;
  const int i = (y - g.rowsTop) / g.rowHeight + 1;
  return i;
}

// Touch in the top band (title/status area) — conventionally "back".
inline bool inTitleBand(int y) { return y >= 0 && y < geom().rowsTop; }

inline bool inFooter(int y) { return y >= geom().footerTop; }

// Footer button action from x: -1 (prev), +1 (next), 0 (center / none).
inline int footerAction(int x, int w) {
  if (x < w / 2) return -1;
  return +1;
}

// Page after appending rows: the current page index keeps pointing at the same
// screenful (list only grows at the tail).
inline int pageAfterAppend(int page, size_t appended, int pageSize) {
  (void)appended;
  (void)pageSize;
  return std::max(1, page);
}

// Page after prepending rows: shift the window so the previously first
// screenful stays visible.
inline int pageAfterPrepend(int page, size_t prepended, int pageSize) {
  pageSize = effectivePageSize(pageSize);
  const int shift = static_cast<int>((prepended + static_cast<size_t>(pageSize) - 1) /
                                     static_cast<size_t>(pageSize));
  return std::max(1, page + shift);
}

// Clamp a page into [1,total] preserving the *index* of the first row when
// possible (used by listSetRows with a keep-index hint).
inline int pageForFirstRowIndex(size_t firstRowIndex, int pageSize) {
  pageSize = effectivePageSize(pageSize);
  return static_cast<int>(firstRowIndex / static_cast<size_t>(pageSize)) + 1;
}

}  // namespace M4xUiList
