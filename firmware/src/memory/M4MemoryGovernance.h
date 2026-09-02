#pragma once

#include <cstddef>

class M4MemoryGovernance {
 public:
  explicit M4MemoryGovernance(std::size_t budgetBytes = 64 * 1024)
      : budgetBytes_(budgetBytes), usedBytes_(0) {}

  bool reserve(std::size_t bytes) {
    if (bytes > budgetBytes_ - usedBytes_) return false;
    usedBytes_ += bytes;
    return true;
  }

  void release(std::size_t bytes) {
    usedBytes_ = bytes >= usedBytes_ ? 0 : usedBytes_ - bytes;
  }

  std::size_t remaining() const { return budgetBytes_ - usedBytes_; }

  bool fragmentationStable() const { return usedBytes_ <= budgetBytes_; }

 private:
  std::size_t budgetBytes_;
  std::size_t usedBytes_;
};
