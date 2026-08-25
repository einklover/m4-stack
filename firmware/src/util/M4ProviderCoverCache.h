#pragma once

// Host-owned provider-cover acquisition. The provider supplies only a URL;
// this boundary owns bounded transfer, deterministic cache naming, and the
// conversion callback that produces Home-compatible BMP output.

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <functional>
#include <iterator>
#include <string>

#include "apps/providers/M4NovelProviderContract.h"

namespace M4ProviderCoverCache {

inline constexpr size_t kMaxDownloadBytes = 512u * 1024u;

enum class ImageFormat : uint8_t { Unknown = 0, Jpeg, Bmp, Png, Webp };

// Content sniffing is deliberately independent of URL spelling: CDNs often
// return extensionless or query-string URLs.
inline ImageFormat detectImageFormat(const uint8_t* bytes, size_t size) {
  if (!bytes) return ImageFormat::Unknown;
  if (size >= 2 && bytes[0] == 0xff && bytes[1] == 0xd8) return ImageFormat::Jpeg;
  if (size >= 2 && bytes[0] == 'B' && bytes[1] == 'M') return ImageFormat::Bmp;
  static constexpr uint8_t kPng[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  if (size >= sizeof(kPng) && std::equal(std::begin(kPng), std::end(kPng), bytes)) return ImageFormat::Png;
  if (size >= 12 && bytes[0] == 'R' && bytes[1] == 'I' && bytes[2] == 'F' && bytes[3] == 'F' &&
      bytes[8] == 'W' && bytes[9] == 'E' && bytes[10] == 'B' && bytes[11] == 'P') {
    return ImageFormat::Webp;
  }
  return ImageFormat::Unknown;
}

inline bool canConvertImageFormat(ImageFormat format) {
  return format == ImageFormat::Bmp || format == ImageFormat::Jpeg || format == ImageFormat::Png;
}

struct Request {
  std::string providerId;
  std::string bookId;
  std::string coverUrl;
  int width = 0;
  int height = 0;
};

struct Backend {
  std::function<bool(const std::string&)> exists;
  std::function<bool(const std::string&)> makeDirs;
  std::function<bool(const std::string& url, const std::string& path, size_t maxBytes)> fetch;
  std::function<bool(const std::string& sourcePath, const std::string& targetPath, int width, int height)> convert;
  std::function<void(const std::string&)> remove;
};

struct Result {
  std::string coverBmpPath;  // contains [WIDTH]/[HEIGHT] for HomeActivity
  bool cacheHit = false;
};

inline uint64_t cacheKey(const std::string& providerId, const std::string& bookId) {
  uint64_t h = 1469598103934665603ULL;
  for (const char c : providerId + "\n" + bookId) {
    h ^= static_cast<uint8_t>(c);
    h *= 1099511628211ULL;
  }
  return h;
}

inline std::string hexKey(uint64_t value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[static_cast<size_t>(i)] = kHex[value & 0xfu];
    value >>= 4;
  }
  return out;
}

inline std::string cacheDir(const std::string& providerId, const std::string& bookId) {
  return std::string("/.crosspoint/provider_covers/") + hexKey(cacheKey(providerId, bookId));
}

inline std::string bmpTemplatePath(const std::string& providerId, const std::string& bookId) {
  return cacheDir(providerId, bookId) + "/cover_[WIDTH]x[HEIGHT].bmp";
}

inline std::string sourcePath(const std::string& providerId, const std::string& bookId) {
  return cacheDir(providerId, bookId) + "/source.img";
}

inline std::string concreteBmpPath(const std::string& providerId, const std::string& bookId, int width, int height) {
  return cacheDir(providerId, bookId) + "/cover_" + std::to_string(width) + "x" + std::to_string(height) + ".bmp";
}

inline Result acquire(const Request& request, const Backend& backend) {
  Result out;
  if (request.providerId.empty() || request.bookId.empty() || request.coverUrl.empty() || request.width <= 0 ||
      request.height <= 0 || !backend.exists || !backend.makeDirs || !backend.fetch || !backend.convert) {
    return out;
  }
  out.coverBmpPath = bmpTemplatePath(request.providerId, request.bookId);
  const std::string target = concreteBmpPath(request.providerId, request.bookId, request.width, request.height);
  if (backend.exists(target)) {
    out.cacheHit = true;
    return out;
  }
  if (!backend.makeDirs(cacheDir(request.providerId, request.bookId))) {
    out.coverBmpPath.clear();
    return out;
  }
  const std::string source = sourcePath(request.providerId, request.bookId);
  if (!backend.exists(source) && !backend.fetch(request.coverUrl, source, kMaxDownloadBytes)) {
    out.coverBmpPath.clear();
    return out;
  }
  if (!backend.convert(source, target, request.width, request.height) || !backend.exists(target)) {
    if (backend.remove) backend.remove(source);
    if (backend.remove) backend.remove(target);
    out.coverBmpPath.clear();
    return out;
  }
  return out;
}

// Adapter for the exact provider model Track F consumes.
inline Request requestFor(const std::string& providerId, const std::string& bookId,
                          const M4NovelProvider::BookDetail& detail, int width, int height) {
  return {providerId, bookId, detail.coverUrl, width, height};
}

// Production adapter: streamed HTTP download + existing PNGdec/JPEG converters + SD.
Result acquireProviderCover(const Request& request);

}  // namespace M4ProviderCoverCache
