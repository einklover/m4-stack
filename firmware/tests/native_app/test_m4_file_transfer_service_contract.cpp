#include <cassert>
#include <cstdint>

#include "../../src/network/M4FileTransferServiceContract.h"

int main() {
  {
    M4FileTransferServiceContract contract;
    const auto initial = contract.snapshot();
    assert(initial.state == M4FileTransferServiceState::Stopped);
    assert(initial.mode == M4FileTransferMode::None);
    assert(initial.generation == 0);
    assert(initial.error == M4FileTransferError::None);
    assert(initial.uiMayDetach);
    assert(!initial.cleanupActive);
  }

  {
    M4FileTransferServiceContract contract;
    const uint32_t generation = contract.start(M4FileTransferMode::JoinNetwork);
    assert(generation != 0);

    auto snap = contract.snapshot();
    assert(snap.state == M4FileTransferServiceState::StartingNetwork);
    assert(snap.mode == M4FileTransferMode::JoinNetwork);
    assert(snap.generation == generation);

    const uint32_t duplicate = contract.start(M4FileTransferMode::AccessPoint);
    assert(duplicate == generation);
    snap = contract.snapshot();
    assert(snap.state == M4FileTransferServiceState::StartingNetwork);
    assert(snap.mode == M4FileTransferMode::JoinNetwork);

    assert(contract.onNetworkReady(generation));
    assert(contract.snapshot().state == M4FileTransferServiceState::StartingServer);
    assert(contract.onServerReady(generation));
    assert(contract.snapshot().state == M4FileTransferServiceState::Ready);

    contract.stop();
    snap = contract.snapshot();
    assert(snap.state == M4FileTransferServiceState::Stopping);
    assert(snap.uiMayDetach);
    assert(snap.cleanupActive);

    contract.stop();
    assert(contract.snapshot().generation == generation);
    assert(contract.snapshot().state == M4FileTransferServiceState::Stopping);

    assert(contract.completeStop(generation));
    snap = contract.snapshot();
    assert(snap.state == M4FileTransferServiceState::Stopped);
    assert(snap.mode == M4FileTransferMode::None);
    assert(snap.error == M4FileTransferError::None);
    assert(!snap.cleanupActive);
  }

  {
    M4FileTransferServiceContract contract;
    const uint32_t oldGeneration = contract.start(M4FileTransferMode::AccessPoint);
    contract.stop();
    assert(contract.completeStop(oldGeneration));

    const uint32_t currentGeneration = contract.start(M4FileTransferMode::JoinNetwork);
    assert(currentGeneration != oldGeneration);
    assert(!contract.onNetworkReady(oldGeneration));
    assert(!contract.onServerReady(oldGeneration));
    assert(!contract.fail(oldGeneration, M4FileTransferError::NetworkLost));
    assert(contract.snapshot().state == M4FileTransferServiceState::StartingNetwork);
    assert(contract.snapshot().generation == currentGeneration);
  }

  {
    M4FileTransferServiceContract contract;
    const uint32_t generation = contract.start(M4FileTransferMode::JoinNetwork);
    assert(contract.fail(generation, M4FileTransferError::NetworkStartFailed));
    auto snap = contract.snapshot();
    assert(snap.state == M4FileTransferServiceState::Failed);
    assert(snap.error == M4FileTransferError::NetworkStartFailed);
    assert(snap.uiMayDetach);

    assert(contract.resetFailure());
    snap = contract.snapshot();
    assert(snap.state == M4FileTransferServiceState::Stopping);
    assert(snap.cleanupActive);
    assert(contract.completeStop(generation));

    const uint32_t restarted = contract.start(M4FileTransferMode::JoinNetwork);
    assert(restarted != generation);
    assert(contract.snapshot().state == M4FileTransferServiceState::StartingNetwork);
  }

  {
    M4FileTransferServiceContract contract(UINT32_MAX);
    const uint32_t generation = contract.start(M4FileTransferMode::JoinNetwork);
    assert(generation == 1);
  }

  return 0;
}
