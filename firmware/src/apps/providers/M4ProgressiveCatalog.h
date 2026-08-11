#pragma once

// Shared progressive / windowed catalog policy for every native provider
// (legado / fanqie / jjwxc / weread). Pure helpers — no SD / network.
//
// Goals:
//   1. Open fast: first window of rows is enough to register and enter reader.
//   2. Segmented load: cancel HTTP once first window is full, then full refill.
//   3. Placeholders: when total chapter count is known, build a full index
//      skeleton (index \t 第N章) before titles stream in.
//
// FileRows TSV line format remains provider-defined (uid \t title \t …).

#include "apps/M4xJsonStream.h"
#include "util/M4ContentProviderContract.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>

namespace M4ProgressiveCatalog {

struct Policy {
  // Rows before first Ready (~2 TOC pages on 800x480).
  size_t firstWindow = 64;
  size_t hardCap = M4ContentProvider::kMaxCatalogChapters;
  size_t minUsefulRows = 1;
};

inline Policy defaultPolicy() { return {}; }

inline std::string placeholderTitle(size_t index0) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "第%u章", static_cast<unsigned>(index0 + 1));
  return buf;
}

// Default two-column FileRows line: index \t title \n
inline std::string indexTitleRow(size_t index0, const std::string& title) {
  char idx[24];
  std::snprintf(idx, sizeof(idx), "%u", static_cast<unsigned>(index0));
  std::string row = idx;
  row.push_back('\t');
  for (char c : title) {
    row.push_back((c == '\t' || c == '\r' || c == '\n') ? ' ' : c);
  }
  row.push_back('\n');
  return row;
}

// Build full placeholder catalog body in memory (host-testable).
inline std::string buildPlaceholderBody(size_t total, const Policy& policy = defaultPolicy()) {
  if (total == 0 || total > policy.hardCap) return {};
  std::string body;
  body.reserve(total * 12);
  for (size_t i = 0; i < total; ++i) {
    body += indexTitleRow(i, placeholderTitle(i));
  }
  return body;
}

// Sink wrapper: forwards every row, counts, trips when first window is full.
class FirstWindowSink final : public M4xJsonStream::Sink {
 public:
  FirstWindowSink(M4xJsonStream::Sink& inner, size_t firstWindow)
      : inner_(inner), firstWindow_(std::max<size_t>(1, firstWindow)) {}

  bool write(const uint8_t* data, size_t len) override {
    if (!inner_.write(data, len)) return false;
    ++count_;
    if (count_ >= firstWindow_) windowReady_ = true;
    return true;
  }

  size_t count() const { return count_; }
  bool windowReady() const { return windowReady_; }

 private:
  M4xJsonStream::Sink& inner_;
  size_t firstWindow_ = 64;
  size_t count_ = 0;
  bool windowReady_ = false;
};

inline std::function<bool()> windowCancel(const FirstWindowSink& window,
                                          const std::function<bool()>& userCancel) {
  return [&window, userCancel]() {
    if (userCancel && userCancel()) return true;
    return window.windowReady();
  };
}

inline bool partialOk(size_t rows, bool windowReady, const Policy& policy = defaultPolicy()) {
  return rows >= policy.minUsefulRows && (windowReady || rows > 0);
}

// Decide open strategy from known total + first window size.
enum class OpenStrategy : uint8_t {
  FullStream = 0,      // small catalogs / unknown total
  PlaceholderThenFull, // total known → skeleton first, then stream titles
  WindowThenFull,      // stream first window, Ready, then full refill
};

inline OpenStrategy chooseStrategy(size_t totalHint, size_t firstWindow,
                                   size_t hardCap = M4ContentProvider::kMaxCatalogChapters) {
  if (totalHint > 0 && totalHint <= hardCap) return OpenStrategy::PlaceholderThenFull;
  if (firstWindow > 0) return OpenStrategy::WindowThenFull;
  return OpenStrategy::FullStream;
}

}  // namespace M4ProgressiveCatalog
