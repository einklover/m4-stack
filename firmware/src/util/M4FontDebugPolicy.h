#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace M4FontDebugPolicy {

struct Candidate {
  std::string filename;
  std::string displayName;
  std::string type;
  uint32_t sizeBytes = 0;
};

inline bool isCompleteRuntimeFont(uint32_t sizeBytes, const char* signature, const char* integrity) {
  if (sizeBytes == 0 || !signature || !integrity || std::string(integrity) != "tables_in_file") return false;
  const std::string sig(signature);
  return sig == "sfnt" || sig == "true" || sig == "OTTO" || sig == "ttcf";
}

inline bool isRuntimeFilename(const char* filename) {
  if (!filename || filename[0] == '.') return false;
  const std::string name(filename);
  if (name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
      name.find("..") != std::string::npos) {
    return false;
  }
  const auto dot = name.find_last_of('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 >= name.size()) return false;
  std::string ext = name.substr(dot + 1);
  for (char& c : ext) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return ext == "ttf" || ext == "ttc" || ext == "otf" || ext == "otc";
}

inline std::string validateBasename(const std::string& filename) {
  if (filename.empty()) return "empty_filename";
  if (filename[0] == '.') return "hidden_filename";
  if (filename.find('/') != std::string::npos || filename.find('\\') != std::string::npos ||
      filename.find("..") != std::string::npos) {
    return "path_traversal";
  }
  if (!isRuntimeFilename(filename.c_str())) return "unsupported_font";
  return {};
}

inline const Candidate* findCandidate(const Candidate* candidates, size_t count, const std::string& filename) {
  if (!candidates) return nullptr;
  for (size_t i = 0; i < count; ++i) {
    if (candidates[i].filename == filename) return &candidates[i];
  }
  return nullptr;
}

}  // namespace M4FontDebugPolicy
