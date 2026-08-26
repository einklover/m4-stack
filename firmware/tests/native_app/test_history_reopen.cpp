#include <cassert>
#include <iostream>

#include "util/M4HistoryReopen.h"

using namespace M4HistoryReopen;

int main() {
  const auto exists = [](const std::string&) { return false; };
  const auto installed = [](const std::string& id) { return id == "com.weread.client"; };
  const ProviderAppIdResolver registry = [](const std::string& provider) {
    return provider == "weread" ? "com.weread.client" : "";
  };

  const auto modern = resolveFromRecentBookFields(
      "m4cp://weread/123", "", "三体", "刘慈欣", registry, exists, installed);
  assert(modern.kind == Kind::ProviderNeedsFetch);
  assert(modern.providerId == "weread");
  assert(modern.appId == "com.weread.client");

  const auto legacy = resolveFromRecentBookFields(
      "m4cp://weread/123", "", "三体", "com.weread.client", {}, exists, installed);
  assert(legacy.appId == "com.weread.client");

  assert(appHintForRecentBook("m4cp://weread/123", "", "刘慈欣", registry) ==
         "app:com.weread.client");
  assert(appHintForRecentBook("m4cp://weread/123", "", "com.weread.client", {}) ==
         "app:com.weread.client");
  assert(appHintForRecentBook("/books/a.epub", "", "com.example.author", registry).empty());
  std::cout << "history URI registry + legacy fallback passed\n";
}
