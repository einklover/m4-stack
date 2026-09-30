#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
inline std::vector<uint8_t> disk;
inline unsigned long ioDelayMs = 0;
inline int openedFiles = 0;
inline int reads = 0;
inline int seeks = 0;
inline int failRead = -1;  // 1-based read() call that returns a short count
inline int failSeek = -1;  // 1-based seekSet() call that returns false
constexpr int O_RDWR = 1, O_CREAT = 2, O_TRUNC = 4;
struct FsFile {
  bool opened = false;
  size_t pos = 0;
  explicit operator bool() const { return opened; }
  uint64_t fileSize() const { return disk.size(); }
  bool seekSet(uint32_t p) {
    fakeMillis += ioDelayMs;
    ++seeks;
    if (seeks == failSeek) return false;
    pos = p;
    return p <= disk.size();
  }
  int read(void* p, size_t n) {
    fakeMillis += ioDelayMs;
    ++reads;
    if (reads == failRead) return 4;  // present on disk, short this call
    size_t got = std::min(n, disk.size() - std::min(pos, disk.size()));
    std::memcpy(p, disk.data() + pos, got);
    pos += got;
    return static_cast<int>(got);
  }
  int write(const void* p, size_t n) {
    if (pos + n > disk.size()) disk.resize(pos + n);
    std::memcpy(disk.data() + pos, p, n);
    pos += n;
    return static_cast<int>(n);
  }
  bool close() {
    if(opened) --openedFiles;
    opened = false;
    return true;
  }
  bool sync() { return true; }
};
struct SdType {
  bool ready() { return true; }
  bool exists(const char*) { return !disk.empty(); }
  bool openFileForRead(const char*, const char*, FsFile& f) {
    f.opened = !disk.empty();
    f.pos = 0;
    if(f.opened) ++openedFiles;
    return f.opened;
  }
  FsFile open(const char*, int flags) {
    if (flags & O_TRUNC) disk.clear();
    ++openedFiles;
    return FsFile{true, 0};
  }
  void ensureDirectoryExists(const char*) {}
};
inline SdType SdMan;
