#include <cassert>
#include "../../src/util/M4LibraryScanPolicy.h"

int main() {
  using M4LibraryScanPolicy::Budget;
  Budget b(100);
  assert(b.visit(100));
  assert(b.descend(Budget::kMaxDepth - 1));
  assert(!b.descend(Budget::kMaxDepth) && b.truncated);
  Budget timeout(100);
  assert(timeout.visit(100 + Budget::kMaxMs - 1));
  assert(!timeout.visit(100 + Budget::kMaxMs));
  Budget wrap(0xfffffff0u);
  assert(wrap.visit(0x10u));
  Budget matches(0);
  for (size_t i = 0; i < Budget::kMaxMatches; ++i) assert(matches.match());
  assert(!matches.match() && matches.truncated);
  Budget entries(0);
  for (size_t i = 0; i < Budget::kMaxEntries; ++i) assert(entries.visit(0));
  assert(!entries.visit(0) && entries.truncated);
}
