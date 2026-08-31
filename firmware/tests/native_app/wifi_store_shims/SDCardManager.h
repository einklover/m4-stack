#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace wifi_store_test_sd {
inline std::map<std::string, std::string> files;
inline bool failWrite = false;
inline bool failSync = false;
inline bool failRename = false;
inline bool failOpenWrite = false;
inline bool failMkdir = false;
}

class FsFile {
 public:
  bool isOpen() const { return open_; }
  void attach(const char* path, bool write) {
    path_ = path ? path : "";
    write_ = write;
    pos_ = 0;
    open_ = true;
  }
  int write(const uint8_t* data, size_t len) {
    if (!open_ || !write_ || wifi_store_test_sd::failWrite) return 0;
    auto& body = wifi_store_test_sd::files[path_];
    if (pos_ + len > body.size()) body.resize(pos_ + len);
    std::copy(data, data + len, body.begin() + static_cast<std::ptrdiff_t>(pos_));
    pos_ += len;
    return static_cast<int>(len);
  }
  int read(uint8_t* data, size_t len) {
    if (!open_ || write_) return 0;
    const auto it = wifi_store_test_sd::files.find(path_);
    if (it == wifi_store_test_sd::files.end()) return 0;
    const size_t n = std::min(len, it->second.size() - std::min(pos_, it->second.size()));
    std::copy_n(reinterpret_cast<const uint8_t*>(it->second.data()) + pos_, n, data);
    pos_ += n;
    return static_cast<int>(n);
  }
  uint64_t fileSize() const {
    const auto it = wifi_store_test_sd::files.find(path_);
    return it == wifi_store_test_sd::files.end() ? 0 : it->second.size();
  }
  bool sync() { return open_ && !wifi_store_test_sd::failSync; }
  bool close() {
    open_ = false;
    return true;
  }

 private:
  std::string path_;
  size_t pos_ = 0;
  bool open_ = false;
  bool write_ = false;
};

class SDCardManager {
 public:
  bool mkdir(const char*, bool = false) { return !wifi_store_test_sd::failMkdir; }
  bool exists(const char* path) const { return path && wifi_store_test_sd::files.count(path) != 0; }
  bool remove(const char* path) {
    return path && wifi_store_test_sd::files.erase(path) != 0;
  }
  bool rename(const char* from, const char* to) {
    if (!from || !to || wifi_store_test_sd::failRename) return false;
    const auto it = wifi_store_test_sd::files.find(from);
    if (it == wifi_store_test_sd::files.end()) return false;
    wifi_store_test_sd::files[to] = it->second;
    wifi_store_test_sd::files.erase(it);
    return true;
  }
  bool openFileForWrite(const char*, const char* path, FsFile& file) {
    if (!path || wifi_store_test_sd::failOpenWrite) return false;
    wifi_store_test_sd::files[path].clear();
    file.attach(path, true);
    return true;
  }
  bool openFileForRead(const char*, const char* path, FsFile& file) {
    if (!path || !exists(path)) return false;
    file.attach(path, false);
    return true;
  }
};

inline SDCardManager SdMan;
