#pragma once

// Bounded resolver for a single row in a provider file catalog. The caller
// owns the FileRowSource and therefore controls the storage boundary; this
// helper never allocates a catalog-sized vector and never performs network I/O.

#include "apps/M4FileRowSource.h"
#include "util/M4ContentProviderContract.h"

#include <string>

namespace M4ContentProviderCatalog {

inline bool fieldAt(const std::string& line, int field0, std::string& out) {
  out.clear();
  if (field0 < 0) return true;
  int current = 0;
  size_t start = 0;
  for (size_t i = 0; i <= line.size(); ++i) {
    if (i != line.size() && line[i] != '\t') continue;
    if (current == field0) {
      out.assign(line, start, i - start);
      return true;
    }
    ++current;
    start = i + 1;
  }
  return false;
}

// Native TOC only shows title text; plugins (jjwxc) store VIP as a side field.
// When vipField0 is set and the cell is non-zero / non-"false", prefix "VIP ".
inline void decorateTitleWithVip(const std::string& line, int vipField0, std::string& title) {
  if (vipField0 < 0 || title.empty()) return;
  std::string flag;
  if (!fieldAt(line, vipField0, flag) || flag.empty()) return;
  if (flag == "0" || flag == "false" || flag == "nil" || flag == "False") return;
  if (title.find("VIP") != std::string::npos) return;
  title.insert(0, "VIP ");
}

inline bool resolveRow(M4FileRows::FileRowSource& source,
                       const M4ContentProvider::ChapterCatalogSpec& catalog, int index0,
                       M4ContentProvider::ChapterMeta& out, std::string& errorOut,
                       std::string* rawLineOut = nullptr) {
  using namespace M4ContentProvider;
  out = {};
  errorOut.clear();
  if (rawLineOut) rawLineOut->clear();
  // Accept index within both registered count and on-disk row count. Meta
  // count can lag/lead the file briefly (TOC rewrite / incomplete .ok); a
  // hard equality check made next-chapter resolve always fail for those books.
  if (catalog.kind != ChapterCatalogKind::FileRows || index0 < 0 || !source.isOpen()) {
    errorOut = "bad_catalog";
    return false;
  }
  if (static_cast<size_t>(index0) >= catalog.chapterCount ||
      static_cast<size_t>(index0) >= source.rowCount()) {
    errorOut = "row_out_of_range";
    return false;
  }
  const int page = index0 / source.pageSize() + 1;
  const M4FileRows::PageResult result = source.readPage(page);
  if (!result || result.rows.empty()) {
    errorOut = M4FileRows::errorKey(result.error);
    return false;
  }
  const M4FileRows::Row* row = nullptr;
  for (const M4FileRows::Row& candidate : result.rows) {
    if (candidate.index0 == static_cast<size_t>(index0)) {
      row = &candidate;
      break;
    }
  }
  if (!row || !fieldAt(row->line, catalog.uidField0, out.uid) || out.uid.empty()) {
    out = {};
    errorOut = "empty_uid";
    return false;
  }
  if (!idOk(out.uid.c_str(), kMaxChapterUidLen)) {
    out = {};
    errorOut = "bad_uid";
    return false;
  }
  if (catalog.titleField0 >= 0 && !fieldAt(row->line, catalog.titleField0, out.title)) {
    out.title.clear();
  }
  if (out.title.size() > kMaxTitleLen) {
    out = {};
    errorOut = "bad_title";
    return false;
  }
  if (rawLineOut) *rawLineOut = row->line;
  return true;
}

}  // namespace M4ContentProviderCatalog
