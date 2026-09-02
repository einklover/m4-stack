#pragma once

#include <cstddef>

class M4MemoryGovernance {
 public:
  explicit M4MemoryGovernance(std::size_t budgetBytes = 64 * 1024);

  bool reserve(std::size_t bytes);
  void release(std::size_t bytes);

  bool acquireOwnership(std::size_t bytes);
  void releaseOwnership(std::size_t bytes);

  std::size_t remaining() const;
  std::size_t ownershipRecords() const;
  bool hasLifecycleLeak() const;
  bool fragmentationStable() const;

 private:
  std::size_t budgetBytes_;
  std::size_t usedBytes_;
  std::size_t ownershipRecords_;
};
