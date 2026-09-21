#pragma once

#include <cstddef>

#include "M4MemoryGovernance.h"

// Logical service-owned charges, not byte-accurate accounting for opaque
// ESP-IDF/Wi-Fi internals. The account makes admission and lifecycle release
// explicit for resources M4FileTransferService directly owns.
class M4FileTransferMemoryAccount final {
 public:
  static constexpr std::size_t kBudgetBytes = 64 * 1024;
  static constexpr std::size_t kDnsChargeBytes = 4 * 1024;
  static constexpr std::size_t kHttpRuntimeChargeBytes = 32 * 1024;

  bool acquireDns() {
    if (dnsOwned_) return true;
    if (!governance_.acquireOwnership(kDnsChargeBytes)) return false;
    dnsOwned_ = true;
    return true;
  }

  void releaseDns() {
    if (!dnsOwned_) return;
    dnsOwned_ = false;
    governance_.releaseOwnership(kDnsChargeBytes);
  }

  bool acquireHttpRuntime() {
    if (httpRuntimeOwned_) return true;
    if (!governance_.acquireOwnership(kHttpRuntimeChargeBytes)) return false;
    httpRuntimeOwned_ = true;
    return true;
  }

  void releaseHttpRuntime() {
    if (!httpRuntimeOwned_) return;
    httpRuntimeOwned_ = false;
    governance_.releaseOwnership(kHttpRuntimeChargeBytes);
  }

  bool clean() const {
    return !dnsOwned_ && !httpRuntimeOwned_ && !governance_.hasLifecycleLeak();
  }

  std::size_t ownershipRecords() const { return governance_.ownershipRecords(); }
  std::size_t remaining() const { return governance_.remaining(); }

 private:
  M4MemoryGovernance governance_{kBudgetBytes};
  bool dnsOwned_ = false;
  bool httpRuntimeOwned_ = false;
};
