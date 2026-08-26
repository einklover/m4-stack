#include "apps/providers/M4NativeProviderManager.h"

#include "apps/M4ContentProviderCatalog.h"
#include "apps/M4ContentProviderSession.h"
#include "apps/providers/M4NativeProviderAdapters.h"
#include "apps/providers/M4NativeProviderHeavyGate.h"
#include "apps/providers/M4NativeProviderIo.h"
#include "apps/providers/M4LegadoTocPolicy.h"
#include "apps/providers/M4Psram.h"
#include "util/M4PluginReaderBridge.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <SDCardManager.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

namespace M4NativeProviderManager {
namespace {

bool cacheUsable(const std::string& providerId, const std::string& absPath,
                 size_t* sizeOut) {
  if (providerId != "jjwxc") return M4NativeProviderIo::cacheComplete(absPath, sizeOut);
  if (M4NativeProviderIo::cacheVerified(absPath, sizeOut)) return true;
  // Existing JJWXC markers only said "some bytes were written". Replace them
  // once and drop the matching page index, which may describe a truncated or
  // incorrectly indexed generation.
  size_t legacySize = 0;
  if (M4NativeProviderIo::cacheComplete(absPath, &legacySize)) {
    (void)M4NativeProviderIo::clearCacheArtifacts(absPath);
  }
  return false;
}

struct StoredBook {
  M4ContentProvider::BookSpec spec;
  std::string appDataRoot;
};

std::mutex gMu;
std::unordered_map<std::string, StoredBook> gBooks;
M4NativeProvider::Progress gProgress;
std::atomic<bool> gCancel{false};
TaskHandle_t gWorker = nullptr;

// Legacy catalogs have no persisted row count. Keep the compatibility probe
// bounded so a cold history reopen cannot scan a multi-megabyte TOC on the UI
// task; the native catalog worker remains responsible for the full fetch.
constexpr size_t kLegacyUiScanMaxBytes = 256u * 1024u;

std::string keyOf(const std::string& p, const std::string& b) { return p + "\n" + b; }

std::string defaultAppId(const std::string& p) {
  if (p == "fanqie") return "com.fanqie.client";
  if (p == "jjwxc") return "com.jjwxc.client";
  if (p == "weread") return "com.weread.client";
  return {};
}

std::string appRoot(const std::string& appId) {
  return appId.empty() ? std::string() : std::string("/apps_data/") + appId;
}

std::string safeBookFile(const StoredBook& b) {
  if (b.appDataRoot.empty()) return {};
  return b.appDataRoot + "/provider/books/" + b.spec.bookId + "/book.json";
}

bool readSmall(const std::string& path, std::string& out, size_t cap = 32u * 1024u) {
  out.clear();
  FsFile f;
  if (!SdMan.openFileForRead("NP-STORE", path.c_str(), f)) return false;
  const size_t n = f.fileSize();
  if (n == 0 || n > cap) {
    f.close();
    return false;
  }
  out.resize(n);
  size_t off = 0;
  while (off < n) {
    const int r = f.read(reinterpret_cast<uint8_t*>(&out[off]), n - off);
    if (r <= 0) break;
    off += static_cast<size_t>(r);
  }
  f.close();
  if (off != n) {
    out.clear();
    return false;
  }
  return true;
}

bool writeExact(const std::string& path, const std::string& body) {
  if (!M4NativeProviderIo::ensureParentDirs(path)) return false;
  const std::string tmp = path + ".tmp";
  if (SdMan.exists(tmp.c_str())) SdMan.remove(tmp.c_str());
  FsFile f;
  if (!SdMan.openFileForWrite("NP-STORE", tmp.c_str(), f)) return false;
  size_t off = 0;
  while (off < body.size()) {
    const size_t n = std::min<size_t>(4096, body.size() - off);
    const int w = f.write(reinterpret_cast<const uint8_t*>(body.data() + off), n);
    if (w <= 0) {
      f.close();
      SdMan.remove(tmp.c_str());
      return false;
    }
    off += static_cast<size_t>(w);
  }
  f.close();
  if (SdMan.exists(path.c_str())) SdMan.remove(path.c_str());
  if (!SdMan.rename(tmp.c_str(), path.c_str())) {
    SdMan.remove(tmp.c_str());
    return false;
  }
  return true;
}

bool persist(const StoredBook& b) {
  if (b.spec.catalog.kind != M4ContentProvider::ChapterCatalogKind::FileRows) return false;
  JsonDocument doc;
  doc["version"] = 1;
  doc["providerId"] = b.spec.providerId;
  doc["appId"] = b.spec.appId;
  doc["bookId"] = b.spec.bookId;
  doc["title"] = b.spec.title;
  doc["currentIndex"] = b.spec.currentIndex0;
  JsonObject c = doc["catalog"].to<JsonObject>();
  c["path"] = b.spec.catalog.fileRelPath;
  c["count"] = b.spec.catalog.chapterCount;
  c["uidField"] = b.spec.catalog.uidField0;
  c["titleField"] = b.spec.catalog.titleField0;
  c["vipField"] = b.spec.catalog.vipField0;
  JsonObject cp = doc["cachePolicy"].to<JsonObject>();
  cp["maxChapterBytes"] = b.spec.cachePolicy.maxChapterBytes;
  cp["prefetchAhead"] = b.spec.cachePolicy.prefetchAhead;
  cp["retainBehind"] = b.spec.cachePolicy.retainBehind;
  cp["maxReadyChapters"] = b.spec.cachePolicy.maxReadyChapters;
  cp["offlineReopen"] = b.spec.cachePolicy.offlineReopen;
  std::string body;
  serializeJson(doc, body);
  return body.size() <= 16u * 1024u && writeExact(safeBookFile(b), body);
}

bool loadPersisted(const std::string& providerId, const std::string& bookId,
                   const std::string& appIdHint, StoredBook& out) {
  const std::string appId = appIdHint.empty() ? defaultAppId(providerId) : appIdHint;
  if (appId.empty()) return false;
  out = {};
  out.appDataRoot = appRoot(appId);
  out.spec.providerId = providerId;
  out.spec.bookId = bookId;
  out.spec.appId = appId;
  std::string raw;
  if (!readSmall(safeBookFile(out), raw)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, raw)) return false;
  if (std::string(doc["providerId"] | "") != providerId || std::string(doc["bookId"] | "") != bookId) return false;
  out.spec.title = doc["title"] | "";
  out.spec.currentIndex0 = doc["currentIndex"] | 0;
  JsonObject c = doc["catalog"].as<JsonObject>();
  out.spec.catalog.kind = M4ContentProvider::ChapterCatalogKind::FileRows;
  out.spec.catalog.fileRelPath = c["path"] | "";
  out.spec.catalog.chapterCount = c["count"] | 0;
  out.spec.catalog.uidField0 = c["uidField"] | 0;
  out.spec.catalog.titleField0 = c["titleField"] | 1;
  out.spec.catalog.vipField0 = c["vipField"] | -1;
  JsonObject cp = doc["cachePolicy"].as<JsonObject>();
  if (!cp.isNull()) {
    out.spec.cachePolicy.maxChapterBytes = cp["maxChapterBytes"] | (2u * 1024u * 1024u);
    out.spec.cachePolicy.prefetchAhead = cp["prefetchAhead"] | 1;
    out.spec.cachePolicy.retainBehind = cp["retainBehind"] | 1;
    out.spec.cachePolicy.maxReadyChapters = cp["maxReadyChapters"] | 4;
    out.spec.cachePolicy.offlineReopen = cp["offlineReopen"] | true;
  }
  return M4ContentProvider::validateBookSpec(out.spec) == M4ContentProvider::ValidationError::None;
}

size_t countLines(const std::string& path, size_t cap = M4ContentProvider::kMaxCatalogChapters) {
  FsFile f;
  if (!SdMan.openFileForRead("NP-TOC", path.c_str(), f)) return 0;
  if (f.fileSize() > kLegacyUiScanMaxBytes) {
    f.close();
    return 0;
  }
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
      if (buf[i] == '\n' && ++rows >= cap) {
        f.close();
        return rows;
      }
    }
  }
  f.close();
  if (any && last != '\n' && rows < cap) ++rows;
  return rows;
}

bool inferLegacy(const std::string& providerId, const std::string& bookId,
                 const std::string& appIdHint, const std::string& titleHint, StoredBook& out) {
  const std::string appId = appIdHint.empty() ? defaultAppId(providerId) : appIdHint;
  if (appId.empty()) return false;
  out = {};
  out.appDataRoot = appRoot(appId);
  out.spec.providerId = providerId;
  out.spec.bookId = bookId;
  out.spec.appId = appId;
  out.spec.title = titleHint.empty() ? bookId : titleHint;
  out.spec.catalog.kind = M4ContentProvider::ChapterCatalogKind::FileRows;

  const std::string base = out.appDataRoot + "/cache/" + bookId;
  std::string raw;
  if (readSmall(base + "/toc_catalog.json", raw)) {
    JsonDocument doc;
    if (!deserializeJson(doc, raw)) {
      out.spec.catalog.fileRelPath = doc["source"] | (std::string("cache/") + bookId + "/toc_rows.txt");
      out.spec.catalog.chapterCount = doc["count"] | 0;
      out.spec.catalog.uidField0 = doc["uid_field"] | 0;
      out.spec.catalog.titleField0 = doc["title_field"] | 1;
    }
  }
  if (out.spec.catalog.fileRelPath.empty()) out.spec.catalog.fileRelPath = "cache/" + bookId + "/toc_rows.txt";
  if (out.spec.catalog.chapterCount == 0) {
    out.spec.catalog.chapterCount = countLines(out.appDataRoot + "/" + out.spec.catalog.fileRelPath);
  }
  if (providerId == "jjwxc") out.spec.catalog.vipField0 = 3;
  if (out.spec.catalog.chapterCount == 0) return false;
  out.spec.currentIndex0 = 0;
  if (M4ContentProvider::validateBookSpec(out.spec) != M4ContentProvider::ValidationError::None) return false;
  (void)persist(out);
  return true;
}

bool getBook(const std::string& providerId, const std::string& bookId, StoredBook& out) {
  std::lock_guard<std::mutex> lock(gMu);
  auto it = gBooks.find(keyOf(providerId, bookId));
  if (it == gBooks.end()) return false;
  out = it->second;
  return true;
}

bool readCatalogLine(const StoredBook& b, int index0, std::string& line) {
  line.clear();
  if (index0 < 0 || static_cast<size_t>(index0) >= b.spec.catalog.chapterCount) return false;
  const std::string path = b.appDataRoot + "/" + b.spec.catalog.fileRelPath;
  FsFile f;
  if (!SdMan.openFileForRead("NP-ROW", path.c_str(), f)) return false;
  constexpr size_t kMaxLine = 2048;
  int row = 0;
  uint8_t buf[1024];
  while (f.available()) {
    const int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    for (int i = 0; i < n; ++i) {
      const char c = static_cast<char>(buf[i]);
      if (row == index0) {
        if (c == '\n') {
          f.close();
          if (!line.empty() && line.back() == '\r') line.pop_back();
          return !line.empty();
        }
        if (line.size() >= kMaxLine) {
          f.close();
          return false;
        }
        line.push_back(c);
      } else if (c == '\n') {
        ++row;
        if (row > index0) {
          f.close();
          return false;
        }
      }
    }
  }
  f.close();
  return row == index0 && !line.empty();
}

bool resolveChapter(const StoredBook& b, int index0, M4ContentProvider::ChapterMeta& ch,
                    std::string& rawLine) {
  ch = {};
  rawLine.clear();
  if (b.spec.catalog.kind == M4ContentProvider::ChapterCatalogKind::Inline) {
    if (index0 < 0 || static_cast<size_t>(index0) >= b.spec.chapters.size()) return false;
    ch = b.spec.chapters[static_cast<size_t>(index0)];
    return true;
  }
  if (!readCatalogLine(b, index0, rawLine)) return false;
  if (!M4ContentProviderCatalog::fieldAt(rawLine, b.spec.catalog.uidField0, ch.uid) || ch.uid.empty()) return false;
  if (b.spec.catalog.titleField0 >= 0) {
    (void)M4ContentProviderCatalog::fieldAt(rawLine, b.spec.catalog.titleField0, ch.title);
  }
  return M4ContentProvider::idOk(ch.uid.c_str(), M4ContentProvider::kMaxChapterUidLen);
}

void updateProgress(const M4NativeProvider::Progress& p) {
  std::lock_guard<std::mutex> lock(gMu);
  gProgress = p;
}

void setPhase(const StoredBook& b, const M4ContentProvider::ChapterMeta& ch, int index0,
              M4NativeProvider::Phase phase, size_t recv, size_t written, int pct,
              const std::string& error = {}) {
  M4NativeProvider::Progress p;
  {
    std::lock_guard<std::mutex> lock(gMu);
    p = gProgress;
  }
  if (p.startedMs == 0 || p.providerId != b.spec.providerId || p.bookId != b.spec.bookId ||
      p.chapterIndex0 != index0) {
    p.startedMs = millis();
  }
  p.providerId = b.spec.providerId;
  p.bookId = b.spec.bookId;
  p.chapterUid = ch.uid;
  p.chapterIndex0 = index0;
  p.phase = phase;
  p.receivedBytes = recv;
  p.writtenBytes = written;
  p.percent = std::max(0, std::min(100, pct));
  p.updatedMs = millis();
  p.error = error;
  updateProgress(p);
}

void processWork(const M4ContentProvider::PrefetchWork& w) {
  // Keep breadcrumbs at the hand-off points, but let the provider/HTTP layer
  // turn a bad heap into a visible error. Returning here would leave the
  // native loading page spinning forever with no ChapterStatus update.
  (void)M4NativeProviderHeavyGate::heapHealthy(0x100);
  StoredBook b;
  if (!getBook(w.providerId, w.bookId, b)) {
    if (!ensureBook(w.providerId, w.bookId) || !getBook(w.providerId, w.bookId, b)) {
      return;
    }
  }
  M4ContentProvider::ChapterMeta ch;
  std::string rawLine;
  if (!resolveChapter(b, w.index0, ch, rawLine)) {
    M4ContentProvider::ChapterStatus st;
    st.providerId = w.providerId;
    st.bookId = w.bookId;
    st.index0 = w.index0;
    st.state = M4ContentProvider::ChapterReady::Error;
    st.error = "catalog_resolve";
    (void)M4ContentProviderSession::setChapterStatus(st);
    setPhase(b, ch, w.index0, M4NativeProvider::Phase::Error, 0, 0, 0, st.error);
    return;
  }
  (void)M4NativeProviderHeavyGate::heapHealthy(0x110);

  const std::string rel = chapterRelPath(b.spec.bookId, ch.uid);
  const std::string abs = b.appDataRoot + "/" + rel;
  size_t cached = 0;
  if (cacheUsable(b.spec.providerId, abs, &cached)) {
    M4ContentProvider::ChapterStatus st;
    st.providerId = b.spec.providerId;
    st.bookId = b.spec.bookId;
    st.chapterUid = ch.uid;
    st.index0 = w.index0;
    st.state = M4ContentProvider::ChapterReady::Ready;
    st.cacheRelPath = rel;
    st.pct = 100;
    (void)M4ContentProviderSession::setChapterStatus(st);
    setPhase(b, ch, w.index0, M4NativeProvider::Phase::Ready, 0, cached, 100);
    return;
  }

  M4ContentProvider::ChapterStatus fetching;
  fetching.providerId = b.spec.providerId;
  fetching.bookId = b.spec.bookId;
  fetching.chapterUid = ch.uid;
  fetching.index0 = w.index0;
  fetching.state = M4ContentProvider::ChapterReady::Fetching;
  fetching.pct = 1;
  (void)M4ContentProviderSession::setChapterStatus(fetching);
  setPhase(b, ch, w.index0, M4NativeProvider::Phase::Resolving, 0, 0, 1);

  auto adapter = M4NativeProviderAdapters::create(b.spec.providerId);
  if (!adapter) {
    fetching.state = M4ContentProvider::ChapterReady::Error;
    fetching.error = "provider_not_supported";
    (void)M4ContentProviderSession::setChapterStatus(fetching);
    setPhase(b, ch, w.index0, M4NativeProvider::Phase::Error, 0, 0, 0, fetching.error);
    return;
  }
  (void)M4NativeProviderHeavyGate::heapHealthy(0x120);

  M4NativeProvider::ChapterRequest req;
  req.book = b.spec;
  req.chapter = ch;
  req.chapterIndex0 = w.index0;
  req.appDataRoot = b.appDataRoot;
  req.cacheRelPath = rel;
  req.cacheAbsPath = abs;
  req.catalogRawLine = rawLine;
  gCancel.store(false, std::memory_order_release);
  int lastStatusPct = -1;
  const auto progressFn = [&](M4NativeProvider::Phase phase, size_t recv, size_t written, int pct) {
    setPhase(b, ch, w.index0, phase, recv, written, pct);
    // ChapterStatus is polled by the UI; avoid rewriting the map on every
    // network/decode chunk (string copies fragment the internal heap).
    if (pct == lastStatusPct && phase != M4NativeProvider::Phase::Ready &&
        phase != M4NativeProvider::Phase::Error) {
      return;
    }
    lastStatusPct = pct;
    M4ContentProvider::ChapterStatus s = fetching;
    s.state = M4ContentProvider::ChapterReady::Fetching;
    s.pct = pct;
    (void)M4ContentProviderSession::setChapterStatus(s);
  };
  const auto cancelledFn = []() { return gCancel.load(std::memory_order_acquire); };

  // TLS handshakes, provider decode and cache commit are one heavy transient
  // stage. Serialize them process-wide so login/discovery cannot overlap the
  // internal-RAM spike even though payload buffers themselves live on SD/PSRAM.
  M4NativeProviderHeavyGate::Lock heavy(M4NativeProviderHeavyGate::mutex());
  (void)M4NativeProviderHeavyGate::heapHealthy(0x130);
  auto result = adapter->fetchChapter(req, progressFn, cancelledFn);

  // A previous implementation could leave a final file/marker/part file in
  // an inconsistent combination. Retry exactly once after removing every
  // generation. Do not retry network/auth/parser failures here.
  if (!result.ok && result.error == "cache_commit_failed" && !cancelledFn()) {
    (void)M4NativeProviderIo::clearCacheArtifacts(abs);
    result = adapter->fetchChapter(req, progressFn, cancelledFn);
  }
  // Network/protocol errors are terminal for this work item. Retrying a whole
  // multi-hop chapter in the same worker hid the first useful error and could
  // corrupt lwIP state; foreground UI retry creates one clean load instead.

  M4ContentProvider::ChapterStatus st;
  st.providerId = b.spec.providerId;
  st.bookId = b.spec.bookId;
  st.chapterUid = ch.uid;
  st.index0 = w.index0;
  if (result.ok) {
    st.state = M4ContentProvider::ChapterReady::Ready;
    st.cacheRelPath = result.cacheRelPath.empty() ? rel : result.cacheRelPath;
    st.pct = 100;
    (void)M4ContentProviderSession::setChapterStatus(st);
    setPhase(b, ch, w.index0, M4NativeProvider::Phase::Ready, result.bytes, result.bytes, 100);
  } else {
    st.state = M4ContentProvider::ChapterReady::Error;
    st.error = result.error.empty() ? "fetch_failed" : result.error;
    (void)M4ContentProviderSession::setChapterStatus(st);
    setPhase(b, ch, w.index0,
             result.authRequired ? M4NativeProvider::Phase::AuthRequired
                                 : (st.error == "cancelled" ? M4NativeProvider::Phase::Cancelled
                                                            : M4NativeProvider::Phase::Error),
             0, 0, 0, st.error);
  }
}

void workerMain(void*) {
  uint32_t idleStarted = millis();
  while (true) {
    const auto w = M4ContentProviderSession::pollWork();
    if (w.valid && supports(w.providerId)) {
      idleStarted = millis();
      processWork(w);
      continue;
    }
    if (millis() - idleStarted >= 1200) {
      std::lock_guard<std::mutex> lock(gMu);
      if (M4ContentProviderSession::pendingWorkCount() == 0) {
        gWorker = nullptr;
        break;
      }
      idleStarted = millis();
    }
    vTaskDelay(pdMS_TO_TICKS(40));
  }
  // Stack was allocated via M4Psram::createTask (PSRAM-first).
  M4Psram::deleteTask(nullptr);
}

void kickWorker() {
  std::lock_guard<std::mutex> lock(gMu);
  if (gWorker) return;
  // ESP-IDF FreeRTOS stack depth is in *bytes*. WeRead chapter fetch
  // (M4HttpTransport session + esp_http_client/mbedTLS + SD decode) blew a
  // 48KB PSRAM stack: panic IllegalInstruction with provider_stage=0x310
  // (tlsBlockAvailable heap walk after corruption). http_probe on the main
  // loop succeeded with the same TLS path — only the worker stack was short.
  // Keep stack in PSRAM so internal RAM still has a contiguous TLS block.
  constexpr uint32_t kWorkerStackBytes = 72u * 1024u;
  const BaseType_t ok =
      M4Psram::createTask(workerMain, "NativeProvider", kWorkerStackBytes, nullptr, 1, &gWorker);
  if (ok != pdPASS) gWorker = nullptr;
}

}  // namespace

bool supports(const std::string& providerId) {
  return providerId == "fanqie" || providerId == "jjwxc" || providerId == "weread" ||
         providerId == "legado";
}

bool registerBook(const M4ContentProvider::BookSpec& spec) {
  if (!supports(spec.providerId) ||
      M4ContentProvider::validateBookSpec(spec) != M4ContentProvider::ValidationError::None || spec.appId.empty()) {
    return false;
  }
  StoredBook b;
  b.spec = spec;
  b.appDataRoot = appRoot(spec.appId);
  {
    std::lock_guard<std::mutex> lock(gMu);
    gBooks[keyOf(spec.providerId, spec.bookId)] = b;
  }
  (void)M4ContentProviderSession::registerBook(spec);
  const bool ok = persist(b);
  kickWorker();
  return ok;
}

bool ensureBook(const std::string& providerId, const std::string& bookId,
                const std::string& appId, const std::string& title) {
  if (!supports(providerId) || !M4ContentProvider::idOk(bookId.c_str(), M4ContentProvider::kMaxBookIdLen)) return false;
  {
    std::lock_guard<std::mutex> lock(gMu);
    if (gBooks.find(keyOf(providerId, bookId)) != gBooks.end()) return true;
  }
  StoredBook b;
  if (!loadPersisted(providerId, bookId, appId, b) && !inferLegacy(providerId, bookId, appId, title, b)) return false;
  if (!title.empty() && (b.spec.title.empty() || b.spec.title == b.spec.bookId)) b.spec.title = title;
  // Legado-only TOC consistency gate: a stale shelf can persist a chapter
  // count larger than what toc_rows.txt actually holds (interrupted refill).
  // Clamp to readable rows and re-persist; zero readable rows fails the load
  // so the caller falls back to a fresh catalog bootstrap instead of opening
  // a hollow TOC.
  if (providerId == "legado" &&
      b.spec.catalog.kind == M4ContentProvider::ChapterCatalogKind::FileRows &&
      !b.spec.catalog.fileRelPath.empty() && b.spec.catalog.chapterCount > 0) {
    const size_t actualRows = countLines(b.appDataRoot + "/" + b.spec.catalog.fileRelPath);
    const size_t clamped = M4LegadoTocPolicy::clampedChapterCount(b.spec.catalog.chapterCount, actualRows);
    if (clamped != b.spec.catalog.chapterCount) {
      Serial.printf("[NativeStore] legado count %u->%u rows=%u book=%s\n",
                    static_cast<unsigned>(b.spec.catalog.chapterCount),
                    static_cast<unsigned>(clamped), static_cast<unsigned>(actualRows),
                    bookId.c_str());
      if (clamped == 0) return false;
      b.spec.catalog.chapterCount = clamped;
      if (!persist(b)) return false;
    }
  }
  {
    std::lock_guard<std::mutex> lock(gMu);
    gBooks[keyOf(providerId, bookId)] = b;
  }
  (void)M4ContentProviderSession::registerBook(b.spec);
  kickWorker();
  return true;
}

bool findChapterIndex(const std::string& providerId, const std::string& bookId,
                      const std::string& chapterUid, int& indexOut) {
  indexOut = -1;
  if (chapterUid.empty()) return false;
  StoredBook b;
  if (!getBook(providerId, bookId, b) ||
      b.spec.catalog.kind != M4ContentProvider::ChapterCatalogKind::FileRows) {
    return false;
  }

  const std::string path = b.appDataRoot + "/" + b.spec.catalog.fileRelPath;
  FsFile f;
  if (!SdMan.openFileForRead("NP-HISTORY", path.c_str(), f)) return false;
  if (f.fileSize() > kLegacyUiScanMaxBytes) {
    f.close();
    return false;
  }
  std::string line;
  line.reserve(256);
  int row = 0;
  bool found = false;
  uint8_t buf[1024];
  while (f.available() && static_cast<size_t>(row) < b.spec.catalog.chapterCount) {
    const int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    for (int i = 0; i < n; ++i) {
      const char c = static_cast<char>(buf[i]);
      if (c != '\n') {
        if (line.size() < 2048) line.push_back(c);
        continue;
      }
      if (!line.empty() && line.back() == '\r') line.pop_back();
      std::string uid;
      if (M4ContentProviderCatalog::fieldAt(line, b.spec.catalog.uidField0, uid) && uid == chapterUid) {
        indexOut = row;
        found = true;
        break;
      }
      line.clear();
      ++row;
    }
    if (found) break;
  }
  if (!found && !line.empty() && static_cast<size_t>(row) < b.spec.catalog.chapterCount) {
    std::string uid;
    if (M4ContentProviderCatalog::fieldAt(line, b.spec.catalog.uidField0, uid) && uid == chapterUid) {
      indexOut = row;
      found = true;
    }
  }
  f.close();
  return found;
}

bool requestChapter(const std::string& providerId, const std::string& bookId, int index0,
                    LoadIntent intent) {
  const bool foreground = intent == LoadIntent::Foreground;
  if (!ensureBook(providerId, bookId)) return false;
  StoredBook b;
  if (!getBook(providerId, bookId, b) || index0 < 0 || static_cast<size_t>(index0) >= M4ContentProvider::bookChapterCount(b.spec)) {
    return false;
  }

  // File-row catalogs can be large. Resolve the UID and cache path in the
  // provider worker instead of scanning from row zero here when a user taps a
  // late chapter or advances from the reader. A persisted Ready cache is the
  // only fast-path that needs to be inspected on the UI task.
  if (b.spec.catalog.kind == M4ContentProvider::ChapterCatalogKind::FileRows) {
    M4ContentProvider::ChapterStatus cur =
        M4ContentProviderSession::chapterAt(providerId, bookId, index0);
    if (!foreground && cur.state == M4ContentProvider::ChapterReady::Error) return false;
    if (cur.state == M4ContentProvider::ChapterReady::Ready) {
      size_t cached = 0;
      const bool usable = M4ContentProvider::isSafeCacheRelPath(cur.cacheRelPath.c_str()) &&
                          cacheUsable(providerId, b.appDataRoot + "/" + cur.cacheRelPath, &cached);
      if (usable) return true;
      cur.state = M4ContentProvider::ChapterReady::Missing;
      cur.cacheRelPath.clear();
      cur.error.clear();
      cur.pct = 0;
      (void)M4ContentProviderSession::setChapterStatus(cur);
    } else if (foreground && cur.state == M4ContentProvider::ChapterReady::Error) {
      cur.state = M4ContentProvider::ChapterReady::Missing;
      cur.cacheRelPath.clear();
      cur.error.clear();
      cur.pct = 0;
      (void)M4ContentProviderSession::setChapterStatus(cur);
    }
    if (foreground) gCancel.store(false, std::memory_order_release);
    const bool queued = M4ContentProviderSession::requestPrefetch(providerId, bookId, index0);
    kickWorker();
    return queued;
  }

  M4ContentProvider::ChapterMeta ch;
  std::string raw;
  if (!resolveChapter(b, index0, ch, raw)) return false;
  const std::string rel = chapterRelPath(bookId, ch.uid);
  const std::string abs = b.appDataRoot + "/" + rel;
  size_t n = 0;
  if (cacheUsable(providerId, abs, &n)) {
    M4ContentProvider::ChapterStatus st;
    st.providerId = providerId;
    st.bookId = bookId;
    st.chapterUid = ch.uid;
    st.index0 = index0;
    st.state = M4ContentProvider::ChapterReady::Ready;
    st.cacheRelPath = rel;
    st.pct = 100;
    (void)M4ContentProviderSession::setChapterStatus(st);
    setPhase(b, ch, index0, M4NativeProvider::Phase::Ready, 0, n, 100);
    return true;
  }
  // Session may still say Ready/Error after SD cache was deleted (invalidate
  // or failed decode). requestPrefetch refuses Ready — reset first.
  {
    M4ContentProvider::ChapterStatus cur =
        M4ContentProviderSession::chapterAt(providerId, bookId, index0);
    if (!foreground && cur.state == M4ContentProvider::ChapterReady::Error) return false;
    if (cur.state == M4ContentProvider::ChapterReady::Ready ||
        (foreground && cur.state == M4ContentProvider::ChapterReady::Error)) {
      cur.state = M4ContentProvider::ChapterReady::Missing;
      cur.cacheRelPath.clear();
      cur.error.clear();
      cur.pct = 0;
      cur.chapterUid = ch.uid;
      cur.index0 = index0;
      (void)M4ContentProviderSession::setChapterStatus(cur);
    }
  }
  if (foreground) gCancel.store(false, std::memory_order_release);
  const bool queued = M4ContentProviderSession::requestPrefetch(providerId, bookId, index0);
  kickWorker();
  return queued;
}

bool ensureChapter(const std::string& providerId, const std::string& bookId, int index0,
                   bool foreground) {
  return requestChapter(providerId, bookId, index0,
                        foreground ? LoadIntent::Foreground : LoadIntent::Prefetch);
}

bool invalidateChapterCache(const std::string& providerId, const std::string& bookId, int index0) {
  StoredBook b;
  if (!getBook(providerId, bookId, b)) return false;

  if (b.spec.catalog.kind == M4ContentProvider::ChapterCatalogKind::FileRows) {
    M4ContentProvider::ChapterStatus cur =
        M4ContentProviderSession::chapterAt(providerId, bookId, index0);
    std::string rel = cur.cacheRelPath;
    if (rel.empty() && !cur.chapterUid.empty()) rel = chapterRelPath(bookId, cur.chapterUid);
    bool cleared = true;
    if (!rel.empty() && M4ContentProvider::isSafeCacheRelPath(rel.c_str())) {
      cleared = M4NativeProviderIo::clearCacheArtifacts(b.appDataRoot + "/" + rel);
    }
    cur.providerId = providerId;
    cur.bookId = bookId;
    cur.index0 = index0;
    cur.state = M4ContentProvider::ChapterReady::Missing;
    cur.cacheRelPath.clear();
    cur.error.clear();
    cur.pct = 0;
    (void)M4ContentProviderSession::setChapterStatus(cur);
    // A file-row cache without a known UID is cleaned by the worker after its
    // bounded catalog resolve; do not block the retry tap on that scan.
    return cleared;
  }

  M4ContentProvider::ChapterMeta ch;
  std::string raw;
  if (!resolveChapter(b, index0, ch, raw)) return false;
  const bool cleared = M4NativeProviderIo::clearCacheArtifacts(
      b.appDataRoot + "/" + chapterRelPath(bookId, ch.uid));
  M4ContentProvider::ChapterStatus cur;
  cur.providerId = providerId;
  cur.bookId = bookId;
  cur.chapterUid = ch.uid;
  cur.index0 = index0;
  cur.state = M4ContentProvider::ChapterReady::Missing;
  (void)M4ContentProviderSession::setChapterStatus(cur);
  return cleared;
}

M4NativeProvider::Progress progress() {
  std::lock_guard<std::mutex> lock(gMu);
  return gProgress;
}

void acknowledgeAuth(const std::string& providerId) {
  std::lock_guard<std::mutex> lock(gMu);
  if (gProgress.providerId == providerId &&
      gProgress.phase == M4NativeProvider::Phase::AuthRequired) {
    gProgress = {};
  }
}

void cancelForeground() { gCancel.store(true, std::memory_order_release); }

std::string appDataRootFor(const std::string& providerId, const std::string& bookId) {
  StoredBook b;
  if (getBook(providerId, bookId, b)) return b.appDataRoot;
  return appRoot(defaultAppId(providerId));
}

std::string chapterRelPath(const std::string& bookId, const std::string& chapterUid) {
  return std::string("cache/") + bookId + "/ch_" + chapterUid + ".txt";
}

void begin() { kickWorker(); }

}  // namespace M4NativeProviderManager
