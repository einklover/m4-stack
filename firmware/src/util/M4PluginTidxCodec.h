#pragma once

// Host-testable completed page-index (.tidx) codec + validation.
// Production I/O uses the same encode/decode/validate path.

#include "util/M4PluginReaderBridge.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace M4PluginTidxCodec {

inline constexpr uint32_t kMaxPages = 65536;

struct DecodeResult {
  bool ok = false;
  const char* error = "ok";
  M4PluginReaderBridge::TidxHeader header{};
  std::vector<uint32_t> offsets;
};

// layoutFp must match production TxtReaderActivity fingerprint.
inline uint32_t layoutFingerprint(int viewportWidth, int linesPerPage, int fontId, int screenMargin,
                                 int encodingType) {
  return static_cast<uint32_t>(viewportWidth) ^ (static_cast<uint32_t>(linesPerPage) << 8) ^
         (static_cast<uint32_t>(fontId) << 16) ^ (static_cast<uint32_t>(screenMargin) << 4) ^
         (static_cast<uint32_t>(encodingType) << 24);
}

inline bool validateOffsets(const std::vector<uint32_t>& offsets, uint32_t fileSize) {
  if (offsets.empty() || offsets.size() > kMaxPages) return false;
  if (offsets[0] != 0) return false;
  if (fileSize == 0) return offsets.size() == 1 && offsets[0] == 0;
  for (size_t i = 0; i < offsets.size(); ++i) {
    if (offsets[i] >= fileSize) return false;
    if (i > 0 && offsets[i] <= offsets[i - 1]) return false;
  }
  // Impossible density: more pages than bytes.
  if (offsets.size() > fileSize) return false;
  return true;
}

// Encode complete index into a contiguous buffer (header + offsets).
inline bool encodeComplete(uint32_t fileSize, uint32_t layoutFp, const std::vector<uint32_t>& offsets,
                           std::vector<uint8_t>& out) {
  if (!validateOffsets(offsets, fileSize)) return false;
  M4PluginReaderBridge::TidxHeader h;
  h.magic = M4PluginReaderBridge::kTidxMagic;
  h.version = M4PluginReaderBridge::kTidxVersion;
  h.complete = 1;
  h.fileSize = fileSize;
  h.pageCount = static_cast<uint32_t>(offsets.size());
  h.layoutFp = layoutFp;
  out.resize(sizeof(h) + offsets.size() * sizeof(uint32_t));
  std::memcpy(out.data(), &h, sizeof(h));
  if (!offsets.empty()) {
    std::memcpy(out.data() + sizeof(h), offsets.data(), offsets.size() * sizeof(uint32_t));
  }
  return true;
}

inline DecodeResult decode(const uint8_t* data, size_t n, uint32_t expectFileSize, uint32_t expectLayoutFp) {
  DecodeResult r;
  if (!data || n < sizeof(M4PluginReaderBridge::TidxHeader)) {
    r.error = "truncated_header";
    return r;
  }
  std::memcpy(&r.header, data, sizeof(r.header));
  if (r.header.magic != M4PluginReaderBridge::kTidxMagic) {
    r.error = "bad_magic";
    return r;
  }
  if (r.header.version != M4PluginReaderBridge::kTidxVersion) {
    r.error = "bad_version";
    return r;
  }
  if (r.header.complete != 1) {
    r.error = "incomplete";
    return r;
  }
  if (r.header.fileSize != expectFileSize) {
    r.error = "size_mismatch";
    return r;
  }
  if (r.header.layoutFp != expectLayoutFp) {
    r.error = "layout_mismatch";
    return r;
  }
  if (r.header.pageCount < 1 || r.header.pageCount > kMaxPages) {
    r.error = "bad_page_count";
    return r;
  }
  const size_t need = sizeof(r.header) + static_cast<size_t>(r.header.pageCount) * sizeof(uint32_t);
  if (n < need) {
    r.error = "truncated_body";
    return r;
  }
  // Reject trailing garbage larger than one padding byte (strict).
  if (n != need) {
    r.error = "trailing_data";
    return r;
  }
  r.offsets.resize(r.header.pageCount);
  std::memcpy(r.offsets.data(), data + sizeof(r.header), r.header.pageCount * sizeof(uint32_t));
  if (!validateOffsets(r.offsets, expectFileSize)) {
    r.error = "bad_offsets";
    r.offsets.clear();
    return r;
  }
  r.ok = true;
  r.error = "ok";
  return r;
}

// Binary search: largest page start <= byteOffset.
inline int pageForByteOffset(const std::vector<uint32_t>& offsets, size_t byteOffset) {
  if (offsets.empty()) return 0;
  int lo = 0, hi = static_cast<int>(offsets.size()) - 1, ans = 0;
  while (lo <= hi) {
    const int mid = lo + (hi - lo) / 2;
    if (offsets[static_cast<size_t>(mid)] <= byteOffset) {
      ans = mid;
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return ans;
}

// Atomic replace policy (host-testable decisions).
enum class ReplaceStep : uint8_t {
  WriteTmp,
  VerifyTmp,
  RenameTmpOverLive,  // preferred: rename without deleting live first
  CleanupBak,
  FailKeepLive,
};

// Returns ordered steps for a safe replace that never deletes the only valid index
// before the new one is ready.
inline std::vector<ReplaceStep> atomicReplacePlan() {
  return {ReplaceStep::WriteTmp, ReplaceStep::VerifyTmp, ReplaceStep::RenameTmpOverLive,
          ReplaceStep::CleanupBak};
}

}  // namespace M4PluginTidxCodec
