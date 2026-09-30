#pragma once

#include <cstring>

namespace M4CacheClearPolicy {
inline bool equalAsciiFolded(const char* a, const char* b) {
  if (!a || !b) return false;
  while (*a && *b) {
    const unsigned char c = static_cast<unsigned char>(*a++);
    const unsigned char d = static_cast<unsigned char>(*b++);
    if ((c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c) !=
        (d >= 'A' && d <= 'Z' ? d + ('a' - 'A') : d)) return false;
  }
  return *a == *b;
}
inline bool keepReaderProgress(const char* name, bool isDirectory) {
  return !isDirectory && name &&
         (equalAsciiFolded(name, "progress.bin") || equalAsciiFolded(name, "progress.dat") ||
          equalAsciiFolded(name, "progress.tmp"));
}
}  // namespace M4CacheClearPolicy
