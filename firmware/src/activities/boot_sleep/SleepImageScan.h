#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Shared by the three sleep-image directory scans and the host test.
// A yield does not bound the scan: each opened directory entry consumes one
// visit, and the clock is checked before that entry is inspected. One SD or
// header operation already in hand can still run past the clock.

namespace SleepImageScan {

constexpr uint32_t kMaxEntries = 48;
constexpr uint32_t kBudgetMs = 400;

struct Window {
  uint32_t startedMs = 0;
  uint32_t visited = 0;
  bool partial = false;
  bool began = false;

  void begin(uint32_t now) {
    startedMs = now;
    visited = 0;
    partial = false;
    began = true;
  }

  // False means the opened entry is past the window. The caller closes it
  // and stops. Unsigned subtraction stays correct across millis() wrap.
  bool allow(uint32_t now) const {
    if (!began) return false;
    if (visited >= kMaxEntries) return false;
    return (now - startedMs) < kBudgetMs;
  }

  void noteVisited() { ++visited; }
  void markStopped() { partial = true; }
};

// Empty names are skipped before any [0] read. Hidden names start with '.'.
inline bool nameSkipped(const std::string& filename) {
  return filename.empty() || filename[0] == '.';
}

// suffix == nullptr means "no suffix filter" (PNG uses the decoder factory).
inline bool nameHasSuffix(const std::string& filename, const char* suffix) {
  if (!suffix) return true;
  const size_t n = std::char_traits<char>::length(suffix);
  if (filename.size() < n) return false;
  return filename.compare(filename.size() - n, n, suffix) == 0;
}

enum class Step { StopBudget, Skip, Accept };

// Classify one already-opened directory entry. StopBudget: close and break.
// Skip: close and continue. Accept: caller may parse/retain the name.
inline Step beginEntry(Window& window, uint32_t now, bool isDirectory, const std::string& filename,
                       const char* suffix) {
  if (!window.allow(now)) {
    window.markStopped();
    return Step::StopBudget;
  }
  window.noteVisited();
  if (isDirectory || nameSkipped(filename)) return Step::Skip;
  if (!nameHasSuffix(filename, suffix)) return Step::Skip;
  return Step::Accept;
}

}  // namespace SleepImageScan
