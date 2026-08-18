#include "apps/providers/M4NativeProvider.h"
#include "apps/providers/M4NativeProviderHttp.h"
#include "apps/providers/M4NativeProviderIo.h"

#include "apps/M4xJsonStream.h"

#include <ctime>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace M4NativeProviderAdapters {
namespace {

constexpr const char* kFanqieUa =
    "Mozilla/5.0 (Linux; Android 10.0; wv) AppleWebKit/603.1.30 Version/4.0 M4Native/1";

// M4 has no NTP on boot. time() is often epoch, so never probe the landing
// host (that second TLS is ESP_ERR_HTTP_CONNECT). Always hit :8043 with an
// nt= date: RTC when sane, otherwise this firmware's build date.
std::string chapterDateUtc() {
  char ymd[16] = {};
  const time_t now = time(nullptr);
  struct tm tm {};
  if (now >= 1700000000 && gmtime_r(&now, &tm) != nullptr && (tm.tm_year + 1900) >= 2024) {
    std::snprintf(ymd, sizeof(ymd), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return ymd;
  }
  int day = 0, year = 0;
  char mon[8] = {};
  if (std::sscanf(__DATE__, "%7s %d %d", mon, &day, &year) == 3 && year >= 2024 && day >= 1 &&
      day <= 31) {
    static const char* kMonths = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char* hit = std::strstr(kMonths, mon);
    const int mi = hit ? static_cast<int>(hit - kMonths) / 3 + 1 : 1;
    std::snprintf(ymd, sizeof(ymd), "%04d-%02d-%02d", year, mi, day);
    return ymd;
  }
  return "2026-08-18";
}

std::string resolveChapterUrl(const std::string& uid) {
  return std::string("https://fq-book.nat.netsite.cc:8043/content?item_id=") + uid +
         "&nt=" + chapterDateUtc();
}

class FanqieProvider final : public M4NativeProvider::Adapter {
 public:
  const char* id() const override { return "fanqie"; }

  M4NativeProvider::FetchResult fetchChapter(const M4NativeProvider::ChapterRequest& req,
                                             const M4NativeProvider::ProgressFn& progress,
                                             const M4NativeProvider::CancelFn& cancelled) override {
    M4NativeProvider::FetchResult out;
    if (req.chapter.uid.empty() || req.cacheAbsPath.empty()) {
      out.error = "bad_chapter";
      return out;
    }

    size_t cached = 0;
    if (M4NativeProviderIo::cacheComplete(req.cacheAbsPath, &cached)) {
      out.ok = true;
      out.bytes = cached;
      out.cacheRelPath = req.cacheRelPath;
      if (progress) progress(M4NativeProvider::Phase::Ready, 0, cached, 100);
      return out;
    }

    M4NativeProviderIo::PartFileSink file;
    if (!file.open(req.cacheAbsPath)) {
      out.error = "sd_open_failed";
      return out;
    }
    M4xJsonStream::ScalarStreamExtractor scalar({"data", "data"}, "content", file);
    M4NativeProviderHttp::ExtractorSink sink(scalar);

    M4NativeProviderHttp::Request http;
    http.method = "GET";
    http.url = resolveChapterUrl(req.chapter.uid);
    http.headers = {
        {"User-Agent", kFanqieUa},
        {"Referer", "https://fanqienovel.com/"},
    };
    http.maxBytes = req.book.cachePolicy.maxChapterBytes > 0
                        ? std::max<size_t>(req.book.cachePolicy.maxChapterBytes, 4u * 1024u * 1024u)
                        : 4u * 1024u * 1024u;
    http.timeoutMs = 30000;
    http.followRedirects = false;
    // Isolated tls_get to this host succeeds with the CA bundle. insecureTls
    // (skip CN) was aborting the same URL as ESP_ERR_HTTP_CONNECT ~150ms.
    http.insecureTls = false;

    if (progress) progress(M4NativeProvider::Phase::Connecting, 0, 0, 0);
    const auto res = M4NativeProviderHttp::requestToSink(
        http, sink,
        [&](size_t received) {
          if (progress) progress(M4NativeProvider::Phase::Receiving, received, file.written(), 0);
        },
        cancelled);
    if (!res.ok) {
      file.close();
      M4NativeProviderIo::removeIncomplete(req.cacheAbsPath);
      out.error = res.error.empty() ? "network" : res.error;
      return out;
    }
    if (!scalar.finish()) {
      file.close();
      M4NativeProviderIo::removeIncomplete(req.cacheAbsPath);
      out.error = M4xJsonStream::errorString(scalar.error());
      if (out.error.empty()) out.error = "json_extract";
      return out;
    }
    if (!file.flush() || file.written() == 0) {
      file.close();
      M4NativeProviderIo::removeIncomplete(req.cacheAbsPath);
      out.error = "empty_content";
      return out;
    }
    const size_t written = file.written();
    file.close();
    if (progress) progress(M4NativeProvider::Phase::Writing, res.bytes, written, 95);
    size_t finalBytes = 0;
    if (!M4NativeProviderIo::commitPart(req.cacheAbsPath, &finalBytes)) {
      out.error = "cache_commit_failed";
      return out;
    }
    out.ok = true;
    out.bytes = finalBytes;
    out.cacheRelPath = req.cacheRelPath;
    if (progress) progress(M4NativeProvider::Phase::Ready, res.bytes, finalBytes, 100);
    return out;
  }
};

}  // namespace

std::unique_ptr<M4NativeProvider::Adapter> createFanqieProvider() {
  return std::make_unique<FanqieProvider>();
}

}  // namespace M4NativeProviderAdapters
