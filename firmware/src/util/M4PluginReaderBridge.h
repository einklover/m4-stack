#pragma once

// Sandboxed plugin → native text reader handoff (host-testable path policy).

#include "apps/M4xPathSafe.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace M4PluginReaderBridge {

inline constexpr size_t kMaxTitleLen = 128;
inline constexpr size_t kMaxProgressKeyLen = 160;
inline constexpr size_t kMaxIdLen = 64;

enum class OpenError : uint8_t {
  Ok = 0,
  BadPath,
  BadExt,
  PathEscape,
  EmptyPath,
  BadMeta,
  NotRegular,
  Missing,
};

inline const char* errorKey(OpenError e) {
  switch (e) {
    case OpenError::Ok:
      return "";
    case OpenError::BadPath:
      return "bad_path";
    case OpenError::BadExt:
      return "bad_ext";
    case OpenError::PathEscape:
      return "path_escape";
    case OpenError::EmptyPath:
      return "empty_path";
    case OpenError::BadMeta:
      return "bad_meta";
    case OpenError::NotRegular:
      return "not_regular";
    case OpenError::Missing:
      return "missing";
    default:
      return "error";
  }
}

// Relative path must be a safe package path ending in .txt (case-insensitive).
inline bool hasTxtExtension(const std::string& rel) {
  if (rel.size() < 4) return false;
  const char* e = rel.c_str() + rel.size() - 4;
  return (e[0] == '.' || e[0] == '.') &&
         (e[1] == 't' || e[1] == 'T') && (e[2] == 'x' || e[2] == 'X') && (e[3] == 't' || e[3] == 'T');
}

inline OpenError validateRelPath(const char* rel) {
  if (!rel || !rel[0]) return OpenError::EmptyPath;
  if (rel[0] == '/' || rel[0] == '\\') return OpenError::PathEscape;
  if (std::strstr(rel, "..") != nullptr) return OpenError::PathEscape;
  if (!M4xPathSafe::isSafePackageRelPath(rel)) return OpenError::BadPath;
  if (!hasTxtExtension(rel)) return OpenError::BadExt;
  return OpenError::Ok;
}

// Join data root + rel; reject if result does not stay under dataRoot + '/'.
inline OpenError resolveUnderDataRoot(const std::string& dataRoot, const char* rel, std::string& absOut) {
  const OpenError pe = validateRelPath(rel);
  if (pe != OpenError::Ok) return pe;
  if (dataRoot.empty()) return OpenError::BadPath;
  const std::string prefix = (dataRoot.back() == '/') ? dataRoot : (dataRoot + "/");
  absOut = prefix + rel;
  if (absOut.compare(0, prefix.size(), prefix) != 0) return OpenError::PathEscape;
  return OpenError::Ok;
}

struct OpenRequest {
  std::string relPath;
  std::string absPath;
  std::string title;
  std::string bookId;
  std::string chapterUid;
  std::string progressKey;
  std::string appId;
  uint32_t generation = 0;
  // Raw file byte to restore (0 = start). Validated against file size on open.
  uint64_t initialByteOffset = 0;
  bool hasInitialByteOffset = false;
  // Optional plugin book TOC (app-data relative toc.json) for system chapter list.
  std::string tocRelPath;
  std::string tocAbsPath;
  int chapterIndex = 0;  // 0-based index in toc.json (current chapter)
  // Optional ContentProvider id (e.g. "weread") — never a filesystem path.
  std::string providerId;
  // Early loader open: the chapter body is still streaming into the file.
  // The receiver shows a loading placeholder instead of paginating the
  // partial file; a second open (pendingComplete=false) arrives when the
  // body is complete.
  bool pendingComplete = false;
};

inline OpenError validateMeta(const char* title, const char* bookId, const char* chapterUid,
                              const char* progressKey) {
  auto lenOk = [](const char* s, size_t max) {
    if (!s) return true;
    return std::strlen(s) <= max;
  };
  if (!lenOk(title, kMaxTitleLen)) return OpenError::BadMeta;
  if (!lenOk(bookId, kMaxIdLen)) return OpenError::BadMeta;
  if (!lenOk(chapterUid, kMaxIdLen)) return OpenError::BadMeta;
  if (!lenOk(progressKey, kMaxProgressKeyLen)) return OpenError::BadMeta;
  return OpenError::Ok;
}

// Index file magic / version for atomic completed indexes.
inline constexpr uint32_t kTidxMagic = 0x57495458;  // "WITX" WeRead Index Txt
inline constexpr uint16_t kTidxVersion = 1;

struct TidxHeader {
  uint32_t magic = kTidxMagic;
  uint16_t version = kTidxVersion;
  uint16_t complete = 0;  // 1 = finished, 0 = partial (must not treat as final)
  uint32_t fileSize = 0;
  uint32_t pageCount = 0;
  uint32_t layoutFp = 0;  // font/margin fingerprint
};

inline bool headerLooksComplete(const TidxHeader& h, uint32_t expectSize, uint32_t expectLayout) {
  return h.magic == kTidxMagic && h.version == kTidxVersion && h.complete == 1 && h.fileSize == expectSize &&
         h.layoutFp == expectLayout && h.pageCount >= 1 && h.pageCount <= 65536;
}

}  // namespace M4PluginReaderBridge
