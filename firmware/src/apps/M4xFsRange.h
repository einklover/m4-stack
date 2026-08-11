#pragma once

// Host-testable policy for fs.fileSize / fs.readRange window I/O.
// Does not perform SD I/O itself; callers supply size/read callbacks.

#include "apps/M4xPathSafe.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace M4xFsRange {

// Maximum bytes returned by one fs.readRange call (API hard cap).
inline constexpr size_t kMaxLength = M4xPathSafe::kMaxReadRangeBytes;  // 16384

// M4xRuntime FreeRTOS task stack is 12288 bytes. Never place a full-range
// read buffer on that stack. Hosts must heap/PSRAM-allocate when the
// requested window exceeds this safe stack ceiling.
inline constexpr size_t kMaxSafeStackReadBytes = 1536;

// Compile-time guard: API max must not be treated as a stack-safe size.
static_assert(kMaxLength > kMaxSafeStackReadBytes,
              "kMaxLength must exceed stack-safe size so heap path is required");
static_assert(kMaxSafeStackReadBytes < 12288 / 4,
              "stack-safe read buffer must stay well under M4xRuntime stack");

// True when the host must allocate the read buffer off-stack (heap/PSRAM).
inline constexpr bool mustHeapAllocateReadBuffer(size_t length) {
  return length > kMaxSafeStackReadBytes;
}

// Policy regression helper: a proposed stack buffer size is illegal if it
// can hold a full max-range read (or anything above the safe ceiling).
inline constexpr bool isStackBufferSafe(size_t stackBufBytes) {
  return stackBufBytes > 0 && stackBufBytes <= kMaxSafeStackReadBytes;
}

enum class ArgError {
  Ok = 0,
  BadPath,
  BadOffset,
  BadLength,
};

// Validate path + range args before any I/O.
// offset is 0-based; length must be in [1, kMaxLength].
inline ArgError validateArgs(const char* rel, long long offset, long long length) {
  if (!rel || rel[0] == '\0') return ArgError::BadPath;
  if (!M4xPathSafe::isSafePackageRelPath(rel)) return ArgError::BadPath;
  if (offset < 0) return ArgError::BadOffset;
  if (length < 1 || static_cast<unsigned long long>(length) > kMaxLength) {
    return ArgError::BadLength;
  }
  return ArgError::Ok;
}

inline const char* argErrorKey(ArgError e) {
  switch (e) {
    case ArgError::BadPath:
      return "bad_path";
    case ArgError::BadOffset:
      return "bad_offset";
    case ArgError::BadLength:
      return "bad_length";
    default:
      return "";
  }
}

// Compute how many bytes to read given file size and validated offset/length.
// Returns 0 when offset is at/ past EOF (short/empty read, not an error).
inline size_t clampReadLength(uint64_t fileSize, uint64_t offset, size_t length) {
  if (offset >= fileSize) return 0;
  const uint64_t remain = fileSize - offset;
  if (remain < static_cast<uint64_t>(length)) return static_cast<size_t>(remain);
  return length;
}

// In-memory range read for unit tests (never copies the whole source when
// only a window is requested — writes at most `length` bytes into out).
inline bool readRangeMem(const uint8_t* data, size_t dataLen, uint64_t offset, size_t length,
                         uint8_t* out, size_t* outLen) {
  if (!out || !outLen) return false;
  *outLen = 0;
  if (length == 0 || length > kMaxLength) return false;
  const size_t n = clampReadLength(static_cast<uint64_t>(dataLen), offset, length);
  if (n == 0) return true;
  if (offset > static_cast<uint64_t>(dataLen)) return true;
  std::memcpy(out, data + static_cast<size_t>(offset), n);
  *outLen = n;
  return true;
}

}  // namespace M4xFsRange
