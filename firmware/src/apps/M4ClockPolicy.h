#pragma once

#include <cstdint>

namespace M4ClockPolicy {

// Unix bounds for a clock that can both (a) satisfy TLS not-before and (b)
// produce a Fanqie `nt=YYYY-MM-DD` the :8043 date gate will accept.
// Below this, ESP32 is usually still at epoch after a dead RTC.
inline constexpr std::int64_t kMinSaneUnix = 1700000000LL;  // 2023-11-14
// Above this, leaf certs issued around 2025-2026 look expired.
inline constexpr std::int64_t kMaxSaneUnix = 1830297600LL;  // 2028-01-01

inline bool clockLooksSane(std::int64_t nowUnix) {
  return nowUnix >= kMinSaneUnix && nowUnix < kMaxSaneUnix;
}

// One online SNTP attempt per boot, and only while the clock is out of range.
inline bool shouldAttemptOnlineSync(std::int64_t nowUnix, bool alreadyAttemptedThisBoot) {
  if (alreadyAttemptedThisBoot) return false;
  return !clockLooksSane(nowUnix);
}

}  // namespace M4ClockPolicy
