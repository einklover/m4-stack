#include "apps/providers/M4NativeProviderDiscovery.h"

#include "apps/M4xJsonStream.h"
#include "apps/providers/M4JjwxcEndpoint.h"
#include "apps/providers/M4WereadEndpoint.h"
#include "apps/providers/M4LegadoBridge.h"
#include "apps/providers/M4NativeProviderExplore.h"
#include "apps/providers/M4NativeProviderHeavyGate.h"
#include "apps/providers/M4NativeProviderHttp.h"
#include "apps/providers/M4NativeProviderIo.h"
#include "apps/providers/M4Psram.h"
#include "util/M4WereadAuthPolicy.h"

#include <Arduino.h>
#include <SDCardManager.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace M4NativeProviderDiscovery {
namespace {

constexpr const char* kFanqieUa =
    "Mozilla/5.0 (Linux; Android 10.0; wv) AppleWebKit/603.1.30 (KHTML, like Gecko) "
    "Version/4.0 Chrome/58.0.3029.110 Mobile Safari/537.36 T7/10.3 SearchCraft/2.6.2 (Baidu; P1 7.0)";
std::mutex gMu;
Snapshot gSnapshot;
std::atomic<bool> gBusy{false};
TaskHandle_t gTask = nullptr;

void publish(Phase phase, size_t received = 0, size_t rows = 0, const std::string& error = {}) {
  std::lock_guard<std::mutex> lock(gMu);
  gSnapshot.phase = phase;
  gSnapshot.receivedBytes = received;
  gSnapshot.rowCount = rows;
  gSnapshot.error = error;
  gSnapshot.updatedMs = millis();
}

std::string appRoot(const std::string& appId) {
  return appId.empty() ? std::string() : std::string("/apps_data/") + appId;
}

std::string rowsPath(const std::string& appId) {
  return appRoot(appId) + "/provider/shelf_rows.tsv";
}

std::string urlEncode(const std::string& s) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 2);
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[(c >> 4) & 0x0F]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

class AtomicRowsSink final : public M4xJsonStream::Sink {
 public:
  ~AtomicRowsSink() override { close(); }

  bool open(const std::string& finalPath) {
    close();
    finalPath_ = finalPath;
    tmpPath_ = finalPath_ + ".tmp";
    written_ = 0;
    if (!M4NativeProviderIo::ensureParentDirs(finalPath_)) return false;
    // Stale zero-byte *.tmp from a previous aborted discovery can block FatFS
    // open-for-write on some cards; force-remove then retry once.
    if (SdMan.exists(tmpPath_.c_str())) (void)SdMan.remove(tmpPath_.c_str());
    open_ = SdMan.openFileForWrite("NP-DISC", tmpPath_.c_str(), f_);
    if (!open_) {
      if (SdMan.exists(tmpPath_.c_str())) (void)SdMan.remove(tmpPath_.c_str());
      open_ = SdMan.openFileForWrite("NP-DISC", tmpPath_.c_str(), f_);
    }
    return open_;
  }

  bool write(const uint8_t* data, size_t len) override {
    if (!open_ || !data) return false;
    const int n = f_.write(data, len);
    if (n != static_cast<int>(len)) return false;
    written_ += len;
    return true;
  }

  // Direct raw write used by the Legado shelf parser after buffering. Same
  // file handle as write(), bypasses the Sink protocol.
  bool rawWrite(const char* data, size_t len) {
    return write(reinterpret_cast<const uint8_t*>(data), len);
  }

  void close() {
    if (open_) {
      f_.close();
      open_ = false;
    }
  }

  bool commit() {
    close();
    if (tmpPath_.empty() || finalPath_.empty() || written_ == 0 || !SdMan.exists(tmpPath_.c_str())) {
      return false;
    }
    const std::string backup = finalPath_ + ".bak";
    if (SdMan.exists(backup.c_str())) SdMan.remove(backup.c_str());
    const bool hadOld = SdMan.exists(finalPath_.c_str());
    if (hadOld && !SdMan.rename(finalPath_.c_str(), backup.c_str())) return false;
    if (!SdMan.rename(tmpPath_.c_str(), finalPath_.c_str())) {
      if (hadOld && SdMan.exists(backup.c_str())) (void)SdMan.rename(backup.c_str(), finalPath_.c_str());
      return false;
    }
    if (SdMan.exists(backup.c_str())) SdMan.remove(backup.c_str());
    return true;
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

  bool write(const uint8_t* data, size_t len) override {
    if (data && prefix_.size() < kPrefixMax) {
      const size_t take = std::min(len, kPrefixMax - prefix_.size());
      prefix_.append(reinterpret_cast<const char*>(data), take);
    }
    return extractor_.feed(data, len);
  }

  const std::string& prefix() const { return prefix_; }

 private:
  static constexpr size_t kPrefixMax = 512;
  M4xJsonStream::RecordExtractor& extractor_;
  std::string prefix_;
};

// Legado shelf rewrite sink: RecordExtractor emits bookUrl\tname\tauthor\n;
// we map bookUrl → short FNV id (M4-safe) and keep the locator in a sidecar
// for later getChapterList/getBookContent. Sidecar is best-effort: a shelf
// without locators still renders; catalog/content will then report
// book_locator_missing rather than failing discovery entirely.
class LegadoRewriteSink final : public M4xJsonStream::Sink {
 public:
  LegadoRewriteSink(AtomicRowsSink& rows, const std::string& appDataRoot)
      : rows_(rows), sidecarPath_(M4LegadoBridge::sidecarPath(appDataRoot)) {
    if (!sidecarPath_.empty()) {
      (void)M4NativeProviderIo::ensureParentDirs(sidecarPath_);
      if (SdMan.exists(sidecarPath_.c_str())) SdMan.remove(sidecarPath_.c_str());
      sidecarOpen_ = SdMan.openFileForWrite("LegadoSidecar", sidecarPath_.c_str(), sidecar_);
    }
  }

  ~LegadoRewriteSink() override { closeSidecar(); }

  // Flush sidecar before AtomicRowsSink commit so the locator map is on FAT
  // even if the rewrite sink outlives the last book write.
  void closeSidecar() {
    if (sidecarOpen_) {
      sidecar_.close();
      sidecarOpen_ = false;
    }
  }

  bool write(const uint8_t* data, size_t len) override {
    if (!data || len == 0) return true;
    // RecordExtractor always emits one complete line per write().
    std::string line(reinterpret_cast<const char*>(data), len);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    if (line.empty()) return true;

    const size_t t1 = line.find('\t');
    const std::string bookUrl = t1 == std::string::npos ? line : line.substr(0, t1);
    const std::string rest = t1 == std::string::npos ? std::string() : line.substr(t1);
    if (bookUrl.empty()) {
      ++skipped_;
      return true;
    }
    const std::string id = M4LegadoBridge::shortId(bookUrl);
    if (!M4LegadoBridge::idOkShort(id)) {
      ++skipped_;
      return true;
    }

    std::string out = id;
    out += rest;
    out += '\n';
    if (!rows_.rawWrite(out.data(), out.size())) return false;
    ++written_;

    if (sidecarOpen_) {
      std::string sc = id;
      sc += '\t';
      sc += bookUrl;
      sc += '\n';
      (void)sidecar_.write(reinterpret_cast<const uint8_t*>(sc.data()), sc.size());
    }
    return true;
  }

  size_t written() const { return written_; }
  size_t skipped() const { return skipped_; }
  bool sidecarOpen() const { return sidecarOpen_; }

 private:
  AtomicRowsSink& rows_;
  std::string sidecarPath_;
  FsFile sidecar_;
  bool sidecarOpen_ = false;
  size_t written_ = 0;
  size_t skipped_ = 0;
};

// Best-effort SD breadcrumb so serial-log races do not hide the failure mode.
void writeDiscoveryDiag(const std::string& appId, const char* stage, bool netOk, size_t netBytes,
                        const std::string& netErr, size_t rows, size_t skipped, bool sidecar) {
  if (appId.empty()) return;
  const std::string path = appRoot(appId) + "/provider/discovery_diag.txt";
  (void)M4NativeProviderIo::ensureParentDirs(path);
  FsFile f;
  if (!SdMan.openFileForWrite("NP-DIAG", path.c_str(), f)) return;
  char buf[256];
  const int n = std::snprintf(
      buf, sizeof(buf),
      "stage=%s net_ok=%d bytes=%u err=%s rows=%u skip=%u sidecar=%d ms=%u\n",
      stage ? stage : "-", netOk ? 1 : 0, static_cast<unsigned>(netBytes),
      netErr.empty() ? "-" : netErr.c_str(), static_cast<unsigned>(rows),
      static_cast<unsigned>(skipped), sidecar ? 1 : 0, static_cast<unsigned>(millis()));
  if (n > 0) (void)f.write(reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(n));
  f.close();
}

struct DiscoverySpec {
  M4NativeProviderHttp::Request request;
  std::vector<std::string> path;
  std::vector<std::string> fields;
  size_t maxRows = 32;
  bool authRequired = false;
  std::string error;
};

DiscoverySpec makeSpec(const std::string& providerId, const std::string& appId,
                       const std::string& category) {
  DiscoverySpec s;
  s.request.timeoutMs = 30000;

  if (providerId == "fanqie") {
    const std::string key = category.empty() ? M4NativeProviderExplore::defaultKey(providerId) : category;
    int gender = 0;
    int categoryId = 0;
    if (!M4NativeProviderExplore::decodeFanqieKey(key, gender, categoryId)) {
      s.error = "bad_category";
      return s;
    }
    s.request.url =
        "https://novel.snssdk.com/api/novel/channel/homepage/new_category/book_list/v1/"
        "?parent_enterfrom=novel_channel_category.tab.&aid=1967&offset=0&limit=24&category_id=" +
        std::to_string(categoryId) + "&gender=" + std::to_string(gender);
    s.request.headers = {{"User-Agent", kFanqieUa}, {"Referer", "https://fanqienovel.com/"}};
    s.request.maxBytes = 4u * 1024u * 1024u;
    s.path = {"data", "data"};
    s.fields = {"book_id", "book_name", "author", "_m4_progress"};
    s.maxRows = 24;
    return s;
  }

  if (providerId == "jjwxc") {
    const std::string channel = category.empty() ? M4NativeProviderExplore::defaultKey(providerId) : category;
    if (channel.empty() ||
        !std::all_of(channel.begin(), channel.end(), [](unsigned char c) { return std::isdigit(c); })) {
      s.error = "bad_category";
      return s;
    }
    const std::string body = std::string("{\"") + channel +
                             "\":{\"offset\":\"0\",\"limit\":\"24\"}}";
    s.request.url = std::string(M4_JJWXC_APP_CDN) + "/bookstore/getFullPage?versionCode=148&channelBody=" +
                    urlEncode(body);
    // M4HttpTransport owns the stable request headers. This CDN endpoint
    // hangs under QEMU when the ESP client is given a second User-Agent or
    // Referer header, so keep the request header set minimal.
    s.request.headers.clear();
    s.request.maxBytes = 512u * 1024u;
    s.path = {channel};
    s.fields = {"novelId", "novelName", "authorName", "_m4_progress"};
    s.maxRows = 24;
    return s;
  }

  if (providerId == "legado") {
    // Legado web exposes the bookshelf as a plain JSON array of book objects.
    // bookUrl is a long locator; the shelf extractor below converts it into a
    // short M4-safe bookId and records the locator in the sidecar file.
    // Endpoint is auto-discovered from Wi-Fi-transfer visitor IPs when needed.
    Serial.printf("[M4DBG] legado makeSpec appId=%s\n", appId.c_str());
    if (!M4LegadoBridge::ensureEndpoint(appRoot(appId))) {
      s.error = "legado_endpoint_missing";
      return s;
    }
    s.request.url = M4LegadoBridge::baseUrl() + "/getBookshelf";
    s.request.headers = {{"User-Agent", "Mozilla/5.0 Murphy-M4 NativeProvider/1"}};
    s.request.maxBytes = 4u * 1024u * 1024u;
    s.path = {"data"};
    // totalChapterNum powers progressive catalog placeholders (fast open).
    s.fields = {"bookUrl", "name", "author", "totalChapterNum"};
    s.maxRows = 64;
    return s;
  }

  if (providerId == "weread") {
    std::string cookie;
    const std::string root = appRoot(appId);
    if (!M4NativeProviderIo::loadCookieHeader(root, "weread", cookie)) {
      s.authRequired = true;
      s.error = "login_required";
      return s;
    }
    s.request.url = std::string(M4_WEREAD_ORIGIN) + "/web/shelf/sync";
    s.request.headers = {{"User-Agent", "Mozilla/5.0 Murphy-M4 NativeProvider/1"},
                         {"Referer", "https://weread.qq.com/"}, {"Cookie", cookie}};
    s.request.maxBytes = 2u * 1024u * 1024u;
    s.path = {"books"};
    s.fields = {"bookId", "title", "author", "progress"};
    // First window only. Waiting for a 4096-row / no-Content-Length body
    // wedges QEMU TLS the same way JJWXC did. More rows stay on Refresh.
    s.maxRows = 64;
    return s;
  }

  s.error = "provider_not_supported";
  return s;
}

void taskMain(void*) {
  Snapshot job;
  {
    std::lock_guard<std::mutex> lock(gMu);
    job = gSnapshot;
  }
  Serial.printf("[M4DBG] discovery task provider=%s app=%s cat=%s\n",
                job.providerId.c_str(), job.appId.c_str(), job.category.c_str());

  DiscoverySpec spec = makeSpec(job.providerId, job.appId, job.category);
  if (spec.authRequired) {
    publish(Phase::AuthRequired, 0, 0, spec.error);
  } else if (!spec.error.empty()) {
    publish(Phase::Error, 0, 0, spec.error);
  } else {
    AtomicRowsSink file;
    if (!file.open(rowsPath(job.appId))) {
      writeDiscoveryDiag(job.appId, "sd_open_failed", false, 0, "sd_open_failed", 0, 0, false);
      publish(Phase::Error, 0, 0, "sd_open_failed");
    } else if (job.providerId == "legado") {
      // Stream /getBookshelf data[] and rewrite bookUrl → shortId + sidecar.
      LegadoRewriteSink rewrite(file, appRoot(job.appId));
      M4xJsonStream::RecordExtractor rows(spec.path, spec.fields, rewrite, spec.maxRows);
      RecordExtractorSink jsonSink(rows);
      publish(Phase::Connecting);
      M4NativeProviderHeavyGate::Lock heavy(M4NativeProviderHeavyGate::mutex());
      const auto net = M4NativeProviderHttp::requestToSink(
          spec.request, jsonSink,
          [&](size_t bytes) { publish(Phase::Receiving, bytes, rows.recordCount()); });
      const bool finished = net.ok && rows.finish();
      const size_t rowCount = rewrite.written();
      const bool parsed = finished && rowCount > 0;
      const bool hadSidecar = rewrite.sidecarOpen();
      rewrite.closeSidecar();
      Serial.printf(
          "[M4DBG] legado shelf: net_ok=%d net_bytes=%u net_err=%s ext=%u written=%u skip=%u sidecar=%d json_err=%s\n",
          net.ok ? 1 : 0, static_cast<unsigned>(net.bytes),
          net.error.empty() ? "-" : net.error.c_str(),
          static_cast<unsigned>(rows.recordCount()), static_cast<unsigned>(rowCount),
          static_cast<unsigned>(rewrite.skipped()), hadSidecar ? 1 : 0,
          M4xJsonStream::errorString(rows.error()));
      if (!parsed) {
        file.discard();
        const std::string error =
            !net.ok ? (net.error.empty() ? "discovery_http" : net.error)
                    : (rowCount == 0
                           ? (rows.recordCount() == 0
                                  ? (rows.error() == M4xJsonStream::Error::None ? "discovery_empty"
                                                                               : M4xJsonStream::errorString(rows.error()))
                                  : "discovery_rewrite_empty")
                           : M4xJsonStream::errorString(rows.error()));
        writeDiscoveryDiag(job.appId, "error", net.ok, net.bytes, error, rowCount, rewrite.skipped(),
                           hadSidecar);
        publish(Phase::Error, net.bytes, rowCount, error);
      } else if (!file.commit()) {
        file.discard();
        writeDiscoveryDiag(job.appId, "commit_fail", net.ok, net.bytes, "discovery_commit_failed",
                           rowCount, rewrite.skipped(), hadSidecar);
        publish(Phase::Error, net.bytes, rowCount, "discovery_commit_failed");
      } else {
        writeDiscoveryDiag(job.appId, "ready", net.ok, net.bytes, "-", rowCount, rewrite.skipped(),
                           hadSidecar);
        publish(Phase::Ready, net.bytes, rowCount);
      }
    } else {
      M4xJsonStream::RecordExtractor rows(spec.path, spec.fields, file, spec.maxRows);
      RecordExtractorSink jsonSink(rows);
      publish(Phase::Connecting);
      M4NativeProviderHeavyGate::Lock heavy(M4NativeProviderHeavyGate::mutex());
      bool sawBody = false;
      writeDiscoveryDiag(job.appId, "http_started", false, 0, "-", 0, 0, false);
      const auto net = M4NativeProviderHttp::requestToSink(
          spec.request, jsonSink,
          [&](size_t bytes) {
            publish(Phase::Receiving, bytes, rows.recordCount());
            if (!sawBody) {
              sawBody = true;
              writeDiscoveryDiag(job.appId, "http_progress", false, bytes, "-",
                                 rows.recordCount(), 0, false);
            }
          },
          [&]() { return rows.recordCount() >= spec.maxRows; });
      const bool boundedWindow = rows.recordCount() >= spec.maxRows && net.error == "cancelled";
      const bool parsed = rows.recordCount() > 0 &&
                          ((net.ok && rows.finish()) || boundedWindow);
      const bool wereadLoginExpired =
          job.providerId == "weread" && M4WereadAuthPolicy::responseIndicatesLoginRequired(jsonSink.prefix());
      if (!parsed) {
        file.discard();
        if (wereadLoginExpired) {
          writeDiscoveryDiag(job.appId, "auth", net.ok, net.bytes, "login_required", 0, 0, false);
          publish(Phase::AuthRequired, net.bytes, 0, "login_required");
        } else {
          const std::string error = !net.ok
                                        ? (net.error.empty() ? "discovery_http" : net.error)
                                        : (rows.recordCount() == 0
                                               ? "discovery_empty"
                                               : M4xJsonStream::errorString(rows.error()));
          if (job.providerId == "weread" &&
              (error == "http_401" || error == "http_403" || error == "login_required")) {
            writeDiscoveryDiag(job.appId, "auth", net.ok, net.bytes, error, rows.recordCount(), 0, false);
            publish(Phase::AuthRequired, net.bytes, 0, error);
          } else {
            writeDiscoveryDiag(job.appId, "error", net.ok || boundedWindow, net.bytes, error,
                               rows.recordCount(), 0, false);
            publish(Phase::Error, net.bytes, rows.recordCount(), error);
          }
        }
      } else if (!file.commit()) {
        file.discard();
        writeDiscoveryDiag(job.appId, "commit_fail", net.ok || boundedWindow, net.bytes,
                           "discovery_commit_failed",
                           rows.recordCount(), 0, false);
        publish(Phase::Error, net.bytes, rows.recordCount(), "discovery_commit_failed");
      } else {
        writeDiscoveryDiag(job.appId, "ready", net.ok || boundedWindow, net.bytes, "-",
                           rows.recordCount(), 0, false);
        publish(Phase::Ready, net.bytes, rows.recordCount());
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

bool startCategory(const std::string& providerId, const std::string& appId,
                   const std::string& category) {
  if ((providerId != "fanqie" && providerId != "jjwxc" && providerId != "weread" &&
       providerId != "legado") ||
      appId.empty()) {
    return false;
  }
  bool expected = false;
  if (!gBusy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return false;
  {
    std::lock_guard<std::mutex> lock(gMu);
    gSnapshot = {};
    gSnapshot.phase = Phase::Connecting;
    gSnapshot.providerId = providerId;
    gSnapshot.appId = appId;
    gSnapshot.category = category;
    gSnapshot.startedMs = millis();
    gSnapshot.updatedMs = gSnapshot.startedMs;
  }
  TaskHandle_t handle = nullptr;
  // Match NativeProvider worker stack budget: mbedTLS + HTTP client + JSON
  // stream need far more than 24KB. Undersized stacks corrupt under QEMU/device
  // and look like a frozen guest after fanqie/jjwxc discovery starts.
  constexpr uint32_t kDiscoveryStackBytes = 72u * 1024u;
  // Priority 0 (below Arduino loopTask) so TLS/HTTP cannot starve m4adb/UI.
  if (M4Psram::createTask(taskMain, "NativeDiscovery", kDiscoveryStackBytes, nullptr, 0,
                          &handle) != pdPASS) {
    gBusy.store(false, std::memory_order_release);
    publish(Phase::Error, 0, 0, "discovery_task_create");
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(gMu);
    gTask = handle;
  }
  return true;
}

bool startDefault(const std::string& providerId, const std::string& appId) {
  const std::string category = M4NativeProviderExplore::defaultKey(providerId);
  return startCategory(providerId, appId, category);
}

Snapshot snapshot() {
  std::lock_guard<std::mutex> lock(gMu);
  return gSnapshot;
}

bool busy() { return gBusy.load(std::memory_order_acquire); }

}  // namespace M4NativeProviderDiscovery
