#include <cassert>
#include <iostream>
#include <set>
#include <string>

#include "util/M4ProviderCoverCache.h"

using namespace M4ProviderCoverCache;

int main() {
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
  std::cout << "provider cover cache bounded/reuse/failure/success passed\n";
}
