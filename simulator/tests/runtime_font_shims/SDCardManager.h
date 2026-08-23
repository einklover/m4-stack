#pragma once

#include <cstddef>
#include <cstdint>

class FsFile {
 public:
  bool isOpen() const { return false; }
  void close() {}
  uint64_t fileSize() const { return 0; }
  bool seekSet(uint32_t) { return false; }
  void rewind() {}
  int read(void*, size_t) { return 0; }
  uint64_t curPosition() const { return 0; }
  uint8_t getError() const { return 0; }
};

class SDCardManager {
 public:
  bool openFileForRead(const char*, const char*, FsFile&) { return false; }
};

inline SDCardManager SdMan;
