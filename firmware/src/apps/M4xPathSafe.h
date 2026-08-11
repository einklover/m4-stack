#pragma once

// Pure path/size policy for .m4x packages (host-testable, no Arduino deps).

#include <cstddef>
#include <string>
#include <vector>

namespace M4xPathSafe {

// Reasonable caps for Murphy M4 RAM / SD install.
inline constexpr size_t kMaxRelPathLen = 128;
inline constexpr size_t kMaxManifestBytes = 16 * 1024;
inline constexpr size_t kMaxEntryBytes = 256 * 1024;
inline constexpr size_t kMaxFileBytes = 512 * 1024;
// Bounded window read for SD-backed chapter bodies (fs.readRange).
// Never used to raise the 512 KiB full-file read/write caps above.
inline constexpr size_t kMaxReadRangeBytes = 16 * 1024;
inline constexpr size_t kMaxTotalExtractBytes = 2 * 1024 * 1024;
inline constexpr size_t kMaxPackageFiles = 64;  // entry + icon + files[] (not counting manifest)
inline constexpr size_t kMaxIdLen = 64;

// Validate a package-relative path that will be written under /apps/<id>/.
// Returns empty string if safe; otherwise a short machine error key.
// Rejects: empty, absolute, backslash, control chars, empty/./.. segments,
// over-long paths, and characters outside a conservative allow-list.
inline std::string validatePackageRelPath(const std::string& rel) {
  if (rel.empty()) return "path_empty";
  if (rel.size() > kMaxRelPathLen) return "path_too_long";
  if (rel[0] == '/' || rel[0] == '\\') return "path_absolute";

  // No Windows separators or escapes.
  if (rel.find('\\') != std::string::npos) return "path_backslash";
  if (rel.find('\0') != std::string::npos) return "path_nul";

  // Reject ".." substring early (covers URL-ish tricks too).
  if (rel.find("..") != std::string::npos) return "path_dotdot";

  size_t segStart = 0;
  auto flushSeg = [&](size_t end) -> std::string {
    if (end < segStart) return "path_bad";
    const size_t len = end - segStart;
    if (len == 0) return "path_empty_segment";
    const std::string seg = rel.substr(segStart, len);
    if (seg == "." || seg == "..") return "path_dot_segment";
    for (unsigned char c : seg) {
      if (c < 0x20 || c == 0x7f) return "path_control";
      const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '_' || c == '-' || c == '.' || c == '+';
      if (!ok) return "path_bad_char";
    }
    return {};
  };

  for (size_t i = 0; i < rel.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(rel[i]);
    if (c == '/') {
      const std::string e = flushSeg(i);
      if (!e.empty()) return e;
      segStart = i + 1;
      continue;
    }
  }
  {
    const std::string e = flushSeg(rel.size());
    if (!e.empty()) return e;
  }

  // Trailing slash not allowed (must be a file path).
  if (!rel.empty() && rel.back() == '/') return "path_trailing_slash";

  return {};
}

inline bool isSafePackageRelPath(const std::string& rel) {
  return validatePackageRelPath(rel).empty();
}

// Per-entry size limit: Lua entry tighter; everything else uses kMaxFileBytes;
// manifest uses kMaxManifestBytes.
inline size_t maxBytesForEntry(const std::string& rel, const std::string& entryPath) {
  if (rel == "manifest.json") return kMaxManifestBytes;
  if (rel == entryPath) return kMaxEntryBytes;
  return kMaxFileBytes;
}

// Build deduplicated extract list: manifest.json + entry + optional icon + files[].
// Only these relative paths may be written by the installer.
struct ExtractList {
  bool ok = false;
  std::string error;
  std::vector<std::string> paths;  // order: manifest, entry, files..., icon
};

inline ExtractList makeExtractList(const std::string& entry, const std::string& icon,
                                   const std::vector<std::string>& files) {
  ExtractList out;
  auto add = [&](const std::string& p) -> bool {
    const std::string e = validatePackageRelPath(p);
    if (!e.empty()) {
      out.error = e + ":" + p;
      return false;
    }
    for (const auto& x : out.paths) {
      if (x == p) return true;  // dedupe
    }
    if (out.paths.size() >= kMaxPackageFiles + 2) {  // +manifest + slack
      out.error = "too_many_files";
      return false;
    }
    out.paths.push_back(p);
    return true;
  };

  if (!add("manifest.json")) return out;
  if (entry.empty()) {
    out.error = "missing_entry";
    return out;
  }
  if (!add(entry)) return out;
  for (const auto& f : files) {
    if (f.empty()) continue;
    if (!add(f)) return out;
  }
  if (!icon.empty()) {
    if (!add(icon)) return out;
  }
  out.ok = true;
  return out;
}

// Staging / rollback directory helpers (path strings only).
inline std::string stagingDirFor(const std::string& installPath) {
  return installPath + ".staging";
}
inline std::string backupDirFor(const std::string& installPath) {
  return installPath + ".bak";
}

}  // namespace M4xPathSafe
