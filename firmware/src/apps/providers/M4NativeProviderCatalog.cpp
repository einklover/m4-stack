#include "apps/providers/M4NativeProviderCatalog.h"

#include "apps/M4xJsonStream.h"
#include "apps/providers/M4JjwxcEndpoint.h"
#include "apps/providers/M4WereadEndpoint.h"
#include "apps/providers/M4LegadoBridge.h"
#include "apps/providers/M4LegadoTocPolicy.h"
#include "apps/providers/M4NativeCatalogPolicy.h"
#include "apps/providers/M4NativeProviderHeavyGate.h"
#include "apps/providers/M4NativeProviderHttp.h"
#include "apps/providers/M4ProgressiveCatalog.h"
#include "apps/providers/M4Psram.h"
#include "apps/providers/M4NativeProviderIo.h"
#include "apps/providers/M4NativeProviderManager.h"
#include "util/M4ContentProviderContract.h"

#include <Arduino.h>
#include <SDCardManager.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace M4NativeProviderCatalog {
namespace {

constexpr const char* kFanqieUa =
    "Mozilla/5.0 (Linux; Android 10.0; wv) AppleWebKit/603.1.30 (KHTML, like Gecko) "
    "Version/4.0 Chrome/58.0.3029.110 Mobile Safari/537.36 T7/10.3 SearchCraft/2.6.2 (Baidu; P1 7.0)";
constexpr const char* kJjUa =
    "Mozilla/5.0 (Linux; Android 5.1; Lenovo) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Version/4.0 Chrome/39.0.0.0 Mobile Safari/537.36/JINJIANG-Android/206(Lenovo;android 5.1;Scale/2.0)";
constexpr const char* kJjRef = "http://android.jjwxc.net?v=206";

std::string urlEncode(const std::string& in) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(in.size() * 2);
  for (unsigned char c : in) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[(c >> 4) & 0xF];
      out += hex[c & 0xF];
    }
  }
  return out;
}

std::mutex gMu;
Snapshot gSnapshot;
std::atomic<bool> gBusy{false};
std::atomic<bool> gCancel{false};
TaskHandle_t gTask = nullptr;

void publish(Phase phase, size_t received = 0, size_t rows = 0, const std::string& error = {},
             bool partial = false, size_t totalHint = 0) {
  std::lock_guard<std::mutex> lock(gMu);
  gSnapshot.phase = phase;
  gSnapshot.receivedBytes = received;
  gSnapshot.rowCount = rows;
  gSnapshot.partial = partial;
  if (totalHint > 0) gSnapshot.totalHint = totalHint;
  gSnapshot.error = error;
  gSnapshot.updatedMs = millis();
}

bool cancelled() { return gCancel.load(std::memory_order_acquire); }

std::string appRoot(const std::string& appId) {
  return appId.empty() ? std::string() : std::string("/apps_data/") + appId;
}

// Read totalChapterNum from shelf_rows.tsv field 3 (Legado progressive metadata).
size_t readShelfTotalHint(const std::string& appId, const std::string& bookId) {
  if (appId.empty() || bookId.empty()) return 0;
  const std::string path = appRoot(appId) + "/provider/shelf_rows.tsv";
  FsFile f;
  if (!SdMan.openFileForRead("NP-TOC-HINT", path.c_str(), f)) return 0;
  std::string line;
  char buf[96];
  while (f.available()) {
    const int n = f.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf) - 1);
    if (n <= 0) break;
    buf[n] = 0;
    for (int i = 0; i < n; ++i) {
      if (buf[i] == '\n') {
        if (line.rfind(bookId, 0) == 0 && line.size() > bookId.size() && line[bookId.size()] == '\t') {
          // id \t name \t author \t totalChapterNum
          size_t t = 0;
          int field = 0;
          size_t start = 0;
          for (; t <= line.size(); ++t) {
            if (t == line.size() || line[t] == '\t') {
              if (field == 3) {
                f.close();
                unsigned long v = 0;
                for (size_t k = start; k < t; ++k) {
                  if (line[k] < '0' || line[k] > '9') {
                    v = 0;
                    break;
                  }
                  v = v * 10u + static_cast<unsigned>(line[k] - '0');
                }
                return static_cast<size_t>(v);
              }
              ++field;
              start = t + 1;
            }
          }
        }
        line.clear();
      } else {
        line.push_back(buf[i]);
      }
    }
  }
  f.close();
  return 0;
}

// Count rows actually present in a committed toc_rows.txt. Bounded by the
// catalog hard cap so a corrupt file cannot spin SD reads forever.
size_t countTocRows(const std::string& absPath) {
  FsFile f;
  if (!SdMan.openFileForRead("NP-TOC-CNT", absPath.c_str(), f)) return 0;
  uint8_t buf[2048];
  size_t rows = 0;
  bool any = false;
  uint8_t last = '\n';
  while (f.available()) {
    const int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    any = true;
    for (int i = 0; i < n; ++i) {
      last = buf[i];
      if (buf[i] == '\n' && ++rows >= M4ContentProvider::kMaxCatalogChapters) {
        f.close();
        return rows;
      }
    }
  }
  f.close();
  if (any && last != '\n' && rows < M4ContentProvider::kMaxCatalogChapters) ++rows;
  return rows;
}

// Write placeholder TSV via AtomicRowsSink-compatible temp commit.
bool writePlaceholderFile(const std::string& absPath, size_t total) {
  const auto policy = M4ProgressiveCatalog::defaultPolicy();
  const std::string body = M4ProgressiveCatalog::buildPlaceholderBody(total, policy);
  if (body.empty()) return false;
  if (!M4NativeProviderIo::ensureParentDirs(absPath)) return false;
  const std::string tmp = M4NativeProviderIo::replacedExtension(absPath, "part");
  if (SdMan.exists(tmp.c_str())) SdMan.remove(tmp.c_str());
  FsFile f;
  if (!SdMan.openFileForWrite("NP-TOC-PH", tmp.c_str(), f)) return false;
  size_t off = 0;
  while (off < body.size()) {
    const int n = f.write(reinterpret_cast<const uint8_t*>(body.data() + off), body.size() - off);
    if (n <= 0) {
      f.close();
      SdMan.remove(tmp.c_str());
      return false;
    }
    off += static_cast<size_t>(n);
  }
  f.sync();
  f.close();
  return M4NativeProviderIo::commitTempFile(tmp, absPath, body.size(), true);
}

std::string tocRelPath(const std::string& bookId) {
  return std::string("cache/") + bookId + "/toc_rows.txt";
}

std::string boundedTitle(std::string title, const std::string& fallback) {
  if (title.empty()) title = fallback;
  if (title.size() <= M4ContentProvider::kMaxTitleLen) return title;
  title.resize(M4ContentProvider::kMaxTitleLen);
  while (!title.empty() && (static_cast<unsigned char>(title.back()) & 0xC0u) == 0x80u) title.pop_back();
  if (title.empty()) return fallback;
  return title;
}

// Growable PSRAM buffer for catalog FileRows TSV. When free SPIRAM is
// available, Legado parses entirely off the SD bus, then flushes once in large
// sequential writes — much more resilient than per-row FatFS writes racing
// e-ink SPI traffic.
class PsramRowsSink final : public M4xJsonStream::Sink {
 public:
  ~PsramRowsSink() override { clear(); }

  // Soft cap for FileRows TSV in PSRAM. Must stay at least as large as the
  // largest Legado catalog we accept (JSON is bigger than TSV, but long
  // titles still need headroom). Aligned with request maxBytes below.
  static constexpr size_t kMaxBytes = 4u * 1024u * 1024u;

  bool reserve(size_t hint) {
    clear();
    const size_t initial = std::max<size_t>(8u * 1024u, std::min(hint, kMaxBytes));
    buf_ = static_cast<uint8_t*>(M4Psram::mallocPrefer(initial));
    if (!buf_) return false;
    cap_ = initial;
    size_ = 0;
    return true;
  }

  bool write(const uint8_t* data, size_t len) override {
    if (!data) return false;
    if (len == 0) return true;
    if (!buf_ && !reserve(std::max(len, size_t{8u * 1024u}))) return false;
    if (len > kMaxBytes || size_ > kMaxBytes - len) return false;
    if (size_ + len > cap_) {
      size_t next = cap_ ? cap_ * 2u : 8u * 1024u;
      while (next < size_ + len && next < kMaxBytes) next *= 2u;
      next = std::min(next, kMaxBytes);
      if (next < size_ + len) return false;
      auto* nb = static_cast<uint8_t*>(M4Psram::mallocPrefer(next));
      if (!nb) return false;
      if (size_) std::memcpy(nb, buf_, size_);
      M4Psram::freePrefer(buf_);
      buf_ = nb;
      cap_ = next;
    }
    std::memcpy(buf_ + size_, data, len);
    size_ += len;
    return true;
  }

  const uint8_t* data() const { return buf_; }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

  void clear() {
    M4Psram::freePrefer(buf_);
    buf_ = nullptr;
    size_ = 0;
    cap_ = 0;
  }

 private:
  uint8_t* buf_ = nullptr;
  size_t size_ = 0;
  size_t cap_ = 0;
};

class AtomicRowsSink final : public M4xJsonStream::Sink {
 public:
  ~AtomicRowsSink() override { close(); }

  bool open(const std::string& finalPath) {
    close();
    finalPath_ = finalPath;
    tmpPath_ = M4NativeProviderIo::replacedExtension(finalPath, "part");
    written_ = 0;
    used_ = 0;
    if (!M4NativeProviderIo::ensureParentDirs(finalPath_)) return false;
    if (SdMan.exists(tmpPath_.c_str())) SdMan.remove(tmpPath_.c_str());
    // SD open can fail briefly under SPI contention (e-ink FAST_REFRESH) or a
    // busy FatFS volume. A few short retries recover most "works on my card"
    // / "fails on theirs" catalog failures after a successful download.
    for (int attempt = 0; attempt < 4; ++attempt) {
      if (SdMan.openFileForWrite("NP-TOC", tmpPath_.c_str(), f_)) {
        open_ = true;
        return true;
      }
      delay(30 + attempt * 40);
    }
    open_ = false;
    return false;
  }

  bool write(const uint8_t* data, size_t len) override {
    if (!open_ || !data) return false;
    if (len == 0) return true;
    // Coalesce tiny TSV rows (one chapter per emit) into larger SPI bursts.
    // Thousands of single-line FatFS writes are far more likely to collide
    // with the shared display SPI bus than a few KB flushes.
    size_t off = 0;
    while (off < len) {
      const size_t take = std::min(len - off, kBufferBytes - used_);
      std::memcpy(buf_ + used_, data + off, take);
      used_ += take;
      off += take;
      if (used_ == kBufferBytes && !flushBuffer()) return false;
    }
    written_ += len;
    return true;
  }

  bool close() {
    if (!open_) return true;
    bool ok = flushBuffer();
    if (ok) f_.sync();
    f_.close();
    open_ = false;
    used_ = 0;
    return ok;
  }

  bool commit() {
    if (!close() || written_ == 0 || tmpPath_.empty()) return false;
    for (int attempt = 0; attempt < 3; ++attempt) {
      if (M4NativeProviderIo::commitTempFile(tmpPath_, finalPath_, written_, true)) return true;
      delay(40 + attempt * 60);
    }
    return false;
  }

  void discard() {
    close();
    if (!tmpPath_.empty() && SdMan.exists(tmpPath_.c_str())) SdMan.remove(tmpPath_.c_str());
  }

 private:
  static constexpr size_t kBufferBytes = 8u * 1024u;

  bool flushBuffer() {
    if (!open_ || used_ == 0) return open_;
    // FatFS can return short writes or transient -1 under SPI bus contention.
    // Retry the remainder (and full failures with a brief backoff) instead of
    // aborting the whole catalog as sink_failed after a few dozen rows.
    size_t off = 0;
    int hardFails = 0;
    while (off < used_) {
      const int n = f_.write(buf_ + off, used_ - off);
      if (n > 0) {
        off += static_cast<size_t>(n);
        hardFails = 0;
        continue;
      }
      if (++hardFails > 6) return false;
      delay(8 * hardFails);
    }
    used_ = 0;
    return true;
  }

  FsFile f_;
  std::string finalPath_;
  std::string tmpPath_;
  uint8_t buf_[kBufferBytes]{};
  size_t used_ = 0;
  size_t written_ = 0;
  bool open_ = false;
};

class RecordExtractorSink final : public M4xJsonStream::Sink {
 public:
  explicit RecordExtractorSink(M4xJsonStream::RecordExtractor& extractor) : extractor_(extractor) {}
  bool write(const uint8_t* data, size_t len) override { return extractor_.feed(data, len); }
 private:
  M4xJsonStream::RecordExtractor& extractor_;
};

struct CatalogSpec {
  M4NativeProviderHttp::Request request;
  std::vector<std::string> path;
  std::vector<std::string> fields;
  int uidField0 = 0;
  int titleField0 = 1;
  int vipField0 = -1;
  size_t maxRows = M4ContentProvider::kMaxCatalogChapters;
  bool authRequired = false;
  std::string error;
};

CatalogSpec makeSpec(const Snapshot& job) {
  CatalogSpec s;
  s.request.timeoutMs = 45000;

  if (job.providerId == "fanqie") {
    s.request.url = "https://fanqienovel.com/api/reader/directory/detail?bookId=" + job.bookId;
    s.request.headers = {{"User-Agent", kFanqieUa}, {"Referer", "https://fanqienovel.com/"}};
    s.request.maxBytes = 4u * 1024u * 1024u;
    s.path = {"data", "chapterListWithVolume"};
    s.fields = {"itemId", "title"};
    return s;
  }

  if (job.providerId == "legado") {
    const std::string locator = M4LegadoBridge::readLocator(appRoot(job.appId), job.bookId);
    if (locator.empty()) {
      s.error = "book_locator_missing";
      return s;
    }
    if (!M4LegadoBridge::ensureEndpoint(appRoot(job.appId))) {
      s.error = "legado_endpoint_missing";
      return s;
    }
    s.request.url = M4LegadoBridge::baseUrl() + "/getChapterList?url=" + urlEncode(locator);
    s.request.headers = {{"User-Agent", "Mozilla/5.0 Murphy-M4 NativeProvider/1"}};
    // Match getBookContent: some server editions answer the chapter list with
    // an HTTP redirect when the locator was re-normalized on the phone.
    s.request.followRedirects = true;
    // Long web novels (2k–10k chapters) often ship 1–3 MB JSON with full
    // title/url fields. 1.5 MB was too tight and surfaced as response_too_large
    // ("目录数据过大"). Match other providers: 4 MB PSRAM-backed body.
    s.request.maxBytes = 4u * 1024u * 1024u;
    // Use chapter index as the FileRows uid: Legado chapter `url` often contains
    // '/' (web paths) which fails idOk(); content fetch already uses index0 via
    // getBookContent?url=&index=. index is a plain decimal string, M4-safe.
    s.path = {"data"};
    s.fields = {"index", "title"};
    s.vipField0 = -1;
    return s;
  }

  if (job.providerId == "jjwxc") {
    s.request.url = std::string(M4_JJWXC_APP_CDN) + "/androidapi/chapterList?novelId=" + job.bookId +
                    "&more=0&whole=1";
    s.request.headers = {{"User-Agent", kJjUa}, {"Referer", kJjRef}, {"Connection", "close"}};
    s.request.maxBytes = 4u * 1024u * 1024u;
    // JJWXC chapterList returns an object whose chapter rows live under the
    // `chapterlist` array. Treating the root object itself as the row array
    // makes a successful HTTP response look like catalog_empty on device.
    s.path = {"chapterlist"};
    s.fields = {"chapterid", "chaptername", "chaptertype", "isvip", "islock"};
    s.vipField0 = 3;
    return s;
  }

  if (job.providerId == "weread") {
    std::string cookie;
    if (!M4NativeProviderIo::loadCookieHeader(appRoot(job.appId), "weread", cookie)) {
      s.authRequired = true;
      s.error = "login_required";
      return s;
    }
    s.request.method = "POST";
    s.request.url = std::string(M4_WEREAD_ORIGIN) + "/web/book/chapterInfos";
    s.request.headers = {{"User-Agent", "Mozilla/5.0 Murphy-M4 NativeProvider/1"},
                         {"Referer", "https://weread.qq.com/"},
                         {"Content-Type", "application/json"}, {"Cookie", cookie}};
    s.request.body = std::string("{\"bookIds\":[\"") + job.bookId + "\"],\"synckeys\":[0],\"teenmode\":0}";
    s.request.maxBytes = 4u * 1024u * 1024u;
    s.path = {"data", "updated"};
    s.fields = {"chapterUid", "title", "wordCount"};
    return s;
  }

  s.error = "provider_not_supported";
  return s;
}

bool registerBook(const Snapshot& job, const CatalogSpec& spec, size_t rowCount, int currentIndex0 = 0) {
  if (rowCount == 0 || rowCount > M4ContentProvider::kMaxCatalogChapters) return false;
  M4ContentProvider::BookSpec book;
  book.providerId = job.providerId;
  book.appId = job.appId;
  book.bookId = job.bookId;
  book.title = boundedTitle(job.title, job.bookId);
  book.currentIndex0 = currentIndex0;
  book.catalog.kind = M4ContentProvider::ChapterCatalogKind::FileRows;
  book.catalog.fileRelPath = tocRelPath(job.bookId);
  book.catalog.chapterCount = rowCount;
  book.catalog.uidField0 = spec.uidField0;
  book.catalog.titleField0 = spec.titleField0;
  book.catalog.vipField0 = spec.vipField0;
  return M4NativeProviderManager::registerBook(book);
}

// Commit a PSRAM TSV body (or fail). Releases PSRAM before returning.
bool commitPsramBody(const std::string& finalPath, PsramRowsSink& mem) {
  if (mem.empty()) return false;
  AtomicRowsSink file;
  if (!file.open(finalPath)) {
    mem.clear();
    return false;
  }
  if (!file.write(mem.data(), mem.size())) {
    file.discard();
    mem.clear();
    return false;
  }
  // Drop PSRAM before FatFS rename so peak RAM stays lower.
  const size_t bytes = mem.size();
  mem.clear();
  if (!file.commit()) {
    file.discard();
    Serial.printf("[NativeCatalog] psram→SD commit failed size=%u\n",
                  static_cast<unsigned>(bytes));
    return false;
  }
  Serial.printf("[NativeCatalog] psram→SD ok size=%u\n", static_cast<unsigned>(bytes));
  return true;
}

// Full catalog download: prefer PSRAM TSV assembly (no SD during parse), fall
// back to buffered AtomicRowsSink if PSRAM cannot reserve.
// On success: *outRows / *outBytes filled. On failure returns false (caller
// decides whether to keep a partial catalog).
bool downloadFullCatalog(const Snapshot& job, const CatalogSpec& spec,
                         const std::string& finalPath, size_t totalHint,
                         bool keepPartialOnFail, size_t* outRows, size_t* outBytes,
                         bool* outTransferOk = nullptr, std::string* outError = nullptr) {
  publish(Phase::Receiving, 0, 0, {}, keepPartialOnFail, totalHint);

  // --- Prefer PSRAM path ---
  PsramRowsSink mem;
  if (M4NativeCatalogPolicy::preferPsramAssembly(job.providerId) && mem.reserve(256u * 1024u)) {
    M4xJsonStream::RecordExtractor rows(spec.path, spec.fields, mem, spec.maxRows);
    RecordExtractorSink jsonSink(rows);
    M4NativeProviderHttp::Result net;
    {
      M4NativeProviderHeavyGate::Lock heavy(M4NativeProviderHeavyGate::mutex());
      net = M4NativeProviderHttp::requestToSink(
          spec.request, jsonSink,
          [&](size_t bytes) {
            publish(Phase::Receiving, bytes, rows.recordCount(), {}, keepPartialOnFail, totalHint);
          },
          [] { return cancelled(); });
    }
    const bool parsed = net.ok && rows.finish() && rows.recordCount() > 0 && !mem.empty();
    if (!parsed) {
      mem.clear();
      if (outBytes) *outBytes = net.bytes;
      if (outRows) *outRows = rows.recordCount();
      if (outTransferOk) *outTransferOk = net.ok;
      if (outError) {
        // Same classification streamCatalogProgressive uses so stale-shelf
        // detection sees identical error identities on both open paths.
        if (!net.ok) {
          *outError = net.error.empty() ? "catalog_http" : net.error;
        } else if (rows.recordCount() == 0) {
          *outError = "catalog_empty";
        } else {
          *outError = M4xJsonStream::errorString(rows.error());
        }
      }
      Serial.printf("[NativeCatalog] full psram parse failed err=%s keep_partial=%d\n",
                    net.error.empty() ? M4xJsonStream::errorString(rows.error())
                                      : net.error.c_str(),
                    keepPartialOnFail ? 1 : 0);
      return false;
    }
    const size_t rowCount = rows.recordCount();
    if (!commitPsramBody(finalPath, mem)) {
      if (outBytes) *outBytes = net.bytes;
      if (outRows) *outRows = rowCount;
      if (outTransferOk) *outTransferOk = net.ok;
      if (outError) *outError = "catalog_commit_failed";
      return false;
    }
    if (outBytes) *outBytes = net.bytes;
    if (outRows) *outRows = rowCount;
    return true;
  }

  // --- Direct buffered SD writes ---
  // Fanqie deliberately takes this path so catalog memory stays O(1) in chapter count.
  Serial.printf("[NativeCatalog] direct SD stream provider=%s\n", job.providerId.c_str());
  AtomicRowsSink file;
  if (!file.open(finalPath)) {
    if (outTransferOk) *outTransferOk = false;
    if (outError) *outError = "sd_open_failed";
    if (!keepPartialOnFail) publish(Phase::Error, 0, 0, "sd_open_failed", false, totalHint);
    return false;
  }
  M4xJsonStream::RecordExtractor rows(spec.path, spec.fields, file, spec.maxRows);
  RecordExtractorSink jsonSink(rows);
  M4NativeProviderHttp::Result net;
  {
    M4NativeProviderHeavyGate::Lock heavy(M4NativeProviderHeavyGate::mutex());
    net = M4NativeProviderHttp::requestToSink(
        spec.request, jsonSink,
        [&](size_t bytes) {
          publish(Phase::Receiving, bytes, rows.recordCount(), {}, keepPartialOnFail, totalHint);
        },
        [] { return cancelled(); });
  }
  const bool parsed = net.ok && rows.finish() && rows.recordCount() > 0;
  if (!parsed) {
    file.discard();
    if (outBytes) *outBytes = net.bytes;
    if (outRows) *outRows = rows.recordCount();
    if (outTransferOk) *outTransferOk = net.ok;
    if (outError) {
      if (!net.ok) {
        *outError = net.error.empty() ? "catalog_http" : net.error;
      } else if (rows.recordCount() == 0) {
        *outError = "catalog_empty";
      } else {
        *outError = M4xJsonStream::errorString(rows.error());
      }
    }
    return false;
  }
  if (!file.commit()) {
    file.discard();
    if (outBytes) *outBytes = net.bytes;
    if (outRows) *outRows = rows.recordCount();
    if (outTransferOk) *outTransferOk = net.ok;
    if (outError) *outError = "catalog_commit_failed";
    return false;
  }
  if (outBytes) *outBytes = net.bytes;
  if (outRows) *outRows = rows.recordCount();
  return true;
}

// Shared progressive stream: first window → Ready(partial), then full refill.
// Used by every native provider so plugins share one open-speed path.
bool streamCatalogProgressive(const Snapshot& job, const CatalogSpec& spec,
                              const std::string& finalPath, size_t totalHint) {
  const auto policy = M4ProgressiveCatalog::defaultPolicy();
  AtomicRowsSink file;
  if (!file.open(finalPath)) {
    publish(Phase::Error, 0, 0, "sd_open_failed", false, totalHint);
    return false;
  }

  M4ProgressiveCatalog::FirstWindowSink window(file, policy.firstWindow);
  M4xJsonStream::RecordExtractor rows(spec.path, spec.fields, window, spec.maxRows);
  RecordExtractorSink jsonSink(rows);

  publish(Phase::Receiving, 0, 0, {}, false, totalHint);
  M4NativeProviderHttp::Result net;
  {
    M4NativeProviderHeavyGate::Lock heavy(M4NativeProviderHeavyGate::mutex());
    net = M4NativeProviderHttp::requestToSink(
        spec.request, jsonSink,
        [&](size_t bytes) { publish(Phase::Receiving, bytes, window.count(), {}, false, totalHint); },
        M4ProgressiveCatalog::windowCancel(window, [] { return cancelled(); }));
  }

  // First window filled → open immediately (partial). HTTP was cancelled early.
  // Do NOT overwrite these real titles with placeholders — placeholders are only
  // for the instant-open path when totalHint is known before any stream.
  if (window.windowReady() && window.count() > 0) {
    if (!file.commit()) {
      file.discard();
      Serial.printf("[NativeCatalog] first-window commit failed rows=%u path=%s\n",
                    static_cast<unsigned>(window.count()), finalPath.c_str());
      publish(Phase::Error, net.bytes, window.count(), "catalog_commit_failed", false, totalHint);
      return false;
    }
    const size_t partialRows = window.count();
    publish(Phase::Registering, net.bytes, partialRows, {}, true, totalHint);
    if (!registerBook(job, spec, partialRows, job.currentIndex0)) {
      publish(Phase::Error, net.bytes, partialRows, "catalog_register_failed", false, totalHint);
      return false;
    }
    Serial.printf("[NativeCatalog] progressive first-window ready rows=%u hint=%u provider=%s\n",
                  static_cast<unsigned>(window.count()), static_cast<unsigned>(totalHint),
                  job.providerId.c_str());
    publish(Phase::Ready, net.bytes, partialRows, {}, true, totalHint);

    // Background full refill (same task): PSRAM TSV → bulk SD when possible.
    if (cancelled()) return true;
    size_t fullRows = 0;
    size_t fullBytes = 0;
    if (!downloadFullCatalog(job, spec, finalPath, totalHint, /*keepPartialOnFail=*/true,
                             &fullRows, &fullBytes)) {
      Serial.printf("[NativeCatalog] progressive full refill failed keep partial rows=%u\n",
                    static_cast<unsigned>(partialRows));
      return true;
    }
    (void)registerBook(job, spec, fullRows, job.currentIndex0);
    publish(Phase::Ready, fullBytes, fullRows, {}, false, fullRows);
    Serial.printf("[NativeCatalog] progressive full ready rows=%u provider=%s\n",
                  static_cast<unsigned>(fullRows), job.providerId.c_str());
    return true;
  }

  // Stream finished without hitting the window (small catalog).
  const bool parsed = net.ok && rows.finish() && rows.recordCount() > 0;
  const size_t rowCount = rows.recordCount();
  if (!parsed) {
    file.discard();
    std::string error;
    if (!net.ok) {
      if (net.error == "sink_failed" && rows.error() != M4xJsonStream::Error::None) {
        error = M4xJsonStream::errorString(rows.error());
      } else if (net.error == "cancelled" || cancelled()) {
        error = "cancelled";
      } else {
        error = net.error.empty() ? "catalog_http" : net.error;
      }
    } else if (rowCount == 0) {
      error = "catalog_empty";
    } else {
      error = M4xJsonStream::errorString(rows.error());
    }
    // Stale Legado phone shelf: empty 200 body / 404 / path-not-found with
    // zero records is not a transient failure and must not be masked as a
    // generic empty catalog. Strictly Legado-only — other providers keep
    // their own error strings and UI mapping.
    if (job.providerId == "legado" && !cancelled() &&
        M4LegadoTocPolicy::isStaleShelfFetch(net.ok, error, rowCount)) {
      error = "legado_shelf_stale";
    }
    if (job.providerId == "weread" &&
        (error == "http_401" || error == "http_403" || error == "login_required")) {
      publish(Phase::AuthRequired, net.bytes, 0, error);
    } else {
      publish(Phase::Error, net.bytes, rowCount, cancelled() ? "cancelled" : error);
    }
    return false;
  }
  if (!file.commit()) {
    file.discard();
    Serial.printf("[NativeCatalog] small-catalog commit failed rows=%u path=%s\n",
                  static_cast<unsigned>(rowCount), finalPath.c_str());
    publish(Phase::Error, net.bytes, rowCount, "catalog_commit_failed");
    return false;
  }
  publish(Phase::Registering, net.bytes, rowCount);
  if (!registerBook(job, spec, rowCount, job.currentIndex0)) {
    publish(Phase::Error, net.bytes, rowCount, "catalog_register_failed");
    return false;
  }
  publish(Phase::Ready, net.bytes, rowCount, {}, false, rowCount);
  return true;
}

void taskMain(void*) {
  Snapshot job;
  {
    std::lock_guard<std::mutex> lock(gMu);
    job = gSnapshot;
  }

  const CatalogSpec spec = makeSpec(job);
  if (spec.authRequired) {
    publish(Phase::AuthRequired, 0, 0, spec.error);
  } else if (!spec.error.empty()) {
    publish(Phase::Error, 0, 0, spec.error);
  } else {
    const std::string finalPath = appRoot(job.appId) + "/" + tocRelPath(job.bookId);
    publish(Phase::Connecting);

    // Shared progressive path (all plugins). Strategy from M4ProgressiveCatalog:
    //   PlaceholderThenFull — known total (Legado totalChapterNum) → skeleton now
    //   WindowThenFull      — stream first window, Ready, full refill
    size_t totalHint = 0;
    if (job.providerId == "legado") {
      totalHint = readShelfTotalHint(job.appId, job.bookId);
      // A cached TOC with real rows must never be placeholder-overwritten just
      // because the (possibly stale) shelf still advertises a larger count:
      // seed directly from cache and let only a successful full stream replace it.
      const size_t cachedRows = countTocRows(finalPath);
      if (cachedRows > 0 && !M4LegadoTocPolicy::mayWritePlaceholderSkeleton(totalHint, cachedRows)) {
        Serial.printf("[NativeCatalog] legado cached TOC wins hint=%u rows=%u book=%s\n",
                      static_cast<unsigned>(totalHint), static_cast<unsigned>(cachedRows),
                      job.bookId.c_str());
        (void)registerBook(job, spec, cachedRows, job.currentIndex0);
        publish(Phase::Ready, 0, cachedRows, {}, true, cachedRows);
        size_t fullRows = 0;
        size_t fullBytes = 0;
        if (downloadFullCatalog(job, spec, finalPath, totalHint, /*keepPartialOnFail=*/true,
                                &fullRows, &fullBytes)) {
          (void)registerBook(job, spec, fullRows, job.currentIndex0);
          publish(Phase::Ready, fullBytes, fullRows, {}, false, fullRows);
        }
        return;
      }
    }
    const auto strategy = M4ProgressiveCatalog::chooseStrategy(
        totalHint, M4ProgressiveCatalog::defaultPolicy().firstWindow);

    if (strategy == M4ProgressiveCatalog::OpenStrategy::PlaceholderThenFull) {
      if (writePlaceholderFile(finalPath, totalHint) &&
          registerBook(job, spec, totalHint, job.currentIndex0)) {
        Serial.printf("[NativeCatalog] placeholder ready count=%u book=%s\n",
                      static_cast<unsigned>(totalHint), job.bookId.c_str());
        publish(Phase::Ready, 0, totalHint, {}, true, totalHint);
        // Full stream (PSRAM when available) replaces placeholders with real titles.
        size_t fullRows = 0;
        size_t fullBytes = 0;
        bool fullTransferOk = false;
        std::string fullErr;
        if (downloadFullCatalog(job, spec, finalPath, totalHint, /*keepPartialOnFail=*/true,
                                &fullRows, &fullBytes, &fullTransferOk, &fullErr)) {
          (void)registerBook(job, spec, fullRows, job.currentIndex0);
          publish(Phase::Ready, fullBytes, fullRows, {}, false, fullRows);
          Serial.printf("[NativeCatalog] placeholder→full ready rows=%u\n",
                        static_cast<unsigned>(fullRows));
        } else {
          Serial.printf("[NativeCatalog] full refill failed err=%s keep placeholders\n",
                        fullErr.c_str());
          // Stale Legado shelf on a fresh first open (empty 200 {"data":[]},
          // 404 locator gone, or changed response shape with zero records):
          // replace the hollow Ready-with-placeholders state with a visible
          // error instead of leaving 第N章 skeletons. Transient network
          // failures keep placeholders for retry.
          if (job.providerId == "legado" && !cancelled() &&
              M4LegadoTocPolicy::isStaleShelfFetch(fullTransferOk, fullErr, fullRows)) {
            // Drop the skeleton file before surfacing the error. Otherwise the
            // next open sees cachedRows > 0 and the cache guard would seed
            // Ready from hollow 第N章 placeholders instead of retrying the
            // (stale) shelf. Transient failures keep placeholders on disk.
            if (SdMan.exists(finalPath.c_str())) SdMan.remove(finalPath.c_str());
            publish(Phase::Error, 0, 0, "legado_shelf_stale");
          }
        }
      } else {
        // Placeholder failed — fall through to progressive stream.
        (void)streamCatalogProgressive(job, spec, finalPath, totalHint);
      }
    } else {
      (void)streamCatalogProgressive(job, spec, finalPath, totalHint);
    }
  }

  gBusy.store(false, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(gMu);
    gTask = nullptr;
  }
  M4Psram::deleteTask(nullptr);
}

}  // namespace

bool start(const std::string& providerId, const std::string& bookId,
           const std::string& appId, const std::string& title, int focusIndex0) {
  if ((providerId != "fanqie" && providerId != "jjwxc" && providerId != "weread" &&
       providerId != "legado") ||
      appId.empty() || !M4ContentProvider::idOk(bookId.c_str(), M4ContentProvider::kMaxBookIdLen)) {
    return false;
  }
  bool expected = false;
  if (!gBusy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return false;
  gCancel.store(false, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(gMu);
    gSnapshot = {};
    gSnapshot.phase = Phase::Connecting;
    gSnapshot.providerId = providerId;
    gSnapshot.appId = appId;
    gSnapshot.bookId = bookId;
    gSnapshot.title = title;
    gSnapshot.partial = false;
    gSnapshot.totalHint = 0;
    gSnapshot.currentIndex0 = focusIndex0 > 0 ? focusIndex0 : 0;
    gSnapshot.startedMs = millis();
    gSnapshot.updatedMs = gSnapshot.startedMs;
  }
  TaskHandle_t handle = nullptr;
  // Stack in PSRAM so catalog HTTPS leaves internal RAM for TLS.
  if (M4Psram::createTask(taskMain, "NativeCatalog", M4NativeCatalogPolicy::kTaskStackBytes,
                          nullptr, 1, &handle) != pdPASS) {
    gBusy.store(false, std::memory_order_release);
    publish(Phase::Error, 0, 0, "catalog_task_create");
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(gMu);
    gTask = handle;
  }
  return true;
}

Snapshot snapshot() {
  std::lock_guard<std::mutex> lock(gMu);
  return gSnapshot;
}

bool busy() { return gBusy.load(std::memory_order_acquire); }
void cancel() { gCancel.store(true, std::memory_order_release); }

}  // namespace M4NativeProviderCatalog
