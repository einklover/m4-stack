#include "apps/M4HttpTransportProbe.h"

#include "apps/M4xPsvtsExtract.h"
#include "apps/providers/M4NativeProviderIo.h"
#include "apps/providers/M4NativeProviderManager.h"
#include "apps/M4ContentProviderSession.h"
#include "apps/weread/WereadCrypto.h"

#include <Arduino.h>
#include <SDCardManager.h>
#include <esp_random.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include <time.h>

namespace M4HttpTransportProbe {
namespace {

constexpr const char* kDefaultAppId = "com.weread.client";
constexpr const char* kLogPath = "/apps_data/com.weread.client/logs/http_transport.log";
constexpr const char* kProbeDir = "/apps_data/com.weread.client/logs";
constexpr const char* kShardPath = "/apps_data/com.weread.client/logs/probe_shard.bin";

char gLastPsvts[M4xPsvts::kMaxValueLen + 1] = {};

class DiscardSink final : public M4xJsonStream::Sink {
 public:
  bool write(const uint8_t* data, size_t len) override {
    if (!data && len) return false;
    bytes_ += len;
    // Keep a small printable prefix for empty_content / routing diagnosis.
    for (size_t i = 0; i < len && prefixLen_ < sizeof(prefix_) - 1; ++i) {
      const unsigned char c = data[i];
      prefix_[prefixLen_++] = (c >= 32 && c < 127) ? static_cast<char>(c) : '.';
    }
    prefix_[prefixLen_] = 0;
    return true;
  }
  size_t bytes() const { return bytes_; }
  const char* prefix() const { return prefix_; }

 private:
  size_t bytes_ = 0;
  char prefix_[48] = {};
  size_t prefixLen_ = 0;
};

class FileSink final : public M4xJsonStream::Sink {
 public:
  bool open(const char* path) {
    close();
    if (!path || !path[0]) return false;
    SdMan.mkdir("/apps_data", true);
    SdMan.mkdir("/apps_data/com.weread.client", true);
    SdMan.mkdir(kProbeDir, true);
    if (SdMan.exists(path)) SdMan.remove(path);
    open_ = SdMan.openFileForWrite("HttpProbe", path, f_);
    bytes_ = 0;
    prefixLen_ = 0;
    prefix_[0] = 0;
    return open_;
  }
  bool write(const uint8_t* data, size_t len) override {
    if (!open_ || !data) return false;
    if (len == 0) return true;
    if (f_.write(data, len) != len) return false;
    bytes_ += len;
    for (size_t i = 0; i < len && prefixLen_ < sizeof(prefix_) - 1; ++i) {
      const unsigned char c = data[i];
      prefix_[prefixLen_++] = (c >= 32 && c < 127) ? static_cast<char>(c) : '.';
    }
    prefix_[prefixLen_] = 0;
    return true;
  }
  void close() {
    if (open_) {
      f_.close();
      open_ = false;
    }
  }
  size_t bytes() const { return bytes_; }
  const char* prefix() const { return prefix_; }

 private:
  FsFile f_;
  bool open_ = false;
  size_t bytes_ = 0;
  char prefix_[48] = {};
  size_t prefixLen_ = 0;
};

class PsvtsSink final : public M4xJsonStream::Sink {
 public:
  PsvtsSink() { scanner_.reset(M4xPsvts::kMaxValueLen); }
  bool write(const uint8_t* data, size_t len) override {
    if (!data || len == 0) return true;
    if (scanned_ > M4xPsvts::kMaxScanBytes || len > M4xPsvts::kMaxScanBytes - scanned_) return false;
    scanned_ += len;
    if (!scanner_.found && !scanner_.valueTooLarge) scanner_.feed(data, len);
    return !scanner_.valueTooLarge;
  }
  bool found() const { return scanner_.found && !scanner_.value.empty(); }
  const std::string& value() const { return scanner_.value; }
  size_t scanned() const { return scanned_; }

 private:
  M4xPsvts::Scanner scanner_;
  size_t scanned_ = 0;
};

void fillMem(Result& out, bool before) {
  if (before) out.before = M4HttpTransport::memSnap();
  else out.after = M4HttpTransport::memSnap();
}

void setErr(Result& out, const char* e) {
  out.ok = false;
  if (e) std::snprintf(out.error, sizeof(out.error), "%s", e);
}

void setNote(Result& out, const char* e) {
  // Soft-success note (e.g. cancelled after enough bytes); does not clear ok.
  if (e) std::snprintf(out.error, sizeof(out.error), "%s", e);
}

void setDetail(Result& out, const char* d) {
  if (d) std::snprintf(out.detail, sizeof(out.detail), "%s", d);
}

std::string appRoot(const char* appId) {
  const char* id = (appId && appId[0]) ? appId : kDefaultAppId;
  return std::string("/apps_data/") + id;
}

bool ensureLogDir() {
  SdMan.mkdir("/apps_data", true);
  SdMan.mkdir("/apps_data/com.weread.client", true);
  SdMan.mkdir(kProbeDir, true);
  return true;
}

bool loadCookie(const char* appId, std::string& cookie, Result& out) {
  if (!M4NativeProviderIo::loadCookieHeader(appRoot(appId), "weread", cookie) || cookie.empty()) {
    setErr(out, "login_required");
    setDetail(out, "no_cookie");
    return false;
  }
  return true;
}

void finish(Result& out) {
  out.sessionOpen = M4HttpTransport::sessionOpen();
  fillMem(out, false);
}

bool stepMem(Result& out) {
  fillMem(out, true);
  out.ok = true;
  setDetail(out, "heap_snapshot");
  finish(out);
  return true;
}

bool stepDebugOn(Result& out) {
  ensureLogDir();
  M4HttpTransport::setDebug(true);
  M4HttpTransport::setDebugLogPath(kLogPath);
  M4HttpTransport::debugStep("probe_debug_on", kLogPath);
  out.ok = true;
  setDetail(out, kLogPath);
  finish(out);
  return true;
}

bool stepDebugOff(Result& out) {
  M4HttpTransport::debugStep("probe_debug_off", "");
  M4HttpTransport::setDebug(false);
  // Keep SD path so later silent breadcrumbs still land if re-enabled path-only.
  out.ok = true;
  finish(out);
  return true;
}

bool stepSessionBegin(const Args& a, Result& out) {
  fillMem(out, true);
  const char* host = (a.host && a.host[0]) ? a.host : "weread.qq.com";
  const bool ok = M4HttpTransport::sessionBegin(host);
  out.ok = ok;
  if (!ok) setErr(out, "session_begin_failed");
  setDetail(out, host);
  finish(out);
  return true;
}

bool stepSessionEnd(Result& out) {
  fillMem(out, true);
  M4HttpTransport::sessionEnd();
  out.ok = true;
  setDetail(out, "closed");
  finish(out);
  return true;
}

bool stepTlsGet(const Args& a, Result& out) {
  fillMem(out, true);
  const char* url = (a.url && a.url[0]) ? a.url : "https://weread.qq.com/";
  DiscardSink sink;
  M4HttpTransport::Request req;
  req.method = "GET";
  req.url = url;
  // Connectivity probe: stop after a small window so we never pull multi-MB
  // home pages. Soft-success when we got body + 2xx before cancel/cap.
  constexpr size_t kProbeCap = 16u * 1024u;
  req.maxBytes = kProbeCap;
  req.timeoutMs = a.timeoutMs ? a.timeoutMs : 30000;
  req.followRedirects = true;

  struct CancelCtx {
    DiscardSink* sink;
    size_t stopAfter;
  } cctx{&sink, 8u * 1024u};
  auto cancelFn = [](void* p) -> bool {
    auto* c = static_cast<CancelCtx*>(p);
    return c && c->sink && c->sink->bytes() >= c->stopAfter;
  };

  const bool useSession = a.useSession && M4HttpTransport::sessionOpen();
  M4HttpTransport::Result net =
      useSession ? M4HttpTransport::sessionRequestToSink(req, sink, nullptr, nullptr, cancelFn, &cctx)
                : M4HttpTransport::requestToSink(req, sink, nullptr, nullptr, cancelFn, &cctx);
  out.status = net.status;
  out.bytes = net.bytes ? net.bytes : sink.bytes();
  const bool softCap =
      !net.ok && out.bytes > 0 &&
      (std::strcmp(net.error, "response_too_large") == 0 || std::strcmp(net.error, "cancelled") == 0) &&
      (out.status == 0 || (out.status >= 200 && out.status < 300));
  // esp_http_client may not surface status if cancelled mid-body; prefix HTML is enough.
  const bool htmlOk = out.bytes >= 64 && sink.prefix()[0] == '<';
  out.ok = net.ok || softCap || htmlOk;
  if (!out.ok) setErr(out, net.error[0] ? net.error : "tls_get_failed");
  else if (!net.ok) setNote(out, net.error[0] ? net.error : "tls_soft_ok");
  char det[96];
  std::snprintf(det, sizeof(det), "sess=%d pfx=%.40s", useSession ? 1 : 0, sink.prefix());
  setDetail(out, det);
  finish(out);
  return true;
}

bool stepWereadPsvts(const Args& a, Result& out) {
  fillMem(out, true);
  if (!a.bookId || !a.bookId[0] || !a.chapterUid || !a.chapterUid[0]) {
    setErr(out, "missing_ids");
    finish(out);
    return true;
  }
  std::string cookie;
  if (!loadCookie(a.appId, cookie, out)) {
    finish(out);
    return true;
  }

  const std::string readerUrl = std::string("https://weread.qq.com/web/reader/") +
                                weread_crypto::e(a.bookId) + "k" + weread_crypto::e(a.chapterUid);

  PsvtsSink sink;
  M4HttpTransport::Request req;
  req.method = "GET";
  req.url = readerUrl.c_str();
  req.headers[0] = {"Cookie", cookie.c_str()};
  req.headers[1] = {"Referer", "https://weread.qq.com/"};
  req.headerCount = 2;
  req.maxBytes = M4xPsvts::kMaxScanBytes;
  req.timeoutMs = a.timeoutMs ? a.timeoutMs : 30000;

  const bool useSession = a.useSession && M4HttpTransport::sessionOpen();
  if (a.useSession && !M4HttpTransport::sessionOpen()) {
    M4HttpTransport::sessionBegin("weread.qq.com");
  }
  const bool sess = M4HttpTransport::sessionOpen() && a.useSession;
  M4HttpTransport::Result net =
      sess ? M4HttpTransport::sessionRequestToSink(req, sink, nullptr, nullptr, nullptr, nullptr)
           : M4HttpTransport::requestToSink(req, sink, nullptr, nullptr, nullptr, nullptr);

  out.status = net.status;
  out.bytes = sink.scanned();
  if (!net.ok) {
    setErr(out, net.error[0] ? net.error : "psvts_http");
    finish(out);
    return true;
  }
  if (!sink.found()) {
    setErr(out, "psvts_not_found");
    setDetail(out, "scanned_ok_no_token");
    finish(out);
    return true;
  }
  std::snprintf(gLastPsvts, sizeof(gLastPsvts), "%s", sink.value().c_str());
  std::snprintf(out.psvts, sizeof(out.psvts), "%.70s", gLastPsvts);
  out.ok = true;
  char det[96];
  std::snprintf(det, sizeof(det), "sess=%d psvts_len=%u", sess ? 1 : 0,
                static_cast<unsigned>(sink.value().size()));
  setDetail(out, det);
  finish(out);
  return true;
}

bool stepWereadShard(const Args& a, const char* endpoint, Result& out) {
  fillMem(out, true);
  if (!a.bookId || !a.bookId[0] || !a.chapterUid || !a.chapterUid[0]) {
    setErr(out, "missing_ids");
    finish(out);
    return true;
  }
  const char* psvts = (a.psvts && a.psvts[0]) ? a.psvts : gLastPsvts;
  if (!psvts || !psvts[0]) {
    setErr(out, "psvts_required");
    setDetail(out, "run weread_psvts first");
    finish(out);
    return true;
  }
  std::string cookie;
  if (!loadCookie(a.appId, cookie, out)) {
    finish(out);
    return true;
  }

  const std::string readerUrl = std::string("https://weread.qq.com/web/reader/") +
                                weread_crypto::e(a.bookId) + "k" + weread_crypto::e(a.chapterUid);
  const std::string url = std::string("https://weread.qq.com") + endpoint;
  const long now = static_cast<long>(time(nullptr));
  const long rnd = static_cast<long>(esp_random() % 10000u);
  const std::string body =
      weread_crypto::makeContentParamsJson(a.bookId, a.chapterUid, psvts, false, 1, now, rnd);

  FileSink sink;
  if (!sink.open(kShardPath)) {
    setErr(out, "sd_open_failed");
    finish(out);
    return true;
  }

  M4HttpTransport::Request req;
  req.method = "POST";
  req.url = url.c_str();
  req.headers[0] = {"Cookie", cookie.c_str()};
  req.headers[1] = {"Referer", readerUrl.c_str()};
  req.headers[2] = {"Content-Type", "application/json"};
  req.headerCount = 3;
  req.body = body.data();
  req.bodyLen = body.size();
  req.maxBytes = 2u * 1024u * 1024u;
  req.timeoutMs = a.timeoutMs ? a.timeoutMs : 30000;

  if (a.useSession && !M4HttpTransport::sessionOpen()) {
    M4HttpTransport::sessionBegin("weread.qq.com");
  }
  const bool sess = M4HttpTransport::sessionOpen() && a.useSession;
  M4HttpTransport::Result net =
      sess ? M4HttpTransport::sessionRequestToSink(req, sink, nullptr, nullptr, nullptr, nullptr)
           : M4HttpTransport::requestToSink(req, sink, nullptr, nullptr, nullptr, nullptr);
  sink.close();

  out.status = net.status;
  out.bytes = sink.bytes();
  if (!net.ok || sink.bytes() == 0) {
    setErr(out, net.error[0] ? net.error : "shard_download");
  } else {
    out.ok = true;
  }
  char det[96];
  std::snprintf(det, sizeof(det), "sess=%d ep=%s pfx=%.36s", sess ? 1 : 0, endpoint, sink.prefix());
  setDetail(out, det);
  finish(out);
  return true;
}

bool stepShutdown(Result& out) {
  fillMem(out, true);
  M4HttpTransport::shutdown();
  gLastPsvts[0] = 0;
  out.ok = true;
  setDetail(out, "shutdown");
  finish(out);
  return true;
}

bool stepWereadWorkerFetch(const Args& a, Result& out) {
  fillMem(out, true);
  if (!a.bookId || !a.bookId[0] || !a.chapterUid || !a.chapterUid[0]) {
    setErr(out, "missing_ids");
    finish(out);
    return true;
  }
  const char* appId = (a.appId && a.appId[0]) ? a.appId : kDefaultAppId;
  const char* title = (a.title && a.title[0]) ? a.title : a.bookId;
  M4NativeProviderManager::begin();
  if (!M4NativeProviderManager::ensureBook("weread", a.bookId, appId, title)) {
    setErr(out, "ensure_book_failed");
    setDetail(out, "register/catalog");
    finish(out);
    return true;
  }
  int index0 = -1;
  if (!M4NativeProviderManager::findChapterIndex("weread", a.bookId, a.chapterUid, index0) ||
      index0 < 0) {
    setErr(out, "chapter_index");
    setDetail(out, "toc_missing_uid");
    finish(out);
    return true;
  }
  // Drop any partial cache so we force the worker TLS/decode path.
  (void)M4NativeProviderManager::invalidateChapterCache("weread", a.bookId, index0);
  if (!M4NativeProviderManager::ensureChapter("weread", a.bookId, index0, true)) {
    setErr(out, "ensure_chapter_queue");
    finish(out);
    return true;
  }

  const uint32_t budget = a.timeoutMs ? a.timeoutMs : 90000;
  const uint32_t t0 = millis();
  char lastErr[48] = {};
  while (millis() - t0 < budget) {
    const auto st = M4ContentProviderSession::chapterAt("weread", a.bookId, index0);
    if (st.state == M4ContentProvider::ChapterReady::Ready) {
      out.ok = true;
      out.bytes = 0;
      size_t n = 0;
      // Best-effort: cache path if session exposes rel path.
      if (!st.cacheRelPath.empty()) {
        const std::string abs = std::string("/apps_data/") + appId + "/" + st.cacheRelPath;
        FsFile f;
        if (SdMan.openFileForRead("WR-WSZ", abs.c_str(), f)) {
          n = static_cast<size_t>(f.fileSize());
          f.close();
        }
      }
      out.bytes = n;
      char det[96];
      std::snprintf(det, sizeof(det), "idx=%d pct=%d path=%.40s", index0, st.pct,
                    st.cacheRelPath.c_str());
      setDetail(out, det);
      finish(out);
      return true;
    }
    if (st.state == M4ContentProvider::ChapterReady::Error) {
      std::snprintf(lastErr, sizeof(lastErr), "%s",
                    st.error.empty() ? "fetch_failed" : st.error.c_str());
      setErr(out, lastErr);
      char det[96];
      std::snprintf(det, sizeof(det), "idx=%d state=%d", index0, static_cast<int>(st.state));
      setDetail(out, det);
      finish(out);
      return true;
    }
    // Cooperative: http_probe runs on owner loop; yield so worker + UI can run.
    delay(50);
  }
  setErr(out, lastErr[0] ? lastErr : "worker_timeout");
  setDetail(out, "poll_timeout");
  finish(out);
  return true;
}

bool stepWereadSetCookie(const Args& a, Result& out) {
  fillMem(out, true);
  if ((!a.wrVid || !a.wrVid[0]) && (!a.wrSkey || !a.wrSkey[0]) && (!a.wrRt || !a.wrRt[0])) {
    setErr(out, "missing_cookie");
    finish(out);
    return true;
  }
  std::vector<std::pair<std::string, std::string>> vals;
  if (a.wrVid && a.wrVid[0]) vals.emplace_back("wr_vid", a.wrVid);
  if (a.wrSkey && a.wrSkey[0]) vals.emplace_back("wr_skey", a.wrSkey);
  if (a.wrRt && a.wrRt[0]) vals.emplace_back("wr_rt", a.wrRt);
  if (!M4NativeProviderIo::storeCookieValues(appRoot(a.appId), vals)) {
    setErr(out, "cookie_write_failed");
    finish(out);
    return true;
  }
  out.ok = true;
  char det[96];
  std::snprintf(det, sizeof(det), "keys=%u", static_cast<unsigned>(vals.size()));
  setDetail(out, det);
  finish(out);
  return true;
}

}  // namespace

const char* lastPsvts() { return gLastPsvts; }

bool run(const Args& args, Result& out) {
  out = Result{};
  if (!args.step || !args.step[0]) {
    setErr(out, "missing_step");
    return false;
  }
  std::snprintf(out.step, sizeof(out.step), "%s", args.step);
  fillMem(out, true);

  const char* s = args.step;
  if (std::strcmp(s, "mem") == 0) return stepMem(out);
  if (std::strcmp(s, "debug_on") == 0) return stepDebugOn(out);
  if (std::strcmp(s, "debug_off") == 0) return stepDebugOff(out);
  if (std::strcmp(s, "session_begin") == 0) return stepSessionBegin(args, out);
  if (std::strcmp(s, "session_end") == 0) return stepSessionEnd(out);
  if (std::strcmp(s, "tls_get") == 0) return stepTlsGet(args, out);
  if (std::strcmp(s, "weread_psvts") == 0) return stepWereadPsvts(args, out);
  if (std::strcmp(s, "weread_e0") == 0) return stepWereadShard(args, "/web/book/chapter/e_0", out);
  if (std::strcmp(s, "weread_t0") == 0) return stepWereadShard(args, "/web/book/chapter/t_0", out);
  if (std::strcmp(s, "weread_t1") == 0) return stepWereadShard(args, "/web/book/chapter/t_1", out);
  if (std::strcmp(s, "weread_e1") == 0) return stepWereadShard(args, "/web/book/chapter/e_1", out);
  if (std::strcmp(s, "weread_e3") == 0) return stepWereadShard(args, "/web/book/chapter/e_3", out);
  if (std::strcmp(s, "weread_set_cookie") == 0) return stepWereadSetCookie(args, out);
  if (std::strcmp(s, "weread_worker_fetch") == 0) return stepWereadWorkerFetch(args, out);
  if (std::strcmp(s, "shutdown") == 0) return stepShutdown(out);

  setErr(out, "unknown_step");
  return false;
}

}  // namespace M4HttpTransportProbe
