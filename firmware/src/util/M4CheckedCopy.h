#pragma once
#include <cstddef>
#include <cstdint>

namespace M4CheckedCopy {
// Keep the signed read result: SdFat uses -1 for an I/O error. Only a complete,
// synced copy may authorize removal of the source. No allocation on this path.
template <typename Source, typename Destination, typename Yield>
bool copy(Source& source, Destination& destination, uint64_t expected, Yield yield) {
  uint8_t buffer[1024];
  uint64_t copied = 0;
  while (copied < expected) {
    const size_t wanted = expected - copied < sizeof(buffer) ? expected - copied : sizeof(buffer);
    const int n = source.read(buffer, wanted);
    if (n <= 0 || static_cast<size_t>(n) > wanted ||
        destination.write(buffer, static_cast<size_t>(n)) != static_cast<size_t>(n)) return false;
    copied += static_cast<size_t>(n);
    yield();
  }
  return destination.sync();
}
}  // namespace M4CheckedCopy
