#include "apps/providers/M4NativeProviderCatalog.h"

#include "apps/M4xJsonStream.h"
#include "apps/providers/M4LegadoBridge.h"
#include "apps/providers/M4NativeProviderHeavyGate.h"
#include "apps/providers/M4NativeProviderHttp.h"
#include "apps/providers/M4Psram.h"
#include "apps/providers/M4NativeProviderIo.h"
#include "apps/providers/M4NativeProviderManager.h"
#include "util/M4ContentProviderContract.h"

#include <Arduino.h>
#include <SDCardManager.h>

#include <algorithm>
#include <atomic>
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

void publish(Phase phase, size_t received = 0, size_t rows = 0, const std::string& error = {}) {
  std::lock_guard<std::mutex> lock(gMu);
  gSnapshot.phase = phase;
  gSnapshot.receivedBytes = received;
  gSnapshot.rowCount = rows;
  gSnapshot.error = error;
  gSnapshot.updatedMs = millis();
}

bool cancelled() { return gCancel.load(std::memory_order_acquire); }

std::string appRoot(const std::string& appId) {
  return appId.empty() ? std::string() : std::string("/apps_data/") + appId;
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

class AtomicRowsSink final : public M4xJsonStream::Sink {
 public:
  ~AtomicRowsSink() override { close(); }

  bool open(const std::string& finalPath) {
    close();
    finalPath_ = finalPath;
    tmpPath_ = finalPath + ".tmp";
    written_ = 0;
    if (!M4NativeProviderIo::ensureParentDirs(finalPath_)) return false;
    if (SdMan.exists(tmpPath_.c_str())) SdMan.remove(tmpPath_.c_str());
    open_ = SdMan.openFileForWrite("NP-TOC", tmpPath_.c_str(), f_);
    return open_;
  }

  bool write(const uint8_t* data, size_t len) override {
    if (!open_ || !data) return false;
    // FatFS can return short writes under SPI bus contention (e-ink refresh
    // during progress paints). Retry the remainder instead of aborting the
    // whole catalog as sink_failed after a few dozen rows.
    size_t off = 0;
    while (off < len) {
      const int n = f_.write(data + off, len - off);
      if (n <= 0) return false;
      off += static_cast<size_t>(n);
    }
    written_ += len;
    return true;
  }

  void close() {
    if (open_) {
      f_.sync();
      f_.close();
      open_ = false;
    }
  }

  bool commit() {
    close();
    return written_ > 0 && !tmpPath_.empty() &&
           M4NativeProviderIo::commitTempFile(tmpPath_, finalPath_, written_, true);
  }

  void discard() {
    close();
    if (!tmpPath_.empty() && SdMan.exists(tmpPath_.c_str())) SdMan.remove(tmpPath_.c_str());
  }

 private:
  FsFile f_;
  std::string finalPath_;
  std::string tmpPath_;
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
    // Real shelves are 200–1500 chapters (~100–500 KB JSON). Cap at 1.5 MB.
    s.request.maxBytes = 1536u * 1024u;
    // Use chapter index as the FileRows uid: Legado chapter `url` often contains
    // '/' (web paths) which fails idOk(); content fetch already uses index0 via
    // getBookContent?url=&index=. index is a plain decimal string, M4-safe.
    s.path = {"data"};
    s.fields = {"index", "title"};
    s.vipField0 = -1;
    return s;
  }

  if (job.providerId == "jjwxc") {
    s.request.url = "https://app-cdn.jjwxc.net/androidapi/chapterList?novelId=" + job.bookId +
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
    s.request.url = "https://weread.qq.com/web/book/chapterInfos";
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

bool registerBook(const Snapshot& job, const CatalogSpec& spec, size_t rowCount) {
  if (rowCount == 0 || rowCount > M4ContentProvider::kMaxCatalogChapters) return false;
  M4ContentProvider::BookSpec book;
  book.providerId = job.providerId;
  book.appId = job.appId;
  book.bookId = job.bookId;
  book.title = boundedTitle(job.title, job.bookId);
  book.currentIndex0 = 0;
  book.catalog.kind = M4ContentProvider::ChapterCatalogKind::FileRows;
  book.catalog.fileRelPath = tocRelPath(job.bookId);
  book.catalog.chapterCount = rowCount;
  book.catalog.uidField0 = spec.uidField0;
  book.catalog.titleField0 = spec.titleField0;
  book.catalog.vipField0 = spec.vipField0;
  return M4NativeProviderManager::registerBook(book);
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

    // Legado catalogs are large (often 100–500 KB JSON, 600–1500 rows). Streaming
    // HTTP → JSON → SD in one shot races the e-ink progress repaint on the shared
    // SPI bus and aborts as sink_failed after a handful of rows. Download the
    // bounded body into PSRAM first, then parse/write SD without network callbacks.
    const bool legadoBuffered = job.providerId == "legado";
    std::string legadoBody;
    M4NativeProviderHttp::Result net;
    size_t rowCount = 0;
    std::string parseError;

    if (legadoBuffered) {
      M4NativeProviderHeavyGate::Lock heavy(M4NativeProviderHeavyGate::mutex());
      if (!M4NativeProviderHttp::requestSmall(spec.request, legadoBody, net, spec.request.maxBytes,
                                             [] { return cancelled(); })) {
        publish(Phase::Error, net.bytes, 0,
                cancelled() ? "cancelled" : (net.error.empty() ? "catalog_http" : net.error));
        gBusy.store(false, std::memory_order_release);
        {
          std::lock_guard<std::mutex> lock(gMu);
          gTask = nullptr;
        }
        M4Psram::deleteTask(nullptr);
        return;
      }
      publish(Phase::Receiving, net.bytes, 0);
    }

    AtomicRowsSink file;
    if (!file.open(finalPath)) {
      publish(Phase::Error, net.bytes, 0, "sd_open_failed");
    } else if (legadoBuffered) {
      M4xJsonStream::RecordExtractor rows(spec.path, spec.fields, file, spec.maxRows);
      publish(Phase::Receiving, net.bytes, 0);
      constexpr size_t kChunk = 2048;
      bool fed = true;
      for (size_t i = 0; i < legadoBody.size() && fed; i += kChunk) {
        if (cancelled()) {
          fed = false;
          parseError = "cancelled";
          break;
        }
        const size_t n = std::min(kChunk, legadoBody.size() - i);
        fed = rows.feed(reinterpret_cast<const uint8_t*>(legadoBody.data() + i), n);
        if ((i / kChunk) % 16u == 0u) {
          publish(Phase::Receiving, net.bytes, rows.recordCount());
        }
      }
      const bool parsed = fed && rows.finish() && rows.recordCount() > 0;
      rowCount = rows.recordCount();
      if (!parsed) {
        file.discard();
        if (parseError.empty()) {
          parseError = !fed ? (rows.error() == M4xJsonStream::Error::None
                                   ? "sink_failed"
                                   : M4xJsonStream::errorString(rows.error()))
                            : (rowCount == 0 ? "catalog_empty"
                                             : M4xJsonStream::errorString(rows.error()));
        }
        publish(Phase::Error, net.bytes, rowCount, parseError);
      } else if (!file.commit()) {
        file.discard();
        publish(Phase::Error, net.bytes, rowCount, "catalog_commit_failed");
      } else {
        publish(Phase::Registering, net.bytes, rowCount);
        if (!registerBook(job, spec, rowCount)) {
          publish(Phase::Error, net.bytes, rowCount, "catalog_register_failed");
        } else {
          publish(Phase::Ready, net.bytes, rowCount);
        }
      }
    } else {
      M4xJsonStream::RecordExtractor rows(spec.path, spec.fields, file, spec.maxRows);
      RecordExtractorSink jsonSink(rows);
      M4NativeProviderHeavyGate::Lock heavy(M4NativeProviderHeavyGate::mutex());
      net = M4NativeProviderHttp::requestToSink(
          spec.request, jsonSink,
          [&](size_t bytes) { publish(Phase::Receiving, bytes, rows.recordCount()); },
          [] { return cancelled(); });
      const bool parsed = net.ok && rows.finish() && rows.recordCount() > 0;
      rowCount = rows.recordCount();
      if (!parsed) {
        file.discard();
        // Prefer the JSON-stream error when the transport only saw sink_failed —
        // feed() returns false for TokenTooLarge/Syntax too, which used to look
        // like an SD write failure on device ("目录写入 SD 卡失败").
        std::string error;
        if (!net.ok) {
          if (net.error == "sink_failed" && rows.error() != M4xJsonStream::Error::None) {
            error = M4xJsonStream::errorString(rows.error());
          } else {
            error = net.error.empty() ? "catalog_http" : net.error;
          }
        } else if (rowCount == 0) {
          error = "catalog_empty";
        } else {
          error = M4xJsonStream::errorString(rows.error());
        }
        if (job.providerId == "weread" &&
            (error == "http_401" || error == "http_403" || error == "login_required")) {
          publish(Phase::AuthRequired, net.bytes, 0, error);
        } else {
          publish(Phase::Error, net.bytes, rowCount, cancelled() ? "cancelled" : error);
        }
      } else if (!file.commit()) {
        file.discard();
        publish(Phase::Error, net.bytes, rowCount, "catalog_commit_failed");
      } else {
        publish(Phase::Registering, net.bytes, rowCount);
        if (!registerBook(job, spec, rowCount)) {
          publish(Phase::Error, net.bytes, rowCount, "catalog_register_failed");
        } else {
          publish(Phase::Ready, net.bytes, rowCount);
        }
      }
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
           const std::string& appId, const std::string& title) {
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
    gSnapshot.startedMs = millis();
    gSnapshot.updatedMs = gSnapshot.startedMs;
  }
  TaskHandle_t handle = nullptr;
  // Stack in PSRAM so catalog HTTPS leaves internal RAM for TLS.
  if (M4Psram::createTask(taskMain, "NativeCatalog", 24u * 1024u, nullptr, 1, &handle) != pdPASS) {
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
