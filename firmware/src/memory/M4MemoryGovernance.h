#pragma once

#include <cstddef>

class M4MemoryGovernance {
 public:
  explicit M4MemoryGovernance(std::size_t budgetBytes = 64 * 1024)
      : budgetBytes_(budgetBytes), usedBytes_(0), ownershipRecords_(0) {}

  bool reserve(std::size_t bytes) {
    if (bytes > budgetBytes_ - usedBytes_) return false;
    usedBytes_ += bytes;
    return true;
  }

  void release(std::size_t bytes) {
    usedBytes_ = bytes >= usedBytes_ ? 0 : usedBytes_ - bytes;
  }

  bool acquireOwnership(std::size_t bytes) {
    if (!reserve(bytes)) return false;
    ++ownershipRecords_;
    return true;
  }

  void releaseOwnership(std::size_t bytes) {
    if (ownershipRecords_ > 0) --ownershipRecords_;
    release(bytes);
  }

  std::size_t remaining() const { return budgetBytes_ - usedBytes_; }

  std::size_t ownershipRecords() const { return ownershipRecords_; }

  bool hasLifecycleLeak() const { return ownershipRecords_ != 0; }

  bool fragmentationStable() const { return usedBytes_ <= budgetBytes_; }

 private:
  std::size_t budgetBytes_;
  std::size_t usedBytes_;
  std::size_t ownershipRecords_;
};
