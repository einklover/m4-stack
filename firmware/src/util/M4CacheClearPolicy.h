#pragma once

#include <cstring>

namespace M4CacheClearPolicy {
inline bool keepReaderProgress(const char* name, bool isDirectory) {
  return !isDirectory && name &&
         (std::strcmp(name, "progress.bin") == 0 || std::strcmp(name, "progress.dat") == 0 ||
          std::strcmp(name, "progress.tmp") == 0);
}
}  // namespace M4CacheClearPolicy
