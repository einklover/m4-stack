#pragma once

#include <cstdint>

enum class M4FileTransferServiceState : uint8_t {
  Stopped,
  StartingNetwork,
  StartingServer,
  Ready,
  Stopping,
  Failed,
};

enum class M4FileTransferMode : uint8_t {
  None,
  JoinNetwork,
  AccessPoint,
};

enum class M4FileTransferError : uint8_t {
  None,
  NetworkStartFailed,
  ServerStartFailed,
  NetworkLost,
  Internal,
};

struct M4FileTransferServiceSnapshot {
  M4FileTransferServiceState state = M4FileTransferServiceState::Stopped;
  M4FileTransferMode mode = M4FileTransferMode::None;
  uint32_t generation = 0;
  M4FileTransferError error = M4FileTransferError::None;
  bool uiMayDetach = true;
  bool cleanupActive = false;
};

// Pure lifecycle contract for the future file-transfer service owner.
//
// This intentionally contains no Arduino, FreeRTOS, ESP-IDF, Activity, socket,
// heap-container, or callback dependency. P1A uses it to make ownership and
// stale-event behavior testable before transport code moves out of the UI.
class M4FileTransferServiceContract {
 public:
  explicit constexpr M4FileTransferServiceContract(const uint32_t initialGeneration = 0)
      : generation_(initialGeneration) {}

  constexpr M4FileTransferServiceSnapshot snapshot() const {
    return M4FileTransferServiceSnapshot{
        state_, mode_, generation_, error_, true, state_ == M4FileTransferServiceState::Stopping};
  }

  // Starts a new generation only from Stopped. Repeated or competing starts
  // while a generation is active are idempotent and keep existing ownership.
  constexpr uint32_t start(const M4FileTransferMode mode) {
    if (state_ != M4FileTransferServiceState::Stopped || mode == M4FileTransferMode::None) {
      return generation_;
    }
    generation_ = nextGeneration(generation_);
    mode_ = mode;
    error_ = M4FileTransferError::None;
    state_ = M4FileTransferServiceState::StartingNetwork;
    return generation_;
  }

  constexpr bool onNetworkReady(const uint32_t generation) {
    if (!isCurrent(generation) || state_ != M4FileTransferServiceState::StartingNetwork) return false;
    state_ = M4FileTransferServiceState::StartingServer;
    return true;
  }

  constexpr bool onServerReady(const uint32_t generation) {
    if (!isCurrent(generation) || state_ != M4FileTransferServiceState::StartingServer) return false;
    state_ = M4FileTransferServiceState::Ready;
    return true;
  }

  constexpr bool fail(const uint32_t generation, const M4FileTransferError error) {
    if (!isCurrent(generation) || error == M4FileTransferError::None ||
        state_ == M4FileTransferServiceState::Stopped || state_ == M4FileTransferServiceState::Stopping) {
      return false;
    }
    error_ = error;
    state_ = M4FileTransferServiceState::Failed;
    return true;
  }

  // Stop is deliberately idempotent. UI detachment is a separate ownership
  // decision and snapshot().uiMayDetach remains true while cleanup is active.
  constexpr void stop() {
    if (state_ == M4FileTransferServiceState::Stopped || state_ == M4FileTransferServiceState::Stopping) return;
    state_ = M4FileTransferServiceState::Stopping;
  }

  // A failed generation must still pass through Stopping before resources may
  // be considered released; resetFailure never skips cleanup.
  constexpr bool resetFailure() {
    if (state_ != M4FileTransferServiceState::Failed) return false;
    state_ = M4FileTransferServiceState::Stopping;
    return true;
  }

  constexpr bool completeStop(const uint32_t generation) {
    if (!isCurrent(generation) || state_ != M4FileTransferServiceState::Stopping) return false;
    state_ = M4FileTransferServiceState::Stopped;
    mode_ = M4FileTransferMode::None;
    error_ = M4FileTransferError::None;
    return true;
  }

 private:
  static constexpr uint32_t nextGeneration(const uint32_t generation) {
    const uint32_t next = generation + 1U;
    return next == 0U ? 1U : next;
  }

  constexpr bool isCurrent(const uint32_t generation) const {
    return generation != 0U && generation == generation_;
  }

  M4FileTransferServiceState state_ = M4FileTransferServiceState::Stopped;
  M4FileTransferMode mode_ = M4FileTransferMode::None;
  uint32_t generation_ = 0;
  M4FileTransferError error_ = M4FileTransferError::None;
};
