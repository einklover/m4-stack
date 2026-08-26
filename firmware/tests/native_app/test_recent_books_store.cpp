#include <cstdlib>
#include <iostream>
#include <string>

#include "RecentBooksStore.h"
#include <WString.h>

namespace StringUtils {
bool checkFileExtension(const std::string&, const char*) { return false; }
bool checkFileExtension(const String&, const char*) { return false; }
}  // namespace StringUtils

int main() {
  const auto require = [](bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << "\n";
    std::exit(1);
  };

  auto& store = RecentBooksStore::getInstance();
  const std::string uri = "m4cp://fanqie/cover-persistence";
  const std::string cover = "/.crosspoint/provider_covers/fanqie-cover.bmp";

  store.addBook(uri, "番茄书名", "原作者", cover);
  store.addBook(uri, "", "", "");

  require(store.getCount() == 1, "same URI should replace one history row");
  const RecentBook& book = store.getBooks().front();
  require(book.title == "番茄书名", "title should survive an empty replacement");
  require(book.author == "原作者", "author should survive an empty replacement");
  require(book.coverBmpPath == cover, "cover should survive an empty replacement");

  // Provider updates remain merge-safe when a later fetch has no metadata.
  store.updateProviderBook(uri, "", "", "");
  const RecentBook& afterEmptyProviderUpdate = store.getBooks().front();
  require(afterEmptyProviderUpdate.author == "原作者", "empty provider update should preserve author");
  require(afterEmptyProviderUpdate.coverBmpPath == cover, "empty provider update should preserve cover");

  std::cout << "recent book provider metadata survives empty replacement/update\n";
}
