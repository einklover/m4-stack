#pragma once

#include <cstddef>
#include <cstdint>

namespace JpegImagePolicy {

inline constexpr uint32_t kMaxSourceDimension = 4096;
inline constexpr uint64_t kMaxSourcePixels = 8u * 1024u * 1024u;

inline bool validSourceDimensions(uint32_t width, uint32_t height) {
  return width > 0 && height > 0 && width <= kMaxSourceDimension &&
         height <= kMaxSourceDimension &&
         static_cast<uint64_t>(width) * height <= kMaxSourcePixels;
}

inline bool validDestinationDimensions(int width, int height) {
  return width > 0 && height > 0 && width <= static_cast<int>(kMaxSourceDimension) &&
         height <= static_cast<int>(kMaxSourceDimension);
}

inline bool mcuRowBufferBytes(uint32_t width, uint32_t mcuHeight, size_t& bytes) {
  bytes = 0;
  if (width == 0 || mcuHeight == 0 || width > kMaxSourceDimension || mcuHeight > 32) return false;
  const uint64_t n = static_cast<uint64_t>(width) * mcuHeight;
  if (n > 1024u * 1024u) return false;
  bytes = static_cast<size_t>(n);
  return true;
}

inline bool rgbaBufferBytes(int width, int height, size_t& bytes) {
  bytes = 0;
  if (!validDestinationDimensions(width, height)) return false;
  const uint64_t n = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4u;
  if (n > static_cast<uint64_t>(SIZE_MAX)) return false;
  bytes = static_cast<size_t>(n);
  return true;
}

}  // namespace JpegImagePolicy
