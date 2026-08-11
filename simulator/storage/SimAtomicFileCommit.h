#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace m4sim {

// Transactional file-commit model for the issue #24 catalog/chapter failure
// class. It mirrors the production policy: rename fast path; bounded streaming
// copy fallback when FAT/exFAT rename fails; sync + byte-size verification;
// preserve the previous generation until the replacement is verified.
class SimAtomicFileStore {
 public:
  struct Faults {
    bool renameFails = false;
    bool copyReadFails = false;
    bool copyWriteFails = false;
    bool syncFails = false;
    bool truncateFinal = false;
  };

  struct Result {
    bool ok = false;
    bool usedRename = false;
    bool usedCopyFallback = false;
    size_t copiedMaxChunk = 0;
    std::string error;
  };

  void put(const std::string& path, std::vector<uint8_t> bytes) {
    files_[path] = std::move(bytes);
  }

  bool exists(const std::string& path) const { return files_.count(path) != 0; }
  const std::vector<uint8_t>* get(const std::string& path) const {
    auto it = files_.find(path);
    return it == files_.end() ? nullptr : &it->second;
  }

  void setFaults(Faults f) { faults_ = f; }

  Result commit(const std::string& tempPath, const std::string& finalPath,
                size_t copyChunkBytes = 2048) {
    Result r;
    if (copyChunkBytes == 0 || copyChunkBytes > 2048) {
      r.error = "copy chunk must be within 1..2048 bytes";
      return r;
    }
    auto tempIt = files_.find(tempPath);
    if (tempIt == files_.end()) {
      r.error = "temporary generation missing";
      return r;
    }
    const std::vector<uint8_t> candidate = tempIt->second;
    const bool hadOld = exists(finalPath);
    const std::vector<uint8_t> old = hadOld ? files_.at(finalPath) : std::vector<uint8_t>{};

    if (!faults_.renameFails) {
      files_[finalPath] = candidate;
      files_.erase(tempPath);
      r.usedRename = true;
      if (!verify(finalPath, candidate.size(), r)) {
        restore(finalPath, hadOld, old);
        return r;
      }
      r.ok = true;
      return r;
    }

    r.usedCopyFallback = true;
    const std::string staging = finalPath + ".commit-staging";
    files_[staging].clear();
    files_[staging].reserve(candidate.size());
    size_t offset = 0;
    while (offset < candidate.size()) {
      const size_t n = std::min(copyChunkBytes, candidate.size() - offset);
      r.copiedMaxChunk = std::max(r.copiedMaxChunk, n);
      if (faults_.copyReadFails) {
        files_.erase(staging);
        r.error = "streaming-copy read failure";
        return r;  // old final remains untouched
      }
      if (faults_.copyWriteFails) {
        files_.erase(staging);
        r.error = "streaming-copy write failure";
        return r;
      }
      files_[staging].insert(files_[staging].end(), candidate.begin() + offset,
                             candidate.begin() + offset + n);
      offset += n;
    }

    if (faults_.syncFails) {
      files_.erase(staging);
      r.error = "sync failure";
      return r;
    }
    if (faults_.truncateFinal && !files_[staging].empty()) files_[staging].pop_back();
    if (files_[staging].size() != candidate.size()) {
      files_.erase(staging);
      r.error = "final byte-size verification failed";
      return r;
    }

    // Only after the staging copy has survived sync/size verification do we
    // replace the authoritative generation.
    files_[finalPath] = files_[staging];
    files_.erase(staging);
    files_.erase(tempPath);
    if (!verify(finalPath, candidate.size(), r)) {
      restore(finalPath, hadOld, old);
      return r;
    }
    r.ok = true;
    return r;
  }

 private:
  bool verify(const std::string& path, size_t expected, Result& r) const {
    auto it = files_.find(path);
    if (it == files_.end() || it->second.size() != expected) {
      r.error = "post-commit byte-size verification failed";
      return false;
    }
    return true;
  }

  void restore(const std::string& path, bool hadOld, const std::vector<uint8_t>& old) {
    if (hadOld) files_[path] = old;
    else files_.erase(path);
  }

  std::map<std::string, std::vector<uint8_t>> files_;
  Faults faults_;
};

}  // namespace m4sim
