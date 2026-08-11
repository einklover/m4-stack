#pragma once

#include <cstdint>
#include <string>

namespace M4NativeProviderLogin {

enum class Phase : uint8_t {
  Idle = 0,
  Connecting,
  PreparingQr,
  WaitingScan,
  Success,
  Error,
  Cancelled,
};

struct Snapshot {
  Phase phase = Phase::Idle;
  std::string providerId;
  std::string appDataRoot;
  std::string qrUrl;
  std::string status;
  std::string error;
  uint32_t startedMs = 0;
  uint32_t updatedMs = 0;
};

bool start(const std::string& providerId, const std::string& appDataRoot);
Snapshot snapshot();
void cancel();
bool busy();

}  // namespace M4NativeProviderLogin
