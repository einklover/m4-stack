#pragma once

// Pure state policy for host-owned inline/file-backed lists. This makes source
// transitions explicit: a file source is immutable from Lua row mutation APIs;
// listSetRows intentionally replaces it with an inline source.

#include "util/M4xUiListPolicy.h"

#include <cstddef>

namespace M4xUiList {

enum class SourceKind { InlineRows, FileRows };
enum class MutationResult { Applied, UnsupportedForFile };

struct DataModel {
  SourceKind kind = SourceKind::InlineRows;
  size_t inlineRowCount = 0;
  size_t fileRowCount = 0;
  int pageSize = 12;
  int page = 1;

  size_t rowCount() const { return kind == SourceKind::FileRows ? fileRowCount : inlineRowCount; }
  int totalPages() const { return M4xUiList::totalPages(rowCount(), pageSize); }

  void openInline(size_t rows, int requestedPageSize) {
    kind = SourceKind::InlineRows;
    inlineRowCount = rows;
    fileRowCount = 0;
    pageSize = effectivePageSize(requestedPageSize);
    page = 1;
  }

  void openFile(size_t rows, int requestedPageSize) {
    kind = SourceKind::FileRows;
    inlineRowCount = 0;
    fileRowCount = rows;
    pageSize = effectivePageSize(requestedPageSize);
    page = 1;
  }

  // listSetRows is a source replacement, not an in-place file mutation.
  void setInlineRows(size_t rows, bool keepFirstVisible = true) {
    const size_t first = keepFirstVisible ? rowStart(page, pageSize) : 0;
    kind = SourceKind::InlineRows;
    fileRowCount = 0;
    inlineRowCount = rows;
    page = keepFirstVisible ? pageForFirstRowIndex(first, pageSize) : 1;
    page = clampPage(page, totalPages());
  }

  MutationResult appendInlineRows(size_t rows) {
    if (kind == SourceKind::FileRows) return MutationResult::UnsupportedForFile;
    inlineRowCount += rows;
    page = clampPage(pageAfterAppend(page, rows, pageSize), totalPages());
    return MutationResult::Applied;
  }

  MutationResult prependInlineRows(size_t rows) {
    if (kind == SourceKind::FileRows) return MutationResult::UnsupportedForFile;
    inlineRowCount += rows;
    page = clampPage(pageAfterPrepend(page, rows, pageSize), totalPages());
    return MutationResult::Applied;
  }

  int setPage(int requested) {
    page = clampPage(requested, totalPages());
    return page;
  }
};

}  // namespace M4xUiList
