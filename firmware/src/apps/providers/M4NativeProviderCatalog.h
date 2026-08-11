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
  Ready,        // catalog usable (may still be partial=true while titles refill)
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
  // When true, rowCount/titles may still grow; reader/TOC may open already.
  bool partial = false;
  // Known total from shelf metadata when available (e.g. Legado totalChapterNum).
  size_t totalHint = 0;
  // Resume / open focus chapter (0-based).
  int currentIndex0 = 0;
  uint32_t startedMs = 0;
  uint32_t updatedMs = 0;
  std::string error;
};

// Start a single process-wide catalog bootstrap. Shared progressive policy:
// first window → Ready (partial), optional full refill in the same task.
// focusIndex0 centers any future windowed fill (currently used as resume hint).
// It never starts Lua/AppRuntime.
bool start(const std::string& providerId, const std::string& bookId,
           const std::string& appId, const std::string& title, int focusIndex0 = 0);
Snapshot snapshot();
bool busy();
void cancel();

}  // namespace M4NativeProviderCatalog
