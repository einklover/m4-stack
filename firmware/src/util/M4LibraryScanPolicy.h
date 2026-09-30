#pragma once

#include <cstddef>
#include <cstdint>

namespace M4LibraryScanPolicy {
struct Budget {
  static constexpr size_t kMaxEntries = 4096;
  static constexpr size_t kMaxMatches = 512;
  static constexpr unsigned kMaxDepth = 8;
  static constexpr uint32_t kMaxMs = 3000;
  size_t entries = 0;
  size_t matches = 0;
  uint32_t started = 0;
  bool truncated = false;

  explicit Budget(uint32_t now) : started(now) {}
  bool visit(uint32_t now) {
    if (entries >= kMaxEntries || now - started >= kMaxMs) {
      truncated = true;
      return false;
    }
    ++entries;
    return true;
  }
  bool descend(unsigned depth) {
    if (depth >= kMaxDepth) { truncated = true; return false; }
    return true;
  }
  bool match() {
    if (matches >= kMaxMatches) { truncated = true; return false; }
    ++matches;
    return true;
  }
};
}  // namespace M4LibraryScanPolicy
