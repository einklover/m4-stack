#include "apps/providers/M4NativeProvider.h"
#include "apps/providers/M4NativeProviderHttp.h"
#include "apps/providers/M4NativeProviderIo.h"

#include "apps/providers/M4LegadoBridge.h"

#include "apps/M4xJsonStream.h"

#include <memory>
#include <utility>
#include <vector>

namespace M4NativeProviderAdapters {
namespace {

// Legado web API chapter fetch:
//   GET {base}/getBookContent?url=<bookUrl locator>&index=<chapterIndex0>
// returns a JSON document whose "data" field is the plain chapter text.
// The bookUrl locator is resolved from the sidecar by the short M4 bookId.
class LegadoProvider final : public M4NativeProvider::Adapter {
 public:
  const char* id() const override { return "legado"; }

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

    const std::string locator = M4LegadoBridge::readLocator(req.appDataRoot, req.book.bookId);
    if (locator.empty()) {
      out.error = "book_locator_missing";
      return out;
    }

    M4NativeProviderIo::PartFileSink file;
    if (!file.open(req.cacheAbsPath)) {
      out.error = "sd_open_failed";
      return out;
    }
    // Legado getBookContent returns {"data":"<plain text>"}; empty path means
    // the root object, field "data" selects the string value.
    M4xJsonStream::ScalarStreamExtractor scalar({}, "data", file);
    M4NativeProviderHttp::ExtractorSink sink(scalar);

    M4NativeProviderHttp::Request http;
    http.method = "GET";
    if (!M4LegadoBridge::ensureEndpoint(req.appDataRoot)) {
      out.error = "legado_endpoint_missing";
      return out;
    }
    http.url = M4LegadoBridge::baseUrl() + "/getBookContent?url=" + urlEncode(locator) +
               "&index=" + std::to_string(req.chapterIndex0 >= 0 ? req.chapterIndex0 : 0);
    http.headers = {{"User-Agent", "Mozilla/5.0 Murphy-M4 NativeProvider/1"}};
    http.maxBytes = req.book.cachePolicy.maxChapterBytes > 0
                        ? std::max<size_t>(req.book.cachePolicy.maxChapterBytes, 4u * 1024u * 1024u)
                        : 4u * 1024u * 1024u;
    http.timeoutMs = 30000;
    http.followRedirects = true;

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

 private:
  static std::string urlEncode(const std::string& in) {
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
};

}  // namespace

std::unique_ptr<M4NativeProvider::Adapter> createLegadoProvider() {
  return std::make_unique<LegadoProvider>();
}

}  // namespace M4NativeProviderAdapters
