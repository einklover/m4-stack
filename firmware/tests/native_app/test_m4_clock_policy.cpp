#include "apps/M4ClockPolicy.h"

#include <cassert>
#include <iostream>

int main() {
  using namespace M4ClockPolicy;
  assert(!clockLooksSane(0));
  assert(!clockLooksSane(12345));
  assert(!clockLooksSane(kMinSaneUnix - 1));
  assert(clockLooksSane(kMinSaneUnix));
  assert(clockLooksSane(1735689600LL));  // 2025-01-01
  assert(clockLooksSane(1756080000LL));  // ~2025-08
  assert(!clockLooksSane(kMaxSaneUnix));
  assert(!clockLooksSane(2000000000LL));

  assert(shouldAttemptOnlineSync(0, false));
  assert(!shouldAttemptOnlineSync(0, true));
  assert(!shouldAttemptOnlineSync(1735689600LL, false));
  assert(!shouldAttemptOnlineSync(1735689600LL, true));
  std::cout << "m4 clock policy OK\n";
  return 0;
}
