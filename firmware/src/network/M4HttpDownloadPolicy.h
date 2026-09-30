#pragma once

#include <cstddef>
#include <cstdint>

namespace M4HttpDownloadPolicy {
constexpr size_t kMaxTextBytes = 64u * 1024u;
constexpr size_t kMaxFeedBytes = 512u * 1024u;
constexpr size_t kMaxFileBytes = 128u * 1024u * 1024u;
constexpr uint32_t kIdleTimeoutMs = 15000;
constexpr uint32_t kTotalTimeoutMs = 10u * 60u * 1000u;
constexpr uint32_t kTextTotalTimeoutMs = 45u * 1000u;
inline bool timedOut(uint32_t now, uint32_t started, uint32_t progressed) {
  return now - started >= kTotalTimeoutMs || now - progressed >= kIdleTimeoutMs;
}
inline bool exceeds(size_t current, size_t next, size_t limit) {
  return current > limit || next > limit - current;
}
}  // namespace M4HttpDownloadPolicy
