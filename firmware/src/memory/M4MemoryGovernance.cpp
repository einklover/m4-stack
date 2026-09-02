#include "M4MemoryGovernance.h"

M4MemoryGovernance::M4MemoryGovernance(std::size_t budgetBytes)
    : budgetBytes_(budgetBytes), usedBytes_(0), ownershipRecords_(0) {}

bool M4MemoryGovernance::reserve(std::size_t bytes) {
  if (bytes > budgetBytes_ - usedBytes_) return false;
  usedBytes_ += bytes;
  return true;
}

void M4MemoryGovernance::release(std::size_t bytes) {
  usedBytes_ = bytes >= usedBytes_ ? 0 : usedBytes_ - bytes;
}

bool M4MemoryGovernance::acquireOwnership(std::size_t bytes) {
  if (!reserve(bytes)) return false;
  ++ownershipRecords_;
  return true;
}

void M4MemoryGovernance::releaseOwnership(std::size_t bytes) {
  if (ownershipRecords_ > 0) --ownershipRecords_;
  release(bytes);
}

std::size_t M4MemoryGovernance::remaining() const {
  return budgetBytes_ - usedBytes_;
}

std::size_t M4MemoryGovernance::ownershipRecords() const {
  return ownershipRecords_;
}

bool M4MemoryGovernance::hasLifecycleLeak() const {
  return ownershipRecords_ != 0;
}

bool M4MemoryGovernance::fragmentationStable() const {
  return usedBytes_ <= budgetBytes_;
}
