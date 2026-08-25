#include <cassert>
#include <iostream>
#include <set>
#include <string>
#include <map>

#include "RecentBooksStore.h"
#include "util/M4ProviderCoverCache.h"

using namespace M4ProviderCoverCache;

int main() {
  const uint8_t jpegMagic[] = {0xff, 0xd8, 0xff};
  const uint8_t pngMagic[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  const uint8_t webpMagic[] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'};
  const uint8_t unknownMagic[] = {'n', 'o', 't', ' ', 'a', 'n', ' ', 'i', 'm', 'a', 'g', 'e'};
  assert(detectImageFormat(jpegMagic, sizeof(jpegMagic)) == ImageFormat::Jpeg);
  assert(detectImageFormat(pngMagic, sizeof(pngMagic)) == ImageFormat::Png);
  assert(detectImageFormat(webpMagic, sizeof(webpMagic)) == ImageFormat::Webp);
  assert(detectImageFormat(unknownMagic, sizeof(unknownMagic)) == ImageFormat::Unknown);

  M4NovelProvider::BookDetail extensionlessDetail;
  extensionlessDetail.coverUrl = "https://cdn.invalid/image?id=1";
  const auto extensionlessRequest = requestFor("weread", "extensionless", extensionlessDetail, 120, 160);
  assert(extensionlessRequest.coverUrl.find(".jpg") == std::string::npos);

  std::set<std::string> files;
  std::string fetchedUrl;
  size_t fetchedLimit = 0;
  int fetches = 0;
  int conversions = 0;
  Backend backend;
  backend.exists = [&](const std::string& path) { return files.count(path) != 0; };
  backend.makeDirs = [&](const std::string&) { return true; };
  backend.fetch = [&](const std::string& url, const std::string& path, size_t maxBytes) {
    ++fetches;
    fetchedUrl = url;
    fetchedLimit = maxBytes;
    if (url == "https://fixture.invalid/fail.jpg") return false;
    files.insert(path);
    return true;
  };
  backend.convert = [&](const std::string&, const std::string& target, int width, int height) {
    ++conversions;
    assert(width == 120 && height == 160);
    files.insert(target);
    return true;
  };
  backend.remove = [&](const std::string& path) { files.erase(path); };

  const Request request{"weread", "book/unsafe-is-not-in-id", "https://fixture.invalid/cover.jpg", 120, 160};
  assert(bmpTemplatePath("weread", "book") == bmpTemplatePath("weread", "book"));
  assert(bmpTemplatePath("weread", "book") != bmpTemplatePath("fanqie", "book"));

  const auto failed = acquire(
      Request{"weread", "failed", "https://fixture.invalid/fail.jpg", 120, 160}, backend);
  assert(failed.coverBmpPath.empty());
  assert(fetches == 1);

  const auto success = acquire(request, backend);
  assert(success.coverBmpPath == bmpTemplatePath("weread", "book/unsafe-is-not-in-id"));
  assert(!success.cacheHit);
  assert(fetchedUrl == request.coverUrl);
  assert(fetchedLimit == kMaxDownloadBytes);
  assert(conversions == 1);

  const auto hit = acquire(request, backend);
  assert(hit.cacheHit);
  assert(hit.coverBmpPath == success.coverBmpPath);
  assert(fetches == 2);  // one failed fixture + one successful fetch; cache hit fetches none
  assert(conversions == 1);

  const auto empty = acquire(Request{"weread", "book", "", 120, 160}, backend);
  assert(empty.coverBmpPath.empty());

  RecentBook existing{"m4cp://weread/book", "Title", "Author", "/covers/old.bmp", "/cache/ch.txt"};
  assert(mergeProviderMetadata(existing, "", "Updated Author", ""));
  assert(existing.author == "Updated Author");
  assert(existing.coverBmpPath == "/covers/old.bmp");
  assert(!mergeProviderMetadata(existing, "", "", ""));

  std::map<std::string, std::vector<uint8_t>> payloads;
  Backend formatBackend;
  formatBackend.exists = [&](const std::string& path) { return files.count(path) != 0; };
  formatBackend.makeDirs = [&](const std::string&) { return true; };
  formatBackend.fetch = [&](const std::string& url, const std::string& path, size_t) {
    if (url.find("png") != std::string::npos) payloads[path] = std::vector<uint8_t>(pngMagic, pngMagic + sizeof(pngMagic));
    else if (url.find("webp") != std::string::npos) payloads[path] = std::vector<uint8_t>(webpMagic, webpMagic + sizeof(webpMagic));
    else if (url.find("unknown") != std::string::npos) payloads[path] = std::vector<uint8_t>(unknownMagic, unknownMagic + sizeof(unknownMagic));
    else payloads[path] = std::vector<uint8_t>(jpegMagic, jpegMagic + sizeof(jpegMagic));
    files.insert(path);
    return true;
  };
  formatBackend.convert = [&](const std::string& source, const std::string& target, int, int) {
    if (detectImageFormat(payloads[source].data(), payloads[source].size()) != ImageFormat::Jpeg) return false;
    files.insert(target);
    return true;
  };
  formatBackend.remove = [&](const std::string& path) {
    files.erase(path);
    payloads.erase(path);
  };
  for (const char* kindValue : {"png", "webp", "unknown"}) {
    const std::string kind = kindValue;
    const auto rejected = acquire(Request{"weread", "reject-" + kind, "https://cdn.invalid/" + kind,
                                          120, 160}, formatBackend);
    assert(rejected.coverBmpPath.empty());
    assert(files.count(concreteBmpPath("weread", "reject-" + kind, 120, 160)) == 0);
  }
  const auto extensionlessJpeg = acquire(
      Request{"weread", "extensionless", extensionlessRequest.coverUrl, 120, 160}, formatBackend);
  assert(!extensionlessJpeg.coverBmpPath.empty());
  assert(files.count(concreteBmpPath("weread", "extensionless", 120, 160)) != 0);
  std::cout << "provider cover cache bounded/reuse/failure/success passed\n";
}
