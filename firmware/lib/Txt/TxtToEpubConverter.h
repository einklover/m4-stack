#pragma once

#include <functional>
#include <string>

// Converts a TXT file to a minimal EPUB (ZIP, STORE mode) and caches the result
// on the SD card. Subsequent opens reuse the cache.
//
// Cache location: {cacheDir}/txt_{hash}/book.epub
// Validity check: {cacheDir}/txt_{hash}/epub.meta  (stores TXT file size as uint32)
class TxtToEpubConverter {
 public:
  // Returns true if a valid cached EPUB already exists for the given TXT file.
  static bool isCacheValid(const std::string& txtPath, const std::string& cacheDir);

  // Returns the path of the cached EPUB for the given TXT file.
  static std::string getCachedEpubPath(const std::string& txtPath, const std::string& cacheDir);

  // Converts txtPath → EPUB and writes it to the cache directory.
  // progressCb(percent 0-100) is called during conversion (may be nullptr).
  // Returns true on success.
  static bool convert(const std::string& txtPath, const std::string& cacheDir,
                      const std::function<void(int)>& progressCb);
};
