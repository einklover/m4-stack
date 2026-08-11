#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace M4NativeProviderCatalog {

enum class Phase : uint8_t {
  Idle = 0,
  Connecting,
  Receiving,
  Registering,
  Ready,
  AuthRequired,
  Error,
};

struct Snapshot {
  Phase phase = Phase::Idle;
  std::string providerId;
  std::string appId;
  std::string bookId;
  std::string title;
  size_t receivedBytes = 0;
  size_t rowCount = 0;
  uint32_t startedMs = 0;
  uint32_t updatedMs = 0;
  std::string error;
};

// Start a single process-wide catalog bootstrap. The task streams a provider
// TOC into transactional FileRows and registers a persistent native BookSpec.
// It never starts Lua/AppRuntime.
bool start(const std::string& providerId, const std::string& bookId,
           const std::string& appId, const std::string& title);
Snapshot snapshot();
bool busy();
void cancel();

}  // namespace M4NativeProviderCatalog
