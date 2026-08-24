#pragma once

#include "apps/M4xJsonStream.h"

#include <SDCardManager.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace M4NativeProviderIo {

bool ensureParentDirs(const std::string& absPath);

// Replace the last path extension (`toc_rows.txt` → `toc_rows.part`).
// Do not append `.tmp` onto an existing `.txt`: FatFS 8.3 aliases
// `toc_rows.txt.tmp` to `TOC_ROWS.TXT` and catalog commit then fails.
std::string replacedExtension(const std::string& path, const char* ext);
bool cacheComplete(const std::string& absPath, size_t* sizeOut = nullptr);
// Strong cache check for providers that must not trust legacy one-byte markers.
// New markers record the committed byte size; legacy markers return false.
bool cacheVerified(const std::string& absPath, size_t* sizeOut = nullptr);
bool removeIncomplete(const std::string& absPath);
// Remove all cache generations for one chapter. This is used when an older
// cache has a stale marker, partial file, or an otherwise uncommittable live
// file. It deliberately does not remove the parent directory.
bool clearCacheArtifacts(const std::string& absPath);

// Commit an already-closed temporary file to its final path. Rename is tried
// first for the normal atomic/cheap path. Some FAT/exFAT cards have shown
// successful streaming writes followed by a rename failure; in that case a
// verified streaming copy is used before the old generation is discarded.
// `expectedBytes` must be non-zero and is verified on the committed file.
bool commitTempFile(const std::string& tempAbsPath, const std::string& finalAbsPath,
                    size_t expectedBytes, bool preserveOld = true,
                    bool allowAlreadyFinal = true);

bool commitPart(const std::string& absPath, size_t* sizeOut = nullptr);

class PartFileSink final : public M4xJsonStream::Sink {
 public:
  PartFileSink() = default;
  ~PartFileSink() override { close(); }
  PartFileSink(const PartFileSink&) = delete;
  PartFileSink& operator=(const PartFileSink&) = delete;

  bool open(const std::string& finalAbsPath);
  bool write(const uint8_t* data, size_t len) override;
  bool flush();
  void close();
  size_t written() const { return written_; }
  const std::string& finalPath() const { return finalPath_; }
  const std::string& partPath() const { return partPath_; }

 private:
  bool flushBuffer();
  bool ensureFile();
  static constexpr size_t kBufferBytes = 8u * 1024u;
  FsFile file_;
  std::string finalPath_;
  std::string partPath_;
  uint8_t* buffer_ = nullptr;
  size_t used_ = 0;
  size_t written_ = 0;
  bool open_ = false;
  bool fileReady_ = false;
};

// Compatibility credential reader for existing plugin config.json files.
// Secrets stay in native memory and are never returned to XML or log output.
bool loadCookieHeader(const std::string& appDataRoot, const std::string& providerId,
                      std::string& cookieOut);
bool hasCredential(const std::string& appDataRoot, const std::string& providerId);

// Merge collected Set-Cookie lines back into the existing config.json cookie
// object. Used by native auth renewal/login adapters; values must never be logged.
bool mergeSetCookies(const std::string& appDataRoot, const std::string& providerId,
                     const std::vector<std::string>& setCookieLines);

// Store explicit credential values from a verified login response. This is a
// fallback for services that expose the session identifiers in JSON when the
// HTTP client cannot surface every repeated Set-Cookie line. Empty values are
// ignored; callers must never log this vector.
bool storeCookieValues(const std::string& appDataRoot,
                       const std::vector<std::pair<std::string, std::string>>& values);

}  // namespace M4NativeProviderIo
