#pragma once

// Load WeRead-style toc.json titles for the system chapter-selection UI.
// Format: { "chapters": [ { "title": "...", "chapterUid": "..." }, ... ] }
// Host-testable; no UI / FreeRTOS.

#include <ArduinoJson.h>
#include <SDCardManager.h>

#include "apps/M4ContentProviderCatalog.h"
#include "apps/M4FileRowSource.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace M4PluginTocList {

inline constexpr size_t kMaxChapters = 2000;
inline constexpr size_t kMaxTitleBytes = 128;
inline constexpr size_t kMaxTocFileBytes = 512 * 1024;

// A shared, bounded title source for the native system chapter list.  It
// keeps only the seekable file and reads the requested visible page.  The
// mutex matters because the picker paints on its display task while owner/UI
// code may request a title for a chapter switch at the same time.
struct PagedTitleSource {
  std::shared_ptr<M4FileRows::FileRowSource> rows;
  M4ContentProvider::ChapterCatalogSpec catalog;
  std::string absPath;  // for reopen-on-grow (live progressive TOC file)
  mutable std::mutex mutex;
  // Bumped when chapterCount grows so the picker can detect live refresh.
  std::atomic<uint32_t> generation{0};

  size_t rowCount() const {
    // catalog is authoritative; rows may lag one update briefly under lock.
    return catalog.chapterCount;
  }

  // Expand catalog as progressive loader appends rows.  Reopens the SD file so
  // the reader handle sees the new EOF, then raises known row count.
  bool growTo(size_t newCount) {
    using namespace M4ContentProvider;
    if (newCount == 0 || newCount > kMaxCatalogChapters) return false;
    std::lock_guard<std::mutex> lock(mutex);
    if (newCount <= catalog.chapterCount) return false;
    if (absPath.empty() || !rows) {
      catalog.chapterCount = newCount;
      generation.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    // Re-open seekable input so size() reflects appended bytes.
    class SdCatalogInput final : public M4FileRows::ISeekableInput {
     public:
      explicit SdCatalogInput(const char* path) {
        opened_ = path && SdMan.openFileForRead("M4xTocGrow", path, file_);
      }
      ~SdCatalogInput() override {
        if (opened_) file_.close();
      }
      bool opened() const { return opened_; }
      uint64_t size() const override { return opened_ ? file_.fileSize() : 0; }
      bool seek(uint64_t offset) override { return opened_ && file_.seek(offset); }
      int64_t read(uint8_t* dst, size_t capacity) override {
        if (!opened_) return -1;
        const int n = file_.read(dst, capacity);
        return n < 0 ? -1 : n;
      }

     private:
      FsFile file_;
      bool opened_ = false;
    };

    auto input = std::make_unique<SdCatalogInput>(absPath.c_str());
    if (!input->opened()) return false;
    M4FileRows::Limits limits;
    limits.maxFileBytes = 8u * 1024u * 1024u;
    limits.maxRows = kMaxCatalogChapters;
    limits.maxLineBytes = 2048;
    limits.maxPageSize = 32;
    limits.maxCursors = 12;
    const int pageSize = rows->pageSize() > 0 ? rows->pageSize() : 32;
    auto next = std::make_shared<M4FileRows::FileRowSource>();
    if (next->open(std::move(input), pageSize, limits, newCount) != M4FileRows::Error::None) {
      return false;
    }
    rows = std::move(next);
    catalog.chapterCount = newCount;
    generation.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  bool loadPage(int firstIndex, int count, std::vector<std::string>& titlesOut,
                std::vector<uint8_t>& presentOut) {
    titlesOut.assign(count > 0 ? static_cast<size_t>(count) : 0, {});
    presentOut.assign(count > 0 ? static_cast<size_t>(count) : 0, 0);
    if (!rows || firstIndex < 0 || count <= 0) return false;
    std::lock_guard<std::mutex> lock(mutex);
    if (!rows->isOpen()) return false;
    const int last = std::min<int>(static_cast<int>(catalog.chapterCount), firstIndex + count);
    if (firstIndex >= last) return false;

    // presentOut must reflect rows that were actually read from the file, not
    // the whole requested range: a registered chapterCount larger than
    // toc_rows.txt would otherwise render hollow blank entries as present.
    bool ok = true;
    const int firstPage = firstIndex / rows->pageSize() + 1;
    const int lastPage = (last - 1) / rows->pageSize() + 1;
    for (int page = firstPage; page <= lastPage; ++page) {
      const M4FileRows::PageResult result = rows->readPage(page);
      if (!result) {
        ok = false;
        continue;
      }
      for (const M4FileRows::Row& row : result.rows) {
        if (row.index0 < static_cast<size_t>(firstIndex) || row.index0 >= static_cast<size_t>(last)) continue;
        const int outIndex = static_cast<int>(row.index0) - firstIndex;
        std::string title;
        (void)M4ContentProviderCatalog::fieldAt(row.line, catalog.titleField0, title);
        M4ContentProviderCatalog::decorateTitleWithVip(row.line, catalog.vipField0, title);
        if (title.size() > kMaxTitleBytes) title.resize(kMaxTitleBytes);
        if (title.empty()) {
          char fallback[32];
          snprintf(fallback, sizeof(fallback), "第%u章", static_cast<unsigned>(row.index0 + 1));
          title = fallback;
        }
        titlesOut[static_cast<size_t>(outIndex)] = std::move(title);
        presentOut[static_cast<size_t>(outIndex)] = 1;
      }
    }
    return ok;
  }
};

// Live native TOC source while progressive loader is still appending rows.
inline std::shared_ptr<PagedTitleSource>& livePagedSource() {
  static std::shared_ptr<PagedTitleSource> s;
  return s;
}
inline void setLivePagedSource(std::shared_ptr<PagedTitleSource> src) { livePagedSource() = std::move(src); }
inline void clearLivePagedSource() { livePagedSource().reset(); }
// Grow the open TOC's catalog; returns true if the picker should repaint.
inline bool publishLiveCatalogRowCount(size_t rows) {
  auto src = livePagedSource();
  if (!src) return false;
  return src->growTo(rows);
}

// Open the catalog without scanning every row.  The registered chapter count
// is authoritative; FileRowSource still bounds file size/line size and each
// page read is independently checked.
inline std::shared_ptr<PagedTitleSource> openPagedFileRows(
    const std::string& absPath, const M4ContentProvider::ChapterCatalogSpec& catalog) {
  using namespace M4ContentProvider;
  if (catalog.kind != ChapterCatalogKind::FileRows || absPath.empty() || catalog.chapterCount == 0 ||
      catalog.chapterCount > kMaxCatalogChapters || catalog.uidField0 < 0 || catalog.uidField0 > 15 ||
      catalog.titleField0 < -1 || catalog.titleField0 > 15) {
    return {};
  }

  class SdCatalogInput final : public M4FileRows::ISeekableInput {
   public:
    explicit SdCatalogInput(const char* path) {
      opened_ = path && SdMan.openFileForRead("M4xTocPage", path, file_);
    }
    ~SdCatalogInput() override {
      if (opened_) file_.close();
    }
    bool opened() const { return opened_; }
    uint64_t size() const override { return opened_ ? file_.fileSize() : 0; }
    bool seek(uint64_t offset) override { return opened_ && file_.seek(offset); }
    int64_t read(uint8_t* dst, size_t capacity) override {
      if (!opened_) return -1;
      const int n = file_.read(dst, capacity);
      return n < 0 ? -1 : n;
    }

   private:
    FsFile file_;
    bool opened_ = false;
  };

  auto input = std::make_unique<SdCatalogInput>(absPath.c_str());
  if (!input->opened()) return {};
  M4FileRows::Limits limits;
  limits.maxFileBytes = 8u * 1024u * 1024u;
  limits.maxRows = kMaxCatalogChapters;
  limits.maxLineBytes = 2048;
  limits.maxPageSize = 32;
  limits.maxCursors = 12;
  auto rows = std::make_shared<M4FileRows::FileRowSource>();
  if (rows->open(std::move(input), 32, limits, catalog.chapterCount) != M4FileRows::Error::None) return {};

  auto source = std::make_shared<PagedTitleSource>();
  source->rows = std::move(rows);
  source->catalog = catalog;
  source->absPath = absPath;
  return source;
}

// Read a file-backed provider catalog into the native chapter picker.  The
// provider keeps the catalog file-backed in Lua; this bounded host-side copy
// is only the title vector required by TxtReaderChapterSelectionActivity.
// It is deliberately independent from JSON so large catalogs and plugins that
// use toc_rows.txt work exactly like inline toc.json providers.
inline bool loadTitlesFromFileRows(const std::string& absPath,
                                   const M4ContentProvider::ChapterCatalogSpec& catalog,
                                   std::vector<std::string>& titlesOut) {
  titlesOut.clear();
  using namespace M4ContentProvider;
  // The native picker stores one title per row in a std::vector.  Large
  // provider catalogs remain supported by host ui.listOpenFile, but must not
  // be copied into the e-ink picker in one allocation (that was a crash/oom
  // path when a source returned tens of thousands of chapters).
  if (catalog.kind != ChapterCatalogKind::FileRows || absPath.empty() || catalog.chapterCount == 0 ||
      catalog.chapterCount > kMaxChapters || catalog.chapterCount > kMaxCatalogChapters ||
      catalog.uidField0 < 0 || catalog.uidField0 > 15 ||
      catalog.titleField0 < -1 || catalog.titleField0 > 15) {
    return false;
  }

  class SdCatalogInput final : public M4FileRows::ISeekableInput {
   public:
    explicit SdCatalogInput(const char* path) {
      opened_ = path && SdMan.openFileForRead("M4xTocRows", path, file_);
    }
    ~SdCatalogInput() override {
      if (opened_) file_.close();
    }
    bool opened() const { return opened_; }
    uint64_t size() const override { return opened_ ? file_.fileSize() : 0; }
    bool seek(uint64_t offset) override { return opened_ && file_.seek(offset); }
    int64_t read(uint8_t* dst, size_t capacity) override {
      if (!opened_) return -1;
      const int n = file_.read(dst, capacity);
      return n < 0 ? -1 : n;
    }

   private:
    FsFile file_;
    bool opened_ = false;
  };

  auto input = std::make_unique<SdCatalogInput>(absPath.c_str());
  if (!input->opened()) return false;
  M4FileRows::Limits limits;
  limits.maxFileBytes = 8u * 1024u * 1024u;
  limits.maxRows = catalog.chapterCount;
  limits.maxLineBytes = 2048;
  limits.maxPageSize = 32;
  limits.maxCursors = 12;
  auto source = std::make_unique<M4FileRows::FileRowSource>();
  if (source->open(std::move(input), 32, limits) != M4FileRows::Error::None ||
      source->rowCount() != catalog.chapterCount) {
    return false;
  }

  titlesOut.reserve(catalog.chapterCount);
  for (int page = 1; page <= source->pageCount(); ++page) {
    const auto result = source->readPage(page);
    if (!result) {
      titlesOut.clear();
      return false;
    }
    for (const auto& row : result.rows) {
      std::string title;
      (void)M4ContentProviderCatalog::fieldAt(row.line, catalog.titleField0, title);
      M4ContentProviderCatalog::decorateTitleWithVip(row.line, catalog.vipField0, title);
      if (title.size() > kMaxTitleLen) title.resize(kMaxTitleLen);
      if (title.empty()) {
        char fallback[32];
        snprintf(fallback, sizeof(fallback), "Chapter %u",
                 static_cast<unsigned>(row.index0 + 1));
        title = fallback;
      }
      titlesOut.push_back(std::move(title));
    }
  }
  return titlesOut.size() == catalog.chapterCount;
}

// Returns true when at least one chapter title was loaded.
inline bool loadTitlesFromFile(const std::string& absPath, std::vector<std::string>& titlesOut) {
  titlesOut.clear();
  FsFile f;
  if (!SdMan.openFileForRead("M4xToc", absPath.c_str(), f)) return false;
  const size_t n = f.fileSize();
  if (n == 0 || n > kMaxTocFileBytes) {
    f.close();
    return false;
  }
  std::string raw;
  raw.resize(n);
  const size_t got = f.read(reinterpret_cast<uint8_t*>(&raw[0]), n);
  f.close();
  if (got != n) return false;

  // Cap JSON document heap (ESP32 internal RAM). Large TOCs still parse if under file cap.
  const size_t docCap = std::min(n + 8192, static_cast<size_t>(96 * 1024));
  DynamicJsonDocument doc(docCap);
  if (deserializeJson(doc, raw)) return false;
  JsonArray arr = doc["chapters"].as<JsonArray>();
  if (arr.isNull()) return false;

  titlesOut.reserve(std::min(static_cast<size_t>(arr.size()), kMaxChapters));
  for (JsonObject ch : arr) {
    if (titlesOut.size() >= kMaxChapters) break;
    const char* t = ch["title"] | "";
    std::string title = t ? t : "";
    if (title.size() > kMaxTitleBytes) title.resize(kMaxTitleBytes);
    if (title.empty()) {
      char fallback[32];
      snprintf(fallback, sizeof(fallback), "Chapter %u", static_cast<unsigned>(titlesOut.size() + 1));
      title = fallback;
    }
    titlesOut.push_back(std::move(title));
  }
  return !titlesOut.empty();
}

}  // namespace M4PluginTocList
