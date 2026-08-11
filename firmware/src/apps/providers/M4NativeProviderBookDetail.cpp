#include "apps/providers/M4NativeProviderBookDetail.h"

#include "apps/providers/M4LegadoBridge.h"
#include "apps/providers/M4NativeProviderHttp.h"
#include "apps/providers/M4NativeProviderIo.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <utility>

namespace M4NativeProviderBookDetail {
namespace {

constexpr const char* kFanqieUa =
    "Mozilla/5.0 (Linux; Android 10.0; wv) AppleWebKit/603.1.30 (KHTML, like Gecko) "
    "Version/4.0 Chrome/58.0.3029.110 Mobile Safari/537.36 M4Native/1";
constexpr const char* kJjUa =
    "Mozilla/5.0 (Linux; Android 5.1; Lenovo) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Version/4.0 Chrome/39.0.0.0 Mobile Safari/537.36/JINJIANG-Android/206(Lenovo;android 5.1;Scale/2.0)";
constexpr const char* kJjRef = "http://android.jjwxc.net?v=206";
constexpr size_t kIntroMax = 1536;
constexpr size_t kFieldMax = 192;

std::string appRoot(const std::string& appId) {
  return appId.empty() ? std::string() : std::string("/apps_data/") + appId;
}

std::string boundedUtf8(std::string s, size_t maxBytes) {
  if (s.size() <= maxBytes) return s;
  s.resize(maxBytes);
  while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0u) == 0x80u) s.pop_back();
  return s;
}

bool startsWithAt(const std::string& s, size_t pos, const char* literal) {
  if (!literal || pos > s.size()) return false;
  const size_t n = std::char_traits<char>::length(literal);
  return pos + n <= s.size() && s.compare(pos, n, literal) == 0;
}

std::string cleanIntro(const std::string& src) {
  std::string out;
  out.reserve(std::min(src.size(), kIntroMax));
  bool inTag = false;
  bool pendingSpace = false;
  for (size_t i = 0; i < src.size() && out.size() < kIntroMax; ++i) {
    const char c = src[i];
    if (c == '<') {
      inTag = true;
      pendingSpace = true;
      continue;
    }
    if (inTag) {
      if (c == '>') inTag = false;
      continue;
    }
    if (c == '&') {
      if (startsWithAt(src, i, "&nbsp;")) {
        pendingSpace = true;
        i += 5;
        continue;
      }
      if (startsWithAt(src, i, "&amp;")) {
        if (pendingSpace && !out.empty() && out.back() != ' ') out.push_back(' ');
        pendingSpace = false;
        out.push_back('&');
        i += 4;
        continue;
      }
      if (startsWithAt(src, i, "&lt;")) {
        if (pendingSpace && !out.empty() && out.back() != ' ') out.push_back(' ');
        pendingSpace = false;
        out.push_back('<');
        i += 3;
        continue;
      }
      if (startsWithAt(src, i, "&gt;")) {
        if (pendingSpace && !out.empty() && out.back() != ' ') out.push_back(' ');
        pendingSpace = false;
        out.push_back('>');
        i += 3;
        continue;
      }
      if (startsWithAt(src, i, "&quot;")) {
        if (pendingSpace && !out.empty() && out.back() != ' ') out.push_back(' ');
        pendingSpace = false;
        out.push_back('"');
        i += 5;
        continue;
      }
    }
    if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
      pendingSpace = true;
      continue;
    }
    if (pendingSpace && !out.empty() && out.back() != ' ') out.push_back(' ');
    pendingSpace = false;
    out.push_back(c);
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return boundedUtf8(std::move(out), kIntroMax);
}

std::string jsonText(JsonVariantConst v) {
  if (v.isNull()) return {};
  if (v.is<const char*>()) {
    const char* p = v.as<const char*>();
    return p ? std::string(p) : std::string();
  }
  if (v.is<long long>()) return std::to_string(v.as<long long>());
  if (v.is<unsigned long long>()) return std::to_string(v.as<unsigned long long>());
  if (v.is<double>()) return std::to_string(static_cast<long long>(v.as<double>()));
  if (v.is<bool>()) return v.as<bool>() ? "1" : "0";
  return {};
}

std::string firstText(JsonVariantConst node, std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    const std::string value = jsonText(node[key]);
    if (!value.empty()) return value;
  }
  return {};
}

void assignField(std::string& dst, std::string value, size_t maxBytes = kFieldMax) {
  if (!value.empty()) dst = boundedUtf8(std::move(value), maxBytes);
}

JsonVariantConst fanqieNode(const JsonDocument& doc) {
  JsonArrayConst data = doc["data"].as<JsonArrayConst>();
  if (!data.isNull() && data.size() > 0) return data[0];
  JsonVariantConst nested = doc["data"]["data"];
  if (!nested.isNull()) return nested;
  return doc.as<JsonVariantConst>();
}

JsonVariantConst wereadNode(const JsonDocument& doc) {
  JsonVariantConst v = doc["bookInfo"];
  if (!v.isNull()) return v;
  v = doc["data"]["bookInfo"];
  if (!v.isNull()) return v;
  v = doc["data"];
  if (!v.isNull()) return v;
  return doc.as<JsonVariantConst>();
}

std::string fanqieStatus(JsonVariantConst node) {
  const std::string raw = firstText(node, {"creation_status", "creationStatus"});
  if (raw == "0" || raw == "-1") return "完结";
  if (raw == "1") return "连载";
  if (raw == "4") return "已断更";
  return {};
}

std::string jjwxcStatus(JsonVariantConst node) {
  const std::string raw = firstText(node, {"novelStep", "novelstep"});
  if (raw.empty()) return {};
  return raw == "1" ? "连载" : "完结";
}

bool parseBody(const std::string& body, JsonDocument& doc, Result& out) {
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    out.error = "detail_json";
    return false;
  }
  return true;
}

Result fetchJjwxc(const Request& req, const CancelFn& cancelled) {
  Result out;
  out.detail = seed(req);

  M4NativeProviderHttp::Request http;
  http.url = "https://app-cdn.jjwxc.net/androidapi/novelbasicinfo?novelId=" + req.bookId;
  http.headers = {{"User-Agent", kJjUa}, {"Referer", kJjRef}};
  http.maxBytes = std::max<size_t>(32u * 1024u, req.maxBytes);
  http.timeoutMs = 30000;

  std::string body;
  M4NativeProviderHttp::Result net;
  if (!M4NativeProviderHttp::requestSmall(http, body, net, req.maxBytes, cancelled)) {
    out.error = net.error.empty() ? "detail_http" : net.error;
    out.receivedBytes = net.bytes;
    return out;
  }
  out.receivedBytes = net.bytes;

  JsonDocument doc;
  if (!parseBody(body, doc, out)) return out;
  JsonVariantConst node = doc.as<JsonVariantConst>();
  assignField(out.detail.title, firstText(node, {"novelName", "novelname"}));
  assignField(out.detail.author, firstText(node, {"authorName", "authorname"}));
  assignField(out.detail.intro, cleanIntro(firstText(node, {"novelIntro", "novelIntroShort"})), kIntroMax);
  std::string kind = firstText(node, {"novelClass", "className"});
  const std::string tags = firstText(node, {"novelTags", "tags"});
  if (!tags.empty()) kind += (kind.empty() ? "" : " · ") + tags;
  assignField(out.detail.kind, std::move(kind));
  assignField(out.detail.status, jjwxcStatus(node));
  assignField(out.detail.wordCount, firstText(node, {"novelSize", "novelsize", "novelSizeformat"}));
  assignField(out.detail.lastChapter, firstText(node, {"renewChapterName", "renewchaptername"}));
  out.ok = !out.detail.title.empty();
  if (!out.ok) out.error = "detail_empty";
  return out;
}

Result fetchFanqie(const Request& req, const CancelFn& cancelled) {
  Result out;
  out.detail = seed(req);

  M4NativeProviderHttp::Request http;
  http.url =
      "https://api5-normal-sinfonlineb.fqnovel.com/reading/bookapi/multi-detail/v/"
      "?aid=1967&iid=1&version_code=999&book_id=" + req.bookId;
  http.headers = {{"User-Agent", kFanqieUa}, {"Referer", "https://fanqienovel.com/"}};
  http.maxBytes = std::max<size_t>(48u * 1024u, req.maxBytes);
  http.timeoutMs = 30000;
  http.followRedirects = true;

  std::string body;
  M4NativeProviderHttp::Result net;
  if (!M4NativeProviderHttp::requestSmall(http, body, net, req.maxBytes, cancelled)) {
    out.error = net.error.empty() ? "detail_http" : net.error;
    out.receivedBytes = net.bytes;
    return out;
  }
  out.receivedBytes = net.bytes;

  JsonDocument doc;
  if (!parseBody(body, doc, out)) return out;
  JsonVariantConst node = fanqieNode(doc);
  assignField(out.detail.title, firstText(node, {"book_name", "bookName", "title"}));
  assignField(out.detail.author, firstText(node, {"author", "author_name"}));
  assignField(out.detail.intro, cleanIntro(firstText(node, {"abstract", "introduction", "intro"})), kIntroMax);
  assignField(out.detail.kind, firstText(node, {"category", "complete_category", "sub_info"}));
  assignField(out.detail.status, fanqieStatus(node));
  assignField(out.detail.wordCount, firstText(node, {"word_number", "wordNumber"}));
  assignField(out.detail.lastChapter, firstText(node, {"last_chapter_title", "lastChapterTitle"}));
  out.ok = !out.detail.title.empty();
  if (!out.ok) out.error = "detail_empty";
  return out;
}

Result fetchWeread(const Request& req, const CancelFn& cancelled) {
  Result out;
  out.detail = seed(req);

  std::string cookie;
  if (!M4NativeProviderIo::loadCookieHeader(appRoot(req.appId), "weread", cookie)) {
    out.error = "login_required";
    return out;
  }

  M4NativeProviderHttp::Request http;
  http.url = "https://weread.qq.com/web/book/info?bookId=" + req.bookId;
  http.headers = {{"User-Agent", "Mozilla/5.0 Murphy-M4 NativeProvider/1"},
                  {"Referer", "https://weread.qq.com/"}, {"Cookie", cookie}};
  http.maxBytes = std::max<size_t>(32u * 1024u, req.maxBytes);
  http.timeoutMs = 30000;

  std::string body;
  M4NativeProviderHttp::Result net;
  if (!M4NativeProviderHttp::requestSmall(http, body, net, req.maxBytes, cancelled)) {
    out.error = net.error.empty() ? "detail_http" : net.error;
    out.receivedBytes = net.bytes;
    return out;
  }
  out.receivedBytes = net.bytes;

  JsonDocument doc;
  if (!parseBody(body, doc, out)) return out;
  JsonVariantConst node = wereadNode(doc);
  assignField(out.detail.title, firstText(node, {"title", "bookTitle"}));
  assignField(out.detail.author, firstText(node, {"author", "authorName"}));
  assignField(out.detail.intro, cleanIntro(firstText(node, {"intro", "introduction", "summary"})), kIntroMax);
  assignField(out.detail.kind, firstText(node, {"category", "categories"}));
  assignField(out.detail.status, firstText(node, {"status", "bookStatus"}));
  assignField(out.detail.wordCount, firstText(node, {"wordCount", "word_count"}));
  assignField(out.detail.lastChapter, firstText(node, {"lastChapterTitle", "last_chapter_title"}));
  out.ok = !out.detail.title.empty();
  if (!out.ok) out.error = "detail_empty";
  return out;
}

Result fetchLegado(const Request& req, const CancelFn& cancelled) {
  Result out;
  out.detail = seed(req);

  // Legado has no per-book detail endpoint; the bookshelf carries the book
  // metadata (intro/author/latest chapter). Fetch it and match the short id.
  const std::string locator = M4LegadoBridge::readLocator(appRoot(req.appId), req.bookId);
  if (locator.empty()) {
    out.error = "book_locator_missing";
    return out;
  }

  M4NativeProviderHttp::Request http;
  if (!M4LegadoBridge::ensureEndpoint(appRoot(req.appId))) {
    out.error = "legado_endpoint_missing";
    return out;
  }
  http.url = M4LegadoBridge::baseUrl() + "/getBookshelf";
  http.headers = {{"User-Agent", "Mozilla/5.0 Murphy-M4 NativeProvider/1"}};
  http.maxBytes = std::max<size_t>(128u * 1024u, req.maxBytes);
  http.timeoutMs = 30000;

  std::string body;
  M4NativeProviderHttp::Result net;
  if (!M4NativeProviderHttp::requestSmall(http, body, net, req.maxBytes, cancelled)) {
    out.error = net.error.empty() ? "detail_http" : net.error;
    out.receivedBytes = net.bytes;
    return out;
  }
  out.receivedBytes = net.bytes;

  JsonDocument doc;
  if (!parseBody(body, doc, out)) return out;
  JsonVariantConst arr = doc["data"];
  if (!arr.is<JsonArray>()) arr = doc.as<JsonVariantConst>();
  if (!arr.is<JsonArray>()) {
    out.error = "detail_empty";
    return out;
  }
  for (JsonVariantConst book : arr.as<JsonArrayConst>()) {
    const char* bookUrl = book["bookUrl"] | "";
    if (!bookUrl || M4LegadoBridge::shortId(bookUrl) != req.bookId) continue;
    assignField(out.detail.title, firstText(book, {"name", "title"}));
    assignField(out.detail.author, firstText(book, {"author"}));
    assignField(out.detail.intro, cleanIntro(firstText(book, {"intro", "description"})), kIntroMax);
    assignField(out.detail.lastChapter, firstText(book, {"latestChapterTitle", "durChapterTitle"}));
    const char* origin = book["origin"] | "";
    if (origin && origin[0]) assignField(out.detail.kind, std::string("来源：") + origin);
    break;
  }
  out.ok = !out.detail.title.empty();
  if (!out.ok) out.error = "detail_empty";
  return out;
}

}  // namespace

M4NovelProvider::BookDetail seed(const Request& req) {
  M4NovelProvider::BookDetail detail;
  detail.providerId = req.providerId;
  detail.bookId = req.bookId;
  detail.title = boundedUtf8(req.title, kFieldMax);
  detail.author = boundedUtf8(req.author, kFieldMax);
  return detail;
}

Result fetch(const Request& req, const CancelFn& cancelled) {
  Result out;
  out.detail = seed(req);
  if (req.bookId.empty() || req.providerId.empty() || req.maxBytes == 0) {
    out.error = "bad_detail_request";
    return out;
  }
  if (cancelled && cancelled()) {
    out.error = "cancelled";
    return out;
  }
  if (req.providerId == "jjwxc") return fetchJjwxc(req, cancelled);
  if (req.providerId == "fanqie") return fetchFanqie(req, cancelled);
  if (req.providerId == "weread") return fetchWeread(req, cancelled);
  if (req.providerId == "legado") return fetchLegado(req, cancelled);
  out.error = "provider_not_supported";
  return out;
}

}  // namespace M4NativeProviderBookDetail
