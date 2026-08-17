#include "apps/providers/M4NativeProvider.h"
#include "apps/providers/M4JjwxcEndpoint.h"
#include "apps/providers/M4NativeProviderHttp.h"
#include "apps/providers/M4NativeProviderIo.h"

#include "apps/M4ContentProviderCatalog.h"
#include "apps/M4xJsonStream.h"

#include <Arduino.h>
#include <SDCardManager.h>

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace M4NativeProviderAdapters {
namespace {

constexpr const char* kAppUa =
    "Mozilla/5.0 (Linux; Android 5.1; Lenovo) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Version/4.0 Chrome/39.0.0.0 Mobile Safari/537.36/JINJIANG-Android/206(Lenovo;android 5.1;Scale/2.0)";
constexpr const char* kAppRef = "http://android.jjwxc.net?v=206";
constexpr const char* kWapUa =
    "Mozilla/5.0 (Linux; Android 10.0; wv) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Version/4.0 Chrome/78.0.3904.108 Mobile Safari/537.36";
constexpr size_t kGbkTableBytes = 47760;

bool vipFlag(const std::string& rawLine) {
  std::string flag;
  if (!M4ContentProviderCatalog::fieldAt(rawLine, 3, flag)) return false;
  return !(flag.empty() || flag == "0" || flag == "false" || flag == "nil");
}

class RawFileSink final : public M4xJsonStream::Sink {
 public:
  ~RawFileSink() override { close(); }
  bool open(const std::string& path) {
    close();
    path_ = path;
    M4NativeProviderIo::ensureParentDirs(path_);
    if (SdMan.exists(path_.c_str())) SdMan.remove(path_.c_str());
    open_ = SdMan.openFileForWrite("JJ-WAP", path_.c_str(), f_);
    return open_;
  }
  bool write(const uint8_t* data, size_t len) override {
    if (!open_ || !data) return false;
    const int n = f_.write(data, len);
    if (n != static_cast<int>(len)) return false;
    bytes_ += len;
    return true;
  }
  void close() {
    if (open_) {
      f_.close();
      open_ = false;
    }
  }
  size_t bytes() const { return bytes_; }
 private:
  FsFile f_;
  std::string path_;
  size_t bytes_ = 0;
  bool open_ = false;
};

bool findAscii(const std::string& path, const std::string& needle, size_t start, size_t& found) {
  found = 0;
  if (needle.empty()) return false;
  FsFile f;
  if (!SdMan.openFileForRead("JJ-FIND", path.c_str(), f)) return false;
  const size_t total = f.fileSize();
  if (start >= total) {
    f.close();
    return false;
  }
  constexpr size_t kWin = 1536;
  std::vector<uint8_t> buf(kWin + 64);
  size_t off = start;
  while (off < total) {
    if (!f.seek(off)) break;
    const size_t want = std::min(buf.size(), total - off);
    const int n = f.read(buf.data(), want);
    if (n <= 0) break;
    const auto* begin = reinterpret_cast<const char*>(buf.data());
    const std::string chunk(begin, static_cast<size_t>(n));
    const size_t pos = chunk.find(needle);
    if (pos != std::string::npos) {
      found = off + pos;
      f.close();
      return true;
    }
    if (static_cast<size_t>(n) <= needle.size()) break;
    off += static_cast<size_t>(n) - needle.size();
  }
  f.close();
  return false;
}

struct GbkTable {
  uint8_t* p = nullptr;
  ~GbkTable() {
    if (p) {
#if defined(ARDUINO_ARCH_ESP32)
      heap_caps_free(p);
#else
      std::free(p);
#endif
    }
  }
  bool load(const std::string& appId) {
#if defined(ARDUINO_ARCH_ESP32)
    p = static_cast<uint8_t*>(heap_caps_malloc(kGbkTableBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#endif
    if (!p) p = static_cast<uint8_t*>(std::malloc(kGbkTableBytes));
    if (!p) return false;
    const std::string path = std::string("/apps/") + appId + "/gbk_table.bin";
    FsFile f;
    if (!SdMan.openFileForRead("JJ-GBK", path.c_str(), f) || f.fileSize() < kGbkTableBytes) {
      if (f.isOpen()) f.close();
      return false;
    }
    size_t off = 0;
    while (off < kGbkTableBytes) {
      const int n = f.read(p + off, kGbkTableBytes - off);
      if (n <= 0) break;
      off += static_cast<size_t>(n);
    }
    f.close();
    return off == kGbkTableBytes;
  }
  uint16_t lookup(uint8_t lead, uint8_t trail) const {
    if (!p || lead < 0x81 || lead > 0xFE) return 0;
    if (!((trail >= 0x40 && trail <= 0x7E) || (trail >= 0x80 && trail <= 0xFE))) return 0;
    const int tord = trail < 0x80 ? trail - 0x40 : trail - 0x41;
    const size_t off = (static_cast<size_t>(lead - 0x81) * 190u + static_cast<size_t>(tord)) * 2u;
    if (off + 1 >= kGbkTableBytes) return 0;
    return static_cast<uint16_t>((p[off] << 8) | p[off + 1]);
  }
};

bool emitCodepoint(M4xJsonStream::Sink& sink, uint32_t cp) {
  uint8_t out[4];
  size_t n = 0;
  if (cp == 0) cp = '?';
  if (cp < 0x80) out[n++] = static_cast<uint8_t>(cp);
  else if (cp < 0x800) {
    out[n++] = static_cast<uint8_t>(0xC0 | (cp >> 6));
    out[n++] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out[n++] = static_cast<uint8_t>(0xE0 | (cp >> 12));
    out[n++] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
    out[n++] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
  } else if (cp <= 0x10FFFF) {
    out[n++] = static_cast<uint8_t>(0xF0 | (cp >> 18));
    out[n++] = static_cast<uint8_t>(0x80 | ((cp >> 12) & 0x3F));
    out[n++] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
    out[n++] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
  } else {
    out[n++] = '?';
  }
  return sink.write(out, n);
}

bool emitAscii(M4xJsonStream::Sink& sink, char c) {
  const uint8_t b = static_cast<uint8_t>(c);
  return sink.write(&b, 1);
}

bool convertWapBody(const std::string& rawPath, size_t bodyOff, size_t bodyLen,
                    const std::string& appId, M4NativeProviderIo::PartFileSink& out,
                    const M4NativeProvider::ProgressFn& progress,
                    const M4NativeProvider::CancelFn& cancelled, std::string& err) {
  GbkTable table;
  if (!table.load(appId)) {
    err = "no_gbk_table";
    return false;
  }
  FsFile f;
  if (!SdMan.openFileForRead("JJ-CONV", rawPath.c_str(), f) || !f.seek(bodyOff)) {
    err = "wap_read_failed";
    if (f.isOpen()) f.close();
    return false;
  }

  constexpr size_t kWin = 4096;
  uint8_t buf[kWin + 4];
  size_t consumed = 0;
  size_t carry = 0;
  bool inTag = false;
  bool inEntity = false;
  std::string tag;
  std::string entity;

  auto newlineForTag = [&]() -> bool {
    std::string low = tag;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return low.find("br") == 0 || low.find("/p") == 0 || low.find("/div") == 0 || low.find("/li") == 0;
  };

  auto emitEntityValue = [&]() -> bool {
    if (entity == "nbsp" || entity == "#160") return emitAscii(out, ' ');
    if (entity == "amp") return emitAscii(out, '&');
    if (entity == "lt") return emitAscii(out, '<');
    if (entity == "gt") return emitAscii(out, '>');
    if (entity == "quot") return emitAscii(out, '"');
    if (entity == "apos") return emitAscii(out, '\'');
    if (entity.size() >= 2 && entity[0] == '#') {
      int base = 10;
      size_t start = 1;
      if (entity.size() >= 3 && (entity[1] == 'x' || entity[1] == 'X')) {
        base = 16;
        start = 2;
      }
      if (start < entity.size()) {
        char* endp = nullptr;
        const unsigned long cp = std::strtoul(entity.c_str() + start, &endp, base);
        if (endp && *endp == '\0' && cp > 0 && cp <= 0x10FFFFu &&
            !(cp >= 0xD800u && cp <= 0xDFFFu)) {
          return emitCodepoint(out, static_cast<uint32_t>(cp));
        }
      }
    }
    // Preserve unknown entities literally instead of silently replacing text.
    if (!emitAscii(out, '&')) return false;
    for (char c : entity) {
      if (!emitAscii(out, c)) return false;
    }
    return emitAscii(out, ';');
  };

  auto flushPartialEntity = [&]() -> bool {
    if (!emitAscii(out, '&')) return false;
    for (char c : entity) {
      if (!emitAscii(out, c)) return false;
    }
    entity.clear();
    inEntity = false;
    return true;
  };

  while (consumed < bodyLen) {
    if (cancelled && cancelled()) {
      err = "cancelled";
      f.close();
      return false;
    }
    const size_t want = std::min(kWin, bodyLen - consumed);
    if (carry) buf[0] = buf[kWin];
    const int nr = f.read(buf + carry, want);
    if (nr <= 0) break;
    const size_t n = carry + static_cast<size_t>(nr);
    carry = 0;
    size_t i = 0;
    while (i < n) {
      const uint8_t b = buf[i];
      if (inTag) {
        if (b == '>') {
          if (newlineForTag() && !emitAscii(out, '\n')) {
            err = "sd_write_failed";
            f.close();
            return false;
          }
          inTag = false;
          tag.clear();
        } else if (tag.size() < 32 && b < 0x80) {
          tag.push_back(static_cast<char>(b));
        }
        ++i;
        continue;
      }

      if (inEntity) {
        if (b == ';') {
          if (!emitEntityValue()) {
            err = "sd_write_failed";
            f.close();
            return false;
          }
          entity.clear();
          inEntity = false;
          ++i;
          continue;
        }
        // Entities are ASCII and short. If the stream is malformed, flush the
        // literal prefix and re-process the current byte through normal GBK/HTML
        // handling instead of losing it.
        if (entity.size() >= 14 || b >= 0x80 || b == '<' || b == '&') {
          if (!flushPartialEntity()) {
            err = "sd_write_failed";
            f.close();
            return false;
          }
          continue;
        }
        entity.push_back(static_cast<char>(b));
        ++i;
        continue;
      }

      if (b == '<') {
        inTag = true;
        tag.clear();
        ++i;
        continue;
      }
      if (b == '&') {
        inEntity = true;
        entity.clear();
        ++i;
        continue;
      }
      if (b < 0x80) {
        if (b != '\r' && !emitAscii(out, static_cast<char>(b))) {
          err = "sd_write_failed";
          f.close();
          return false;
        }
        ++i;
        continue;
      }
      if (b >= 0x81 && b <= 0xFE) {
        if (i + 1 >= n) {
          if (consumed + static_cast<size_t>(nr) < bodyLen) {
            buf[kWin] = b;
            carry = 1;
            ++i;
            continue;
          }
          if (!emitAscii(out, '?')) {
            err = "sd_write_failed";
            f.close();
            return false;
          }
          ++i;
          continue;
        }
        const uint8_t b2 = buf[i + 1];
        if (b2 >= 0x30 && b2 <= 0x39) {
          // Four-byte GB18030 sequence: keep memory-bounded compatibility with
          // the legacy plugin and substitute rare codepoints for now.
          if (!emitAscii(out, '?')) {
            err = "sd_write_failed";
            f.close();
            return false;
          }
          i += std::min<size_t>(4, n - i);
          continue;
        }
        const uint16_t cp = table.lookup(b, b2);
        if (!emitCodepoint(out, cp ? cp : '?')) {
          err = "sd_write_failed";
          f.close();
          return false;
        }
        i += 2;
        continue;
      }
      if (!emitAscii(out, '?')) {
        err = "sd_write_failed";
        f.close();
        return false;
      }
      ++i;
    }
    consumed += static_cast<size_t>(nr);
    if (progress) {
      const int pct = bodyLen ? static_cast<int>((consumed * 100u) / bodyLen) : 0;
      progress(M4NativeProvider::Phase::Decoding, bodyLen, out.written(), std::min(94, pct));
    }
  }
  if (inEntity && !flushPartialEntity()) {
    err = "sd_write_failed";
    f.close();
    return false;
  }
  f.close();
  if (consumed < bodyLen) {
    err = "wap_truncated";
    return false;
  }
  return out.written() > 0;
}

class JjwxcProvider final : public M4NativeProvider::Adapter {
 public:
  const char* id() const override { return "jjwxc"; }

  M4NativeProvider::FetchResult fetchChapter(const M4NativeProvider::ChapterRequest& req,
                                             const M4NativeProvider::ProgressFn& progress,
                                             const M4NativeProvider::CancelFn& cancelled) override {
    if (vipFlag(req.catalogRawLine)) return fetchVip(req, progress, cancelled);
    return fetchFree(req, progress, cancelled);
  }

 private:
  M4NativeProvider::FetchResult fetchFree(const M4NativeProvider::ChapterRequest& req,
                                          const M4NativeProvider::ProgressFn& progress,
                                          const M4NativeProvider::CancelFn& cancelled) {
    M4NativeProvider::FetchResult out;
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
    M4xJsonStream::ScalarStreamExtractor scalar({}, "content", file);
    M4NativeProviderHttp::ExtractorSink sink(scalar);
    M4NativeProviderHttp::Request http;
    http.url = std::string(M4_JJWXC_APP_CDN) + "/androidapi/chapterContent?novelId=" + req.book.bookId +
               "&chapterId=" + req.chapter.uid;
    http.headers = {{"User-Agent", kAppUa}, {"Referer", kAppRef}};
    http.maxBytes = std::max<size_t>(4u * 1024u * 1024u, req.book.cachePolicy.maxChapterBytes);
    http.timeoutMs = 45000;
    if (progress) progress(M4NativeProvider::Phase::Connecting, 0, 0, 0);
    const auto net = M4NativeProviderHttp::requestToSink(
        http, sink,
        [&](size_t n) {
          if (progress) progress(M4NativeProvider::Phase::Receiving, n, file.written(), 0);
        }, cancelled);
    if (!net.ok || !scalar.finish() || !file.flush() || file.written() == 0) {
      file.close();
      M4NativeProviderIo::removeIncomplete(req.cacheAbsPath);
      out.error = net.ok ? (M4xJsonStream::errorString(scalar.error())) : net.error;
      if (out.error.empty()) out.error = "empty_content";
      return out;
    }
    const size_t n = file.written();
    file.close();
    size_t finalBytes = 0;
    if (!M4NativeProviderIo::commitPart(req.cacheAbsPath, &finalBytes)) {
      out.error = "cache_commit_failed";
      return out;
    }
    out.ok = true;
    out.bytes = finalBytes;
    out.cacheRelPath = req.cacheRelPath;
    if (progress) progress(M4NativeProvider::Phase::Ready, net.bytes, finalBytes, 100);
    return out;
  }

  M4NativeProvider::FetchResult fetchVip(const M4NativeProvider::ChapterRequest& req,
                                         const M4NativeProvider::ProgressFn& progress,
                                         const M4NativeProvider::CancelFn& cancelled) {
    M4NativeProvider::FetchResult out;
    size_t cached = 0;
    if (M4NativeProviderIo::cacheComplete(req.cacheAbsPath, &cached)) {
      out.ok = true;
      out.bytes = cached;
      out.cacheRelPath = req.cacheRelPath;
      if (progress) progress(M4NativeProvider::Phase::Ready, 0, cached, 100);
      return out;
    }
    std::string cookie;
    if (!M4NativeProviderIo::loadCookieHeader(req.appDataRoot, "jjwxc", cookie)) {
      out.authRequired = true;
      out.error = "login_required";
      return out;
    }

    const std::string raw = req.cacheAbsPath + ".wap.tmp";
    RawFileSink rawSink;
    if (!rawSink.open(raw)) {
      out.error = "sd_open_failed";
      return out;
    }
    M4NativeProviderHttp::Request http;
    http.url = std::string(M4_JJWXC_WAP_BASE) + "/book2/" + req.book.bookId + "/" + req.chapter.uid;
    http.headers = {{"User-Agent", kWapUa}, {"Cookie", cookie}};
    http.maxBytes = 2u * 1024u * 1024u;
    http.timeoutMs = 30000;
    if (progress) progress(M4NativeProvider::Phase::Connecting, 0, 0, 0);
    const auto net = M4NativeProviderHttp::requestToSink(
        http, rawSink,
        [&](size_t n) {
          if (progress) progress(M4NativeProvider::Phase::Receiving, n, 0, 0);
        }, cancelled);
    rawSink.close();
    if (!net.ok || rawSink.bytes() == 0) {
      if (SdMan.exists(raw.c_str())) SdMan.remove(raw.c_str());
      out.error = net.error.empty() ? "wap_download" : net.error;
      if (out.error == "http_401" || out.error == "http_403") out.authRequired = true;
      return out;
    }

    size_t ul = 0, li = 0, end = 0;
    if (!findAscii(raw, "<ul class=\"content_ul\">", 0, ul) ||
        !findAscii(raw, "<li>", ul, li) || !findAscii(raw, "</li>", li + 4, end) || end <= li + 4) {
      if (SdMan.exists(raw.c_str())) SdMan.remove(raw.c_str());
      out.error = "vip_no_body";
      return out;
    }

    M4NativeProviderIo::PartFileSink text;
    if (!text.open(req.cacheAbsPath)) {
      SdMan.remove(raw.c_str());
      out.error = "sd_open_failed";
      return out;
    }
    std::string convErr;
    const bool converted = convertWapBody(raw, li + 4, end - (li + 4), req.book.appId, text,
                                          progress, cancelled, convErr);
    SdMan.remove(raw.c_str());
    if (!converted || !text.flush() || text.written() == 0) {
      text.close();
      M4NativeProviderIo::removeIncomplete(req.cacheAbsPath);
      out.error = convErr.empty() ? "gbk_convert" : convErr;
      return out;
    }
    text.close();
    size_t finalBytes = 0;
    if (!M4NativeProviderIo::commitPart(req.cacheAbsPath, &finalBytes)) {
      out.error = "cache_commit_failed";
      return out;
    }
    out.ok = true;
    out.bytes = finalBytes;
    out.cacheRelPath = req.cacheRelPath;
    if (progress) progress(M4NativeProvider::Phase::Ready, net.bytes, finalBytes, 100);
    return out;
  }
};

}  // namespace

std::unique_ptr<M4NativeProvider::Adapter> createJjwxcProvider() {
  return std::make_unique<JjwxcProvider>();
}

}  // namespace M4NativeProviderAdapters
