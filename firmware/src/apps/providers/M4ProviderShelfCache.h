#pragma once

#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace M4ProviderShelfCache {

struct Schema {
  std::string providerId;
  uint32_t schemaVersion = 0;
  std::vector<std::string> columns;
  std::string fingerprint;
};

struct Metadata {
  uint32_t formatVersion = 0;
  std::string providerId;
  uint32_t schemaVersion = 0;
  std::string fingerprint;
};

inline uint64_t fnv1aAppend(uint64_t hash, const std::string& value) {
  for (unsigned char c : value) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  // Separators make ["ab", "c"] distinct from ["a", "bc"].
  hash ^= 0xFF;
  hash *= 1099511628211ULL;
  return hash;
}

inline std::string fingerprintFor(const std::string& providerId, uint32_t schemaVersion,
                                  const std::vector<std::string>& columns) {
  uint64_t hash = 1469598103934665603ULL;
  hash = fnv1aAppend(hash, providerId);
  hash = fnv1aAppend(hash, std::to_string(schemaVersion));
  for (const auto& column : columns) hash = fnv1aAppend(hash, column);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[static_cast<size_t>(i)] = kHex[hash & 0x0F];
    hash >>= 4;
  }
  return out;
}

inline Schema makeSchema(const char* providerId, std::initializer_list<const char*> columns) {
  Schema schema;
  schema.providerId = providerId ? providerId : "";
  // Version 2 is the cover-capable native shelf generation. The fingerprint
  // still includes every ordered column so a forgotten version bump is safe.
  schema.schemaVersion = 2;
  for (const char* column : columns) schema.columns.emplace_back(column ? column : "");
  schema.fingerprint = fingerprintFor(schema.providerId, schema.schemaVersion, schema.columns);
  return schema;
}

// The ordered vectors here are also consumed by discovery. Keep this as the
// only persisted-column declaration for native provider shelves.
inline const Schema* schema(const std::string& providerId) {
  static const Schema fanqie = makeSchema(
      "fanqie", {"book_id", "book_name", "author", "_m4_progress", "thumb_url"});
  static const Schema jjwxc = makeSchema(
      "jjwxc", {"novelId", "novelName", "authorName", "_m4_progress", "cover"});
  static const Schema weread =
      makeSchema("weread", {"bookId", "title", "author", "progress", "cover"});
  static const Schema legado = makeSchema(
      "legado", {"bookUrl", "name", "author", "totalChapterNum", "latestChapterTitle", "coverUrl"});
  if (providerId == fanqie.providerId) return &fanqie;
  if (providerId == jjwxc.providerId) return &jjwxc;
  if (providerId == weread.providerId) return &weread;
  if (providerId == legado.providerId) return &legado;
  return nullptr;
}

inline std::string replaceExtension(const std::string& path, const char* extension) {
  if (path.empty() || !extension || !extension[0]) return path;
  const size_t slash = path.find_last_of('/');
  const size_t dot = path.find_last_of('.');
  const size_t stemEnd = dot != std::string::npos && (slash == std::string::npos || dot > slash)
                             ? dot
                             : path.size();
  std::string out = path.substr(0, stemEnd);
  out.push_back('.');
  out += extension;
  return out;
}

inline std::string metadataPath(const std::string& rowsPath) {
  return replaceExtension(rowsPath, "meta");
}

inline std::string metadataTempPath(const std::string& rowsPath) {
  return replaceExtension(rowsPath, "mpt");
}

inline std::string rowsTempPath(const std::string& rowsPath) {
  return replaceExtension(rowsPath, "part");
}

inline std::string metadataJson(const std::string& providerId) {
  const Schema* expected = schema(providerId);
  if (!expected) return {};
  return std::string("{\"formatVersion\":1,\"providerId\":\"") + expected->providerId +
         "\",\"schemaVersion\":" + std::to_string(expected->schemaVersion) +
         ",\"fingerprint\":\"" + expected->fingerprint + "\"}\n";
}

namespace detail {

class JsonCursor {
 public:
  explicit JsonCursor(const std::string& input) : input_(input) {}

  void whitespace() {
    while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_]))) ++pos_;
  }

  bool token(char expected) {
    whitespace();
    if (pos_ >= input_.size() || input_[pos_] != expected) return false;
    ++pos_;
    return true;
  }

  bool string(std::string& out) {
    whitespace();
    if (pos_ >= input_.size() || input_[pos_] != '"') return false;
    ++pos_;
    out.clear();
    while (pos_ < input_.size()) {
      const unsigned char c = static_cast<unsigned char>(input_[pos_++]);
      if (c == '"') return true;
      // Generated metadata is ASCII-only. Reject escapes instead of having a
      // second partial JSON implementation that could accept malformed input.
      if (c == '\\' || c < 0x20) return false;
      out.push_back(static_cast<char>(c));
    }
    return false;
  }

  bool uint32(uint32_t& out) {
    whitespace();
    if (pos_ >= input_.size() || !std::isdigit(static_cast<unsigned char>(input_[pos_]))) return false;
    uint64_t value = 0;
    while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
      value = value * 10 + static_cast<unsigned>(input_[pos_] - '0');
      if (value > 0xFFFFFFFFULL) return false;
      ++pos_;
    }
    out = static_cast<uint32_t>(value);
    return true;
  }

  bool done() {
    whitespace();
    return pos_ == input_.size();
  }

 private:
  const std::string& input_;
  size_t pos_ = 0;
};

}  // namespace detail

inline bool parseMetadata(const std::string& raw, Metadata& out) {
  out = {};
  detail::JsonCursor cursor(raw);
  if (!cursor.token('{')) return false;
  bool formatSeen = false;
  bool providerSeen = false;
  bool schemaSeen = false;
  bool fingerprintSeen = false;
  cursor.whitespace();
  if (cursor.token('}')) return false;
  while (true) {
    std::string key;
    if (!cursor.string(key) || !cursor.token(':')) return false;
    if (key == "formatVersion") {
      if (formatSeen || !cursor.uint32(out.formatVersion)) return false;
      formatSeen = true;
    } else if (key == "providerId") {
      if (providerSeen || !cursor.string(out.providerId)) return false;
      providerSeen = true;
    } else if (key == "schemaVersion") {
      if (schemaSeen || !cursor.uint32(out.schemaVersion)) return false;
      schemaSeen = true;
    } else if (key == "fingerprint") {
      if (fingerprintSeen || !cursor.string(out.fingerprint)) return false;
      fingerprintSeen = true;
    } else {
      return false;
    }
    cursor.whitespace();
    if (cursor.token('}')) break;
    if (!cursor.token(',')) return false;
  }
  return formatSeen && providerSeen && schemaSeen && fingerprintSeen && cursor.done();
}

inline bool isFresh(const std::string& providerId, const std::string& rawMetadata) {
  const Schema* expected = schema(providerId);
  if (!expected) return false;
  Metadata actual;
  if (!parseMetadata(rawMetadata, actual)) return false;
  return actual.formatVersion == 1 && actual.providerId == expected->providerId &&
         actual.schemaVersion == expected->schemaVersion && actual.fingerprint == expected->fingerprint;
}

inline bool cacheNeedsRefresh(const std::string& providerId, bool rowsPresent,
                              const std::string& rawMetadata) {
  return !rowsPresent || !isFresh(providerId, rawMetadata);
}

inline bool shouldAutoDiscover(bool cacheNeedsRefresh, bool autoAttempted, bool busy) {
  return cacheNeedsRefresh && !autoAttempted && !busy;
}

}  // namespace M4ProviderShelfCache
