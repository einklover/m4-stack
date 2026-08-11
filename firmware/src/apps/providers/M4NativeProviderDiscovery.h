#pragma once

#include <cstdint>
#include <string>

namespace M4NativeProviderDiscovery {

enum class Phase : uint8_t { Idle = 0, Connecting, Receiving, Ready, AuthRequired, Error };

struct Snapshot {
  Phase phase = Phase::Idle;
  std::string providerId;
  std::string appId;
  std::string category;
  size_t receivedBytes = 0;
  size_t rowCount = 0;
  uint32_t startedMs = 0;
  uint32_t updatedMs = 0;
  std::string error;
};

// Starts a bounded default discovery job only when one is not already running.
// WeRead fetches the authenticated shelf. Fanqie/JJWXC fetch one public default
// recommendation channel so a fresh zero-Lua install is immediately usable.
bool startDefault(const std::string& providerId, const std::string& appId);
bool startCategory(const std::string& providerId, const std::string& appId,
                   const std::string& category);

Snapshot snapshot();
bool busy();

}  // namespace M4NativeProviderDiscovery
