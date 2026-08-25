#pragma once

#include "apps/providers/M4NovelProviderContract.h"

#include <cstddef>
#include <functional>
#include <string>

namespace M4NativeProviderBookDetail {

// Provider-neutral input for the ruleBookInfo stage. title/author are seeds
// already available from discovery/history and remain valid fallbacks when a
// remote detail endpoint omits optional metadata.
struct Request {
  std::string providerId;
  std::string appId;
  std::string bookId;
  std::string title;
  std::string author;
  std::string coverUrl;
  size_t maxBytes = 96u * 1024u;
};

struct Result {
  bool ok = false;
  M4NovelProvider::BookDetail detail;
  size_t receivedBytes = 0;
  std::string error;
  // True when the result came from local shelf/seed without network I/O.
  bool localOnly = false;
};

using CancelFn = std::function<bool()>;

// Synchronous bounded metadata fetch. For Legado this is local-only (shelf
// row + seed) and must never call ensureEndpoint or re-fetch /getBookshelf.
Result fetch(const Request& req, const CancelFn& cancelled = {});

// Build the normalized fallback model without network I/O.
M4NovelProvider::BookDetail seed(const Request& req);

namespace detail {
inline std::string boundedUtf8Field(std::string s, size_t maxBytes) {
  if (s.size() <= maxBytes) return s;
  s.resize(maxBytes);
  while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0u) == 0x80u) s.pop_back();
  return s;
}
}  // namespace detail

// Host-testable shelf-row parser. Legado shelf_rows.tsv columns:
//   id \t name \t author \t totalChapterNum [\t latestChapterTitle]
// Returns true when the line matches bookId and yields at least a title.
inline bool applyShelfRow(const std::string& line, const std::string& bookId,
                          M4NovelProvider::BookDetail& book) {
  constexpr size_t kFieldMax = 192;
  if (bookId.empty() || line.rfind(bookId, 0) != 0) return false;
  if (line.size() <= bookId.size() || line[bookId.size()] != '\t') return false;

  std::string fields[5];
  size_t field = 0;
  size_t start = 0;
  for (size_t i = 0; i <= line.size() && field < 5; ++i) {
    if (i == line.size() || line[i] == '\t') {
      fields[field] = line.substr(start, i - start);
      ++field;
      start = i + 1;
    }
  }
  if (fields[0] != bookId) return false;

  if (!fields[1].empty()) book.title = detail::boundedUtf8Field(std::move(fields[1]), kFieldMax);
  if (!fields[2].empty()) book.author = detail::boundedUtf8Field(std::move(fields[2]), kFieldMax);
  // fields[3] = totalChapterNum (catalog hint); not shown on detail.
  if (field >= 5 && !fields[4].empty()) {
    book.lastChapter = detail::boundedUtf8Field(std::move(fields[4]), kFieldMax);
  }
  return !book.title.empty();
}

// True when Legado detail can be served without re-fetching /getBookshelf.
inline bool legadoLocalDetailSufficient(const M4NovelProvider::BookDetail& book) {
  return !book.title.empty();
}

}  // namespace M4NativeProviderBookDetail
