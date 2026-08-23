#pragma once

#include "apps/providers/M4NativeProviderBookDetail.h"

#include <cstdint>
#include <string>

namespace M4NativeProviderBookDetailAsync {

enum class Phase : uint8_t { Idle = 0, Loading, Ready, Error };

struct Snapshot {
  Phase phase = Phase::Idle;
  std::string providerId;
  std::string appId;
  std::string bookId;
  M4NativeProviderBookDetail::Result result;
  uint32_t startedMs = 0;
  uint32_t updatedMs = 0;
};

// Starts one bounded, process-wide detail enrichment job. The caller keeps
// the seed model visible while this optional request parses off the UI task.
bool start(const M4NativeProviderBookDetail::Request& request);
Snapshot snapshot();
bool busy();
void cancel();

}  // namespace M4NativeProviderBookDetailAsync
