// Host regression for the Legado short bookId derivation (M4LegadoBridge).
// Uses real /getBookshelf bookUrl locators to verify:
//   * FNV-1a ids are stable and deterministic
//   * ids are exactly 16 lowercase hex chars (M4-safe: no '/', '?', '#', ' ')
//   * distinct locators map to distinct ids
#include <cassert>
#include <cstdio>
#include <cstring>

#include "apps/providers/M4LegadoBridge.h"

// Generated from a real Legado /getBookshelf response.
#include "legado_fixture.inc"

int main() {
  assert(kLegadoShelfFixtureCount >= 3);
  for (size_t i = 0; i < kLegadoShelfFixtureCount; ++i) {
    const std::string id = M4LegadoBridge::shortId(kLegadoShelfFixture[i].url);
    assert(M4LegadoBridge::idOkShort(id));
    assert(id.size() == 16);
    // Deterministic: same locator twice -> same id.
    assert(id == M4LegadoBridge::shortId(kLegadoShelfFixture[i].url));
    // Distinct locators -> distinct ids.
    for (size_t j = i + 1; j < kLegadoShelfFixtureCount; ++j) {
      assert(id != M4LegadoBridge::shortId(kLegadoShelfFixture[j].url));
    }
    printf("id=%s url_len=%zu name=%s\n", id.c_str(),
           std::strlen(kLegadoShelfFixture[i].url), kLegadoShelfFixture[i].name);
  }
  printf("legado short-id derivation: PASS (%zu books, ids stable + M4-safe)\n",
         kLegadoShelfFixtureCount);
  return 0;
}
