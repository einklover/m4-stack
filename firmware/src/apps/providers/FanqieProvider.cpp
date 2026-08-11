#include "apps/providers/M4NativeProvider.h"
#include "apps/providers/M4NativeProviderHttp.h"
#include "apps/providers/M4NativeProviderIo.h"

#include "apps/M4xJsonStream.h"

#include <memory>
#include <utility>
#include <vector>

namespace M4NativeProviderAdapters {
namespace {

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
    http.url = std::string("https://fq-book.netsite.cc/content?item_id=") + req.chapter.uid;
    http.headers = {
        {"User-Agent", "Mozilla/5.0 (Linux; Android 10.0; wv) AppleWebKit/603.1.30 Version/4.0 M4Native/1"},
        {"Referer", "https://fanqienovel.com/"},
    };
    http.maxBytes = req.book.cachePolicy.maxChapterBytes > 0
                        ? std::max<size_t>(req.book.cachePolicy.maxChapterBytes, 4u * 1024u * 1024u)
                        : 4u * 1024u * 1024u;
    http.timeoutMs = 30000;
    http.followRedirects = true;
    // Public mirror currently uses a chain missing from some M4 CA bundles.
    // No credential is sent on this request.
    http.insecureTls = true;

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
