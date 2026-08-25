#include <cassert>
#include <iostream>

#include "apps/providers/M4NovelProviderContract.h"

int main() {
  M4NovelProvider::BookCard card;
  M4NovelProvider::BookDetail detail;
  assert(card.coverUrl.empty());
  assert(detail.coverUrl.empty());
  card.coverUrl = "https://fixture.invalid/cover.jpg";
  detail.coverUrl = card.coverUrl;
  assert(card.coverUrl == detail.coverUrl);
  std::cout << "provider coverUrl contract passed\n";
}
