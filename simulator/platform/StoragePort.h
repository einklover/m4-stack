// Platform-neutral asynchronous storage contract.
//
// ReaderModel models ordering and liveness, not SdFat/SPI details. Backends
// translate this small contract to deterministic simulated SD, host files, or
// the real ESP32 storage implementation.
#pragma once

#include <cstddef>
#include <functional>
#include <string>

namespace m4platform {

class StoragePort {
public:
  virtual ~StoragePort() = default;

  // Read `bytes` associated with a logical operation. The callback receives
  // the number of bytes actually read; zero represents a failed/short read in
  // the behavioral model.
  virtual void read(const std::string& what, size_t bytes, double speedScale,
                    std::function<void(size_t)> onDone) = 0;
};

}  // namespace m4platform
