#pragma once

#include "util/M4ContentProviderContract.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace M4NativeProvider {

enum class Phase : uint8_t {
  Idle = 0,
  Resolving,
  Connecting,
  Receiving,
  Decoding,
  Writing,
  Ready,
  AuthRequired,
  Error,
  Cancelled,
};

struct Progress {
  Phase phase = Phase::Idle;
  std::string providerId;
  std::string bookId;
  std::string chapterUid;
  int chapterIndex0 = -1;
  size_t receivedBytes = 0;
  size_t writtenBytes = 0;
  int percent = 0;
  uint32_t startedMs = 0;
  uint32_t updatedMs = 0;
  std::string error;
};

inline const char* phaseKey(Phase p) {
  switch (p) {
    case Phase::Resolving: return "resolving";
    case Phase::Connecting: return "connecting";
    case Phase::Receiving: return "receiving";
    case Phase::Decoding: return "decoding";
    case Phase::Writing: return "writing";
    case Phase::Ready: return "ready";
    case Phase::AuthRequired: return "auth_required";
    case Phase::Error: return "error";
    case Phase::Cancelled: return "cancelled";
    default: return "idle";
  }
}

struct ChapterRequest {
  M4ContentProvider::BookSpec book;
  M4ContentProvider::ChapterMeta chapter;
  int chapterIndex0 = -1;
  std::string appDataRoot;
  std::string cacheRelPath;
  std::string cacheAbsPath;
  // FileRows raw line is useful to adapters with side fields (JJWXC VIP).
  std::string catalogRawLine;
};

struct FetchResult {
  bool ok = false;
  bool authRequired = false;
  std::string cacheRelPath;
  size_t bytes = 0;
  std::string error;
};

using ProgressFn = std::function<void(Phase phase, size_t received, size_t written, int percent)>;
using CancelFn = std::function<bool()>;

class Adapter {
 public:
  virtual ~Adapter() = default;
  virtual const char* id() const = 0;

  // Blocking implementation runs only on NativeProviderManager's single
  // worker task. It must stream network/decode to SD and never require Lua.
  virtual FetchResult fetchChapter(const ChapterRequest& req,
                                   const ProgressFn& progress,
                                   const CancelFn& cancelled) = 0;
};

}  // namespace M4NativeProvider
