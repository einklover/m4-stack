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
  assert(canConvertImageFormat(ImageFormat::Bmp));
  assert(canConvertImageFormat(ImageFormat::Jpeg));
  assert(canConvertImageFormat(ImageFormat::Png));
  assert(!canConvertImageFormat(ImageFormat::Webp));
  assert(!canConvertImageFormat(ImageFormat::Unknown));

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
    else if (url.find("bmp") != std::string::npos) payloads[path] = {'B', 'M', 0, 0};
    else if (url.find("webp") != std::string::npos) payloads[path] = std::vector<uint8_t>(webpMagic, webpMagic + sizeof(webpMagic));
    else if (url.find("unknown") != std::string::npos) payloads[path] = std::vector<uint8_t>(unknownMagic, unknownMagic + sizeof(unknownMagic));
    else payloads[path] = std::vector<uint8_t>(jpegMagic, jpegMagic + sizeof(jpegMagic));
    files.insert(path);
    return true;
  };
  formatBackend.convert = [&](const std::string& source, const std::string& target, int, int) {
    if (!canConvertImageFormat(
            detectImageFormat(payloads[source].data(), payloads[source].size()))) return false;
    files.insert(target);
    return true;
  };
  formatBackend.remove = [&](const std::string& path) {
    files.erase(path);
    payloads.erase(path);
  };
  const auto pngSuccess = acquire(
      Request{"weread", "accept-png", "https://cdn.invalid/png", 120, 160}, formatBackend);
  assert(!pngSuccess.coverBmpPath.empty());
  assert(files.count(concreteBmpPath("weread", "accept-png", 120, 160)) != 0);

  for (const char* kindValue : {"bmp", "jpeg"}) {
    const std::string kind = kindValue;
    const auto accepted = acquire(Request{"weread", "accept-" + kind,
                                         "https://cdn.invalid/" + kind, 120, 160},
                                  formatBackend);
    assert(!accepted.coverBmpPath.empty());
  }

  for (const char* kindValue : {"webp", "unknown"}) {
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

  // Home Scene sizes: generate on miss from source.img, never fetch.
  std::set<std::string> homeFiles;
  int homeFetches = 0;
  int homeConverts = 0;
  int lastConvertW = 0;
  int lastConvertH = 0;
  std::string lastConvertSource;
  Backend homeBackend;
  homeBackend.exists = [&](const std::string& path) { return homeFiles.count(path) != 0; };
  homeBackend.fetch = [&](const std::string&, const std::string&, size_t) {
    ++homeFetches;
    return false;
  };
  homeBackend.convert = [&](const std::string& source, const std::string& target, int width, int height) {
    ++homeConverts;
    lastConvertSource = source;
    lastConvertW = width;
    lastConvertH = height;
    homeFiles.insert(target);
    return true;
  };
  homeBackend.remove = [&](const std::string& path) { homeFiles.erase(path); };
  const std::string templatePath =
      "/.crosspoint/provider_covers/032352886ae8bcbc/cover_[WIDTH]x[HEIGHT].bmp";
  const std::string sourceImg = "/.crosspoint/provider_covers/032352886ae8bcbc/source.img";
  const std::string fengyan = "/.crosspoint/provider_covers/032352886ae8bcbc/cover_171x254.bmp";
  homeFiles.insert(sourceImg);
  homeFiles.insert(fengyan);

  const auto miss110 = ensureSizedCoverFromSource(templatePath, 110, 180, homeBackend);
  assert(!miss110.thumbPath.empty());
  assert(miss110.generated);
  assert(!miss110.cacheHit);
  assert(homeFetches == 0);
  assert(homeConverts == 1);
  assert(lastConvertSource == sourceImg);
  assert(lastConvertW == 110 && lastConvertH == 180);
  assert(homeFiles.count("/.crosspoint/provider_covers/032352886ae8bcbc/cover_110x180.bmp") != 0);

  const auto hit110 = ensureSizedCoverFromSource(templatePath, 110, 180, homeBackend);
  assert(hit110.cacheHit);
  assert(!hit110.generated);
  assert(homeConverts == 1);
  assert(homeFetches == 0);

  const auto miss74 = ensureSizedCoverFromSource(templatePath, 74, 106, homeBackend);
  assert(miss74.generated);
  assert(lastConvertW == 74 && lastConvertH == 106);
  assert(homeConverts == 2);

  homeFiles.erase(sourceImg);
  const auto noSource = ensureSizedCoverFromSource(templatePath, 64, 64, homeBackend);
  assert(noSource.thumbPath.empty());
  assert(homeConverts == 2);

  const auto outside = ensureSizedCoverFromSource("/books/local/cover_[WIDTH]x[HEIGHT].bmp", 110, 180, homeBackend);
  assert(outside.thumbPath.empty());

  bool cancelFlag = true;
  const auto cancelled = ensureSizedCoverFromSource(
      templatePath, 50, 80, homeBackend, [&]() { return cancelFlag; });
  assert(cancelled.thumbPath.empty());
  assert(homeFetches == 0);

  std::cout << "provider cover cache bounded/reuse/failure/success passed\n";
}
