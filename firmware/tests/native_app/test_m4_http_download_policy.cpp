#include <cassert>
#include <cstdint>
#include "../../src/network/M4HttpDownloadPolicy.h"

int main() {
  using namespace M4HttpDownloadPolicy;
  assert(!exceeds(100, 10, 110));
  assert(exceeds(100, 11, 110));
  assert(exceeds(static_cast<size_t>(-1), 1, kMaxFileBytes));
  assert(!timedOut(14999, 0, 0));
  assert(timedOut(15000, 0, 0));
  assert(timedOut(kTotalTimeoutMs, 0, kTotalTimeoutMs - 1));
  assert(!timedOut(10, UINT32_MAX - 5, UINT32_MAX - 5));
}
