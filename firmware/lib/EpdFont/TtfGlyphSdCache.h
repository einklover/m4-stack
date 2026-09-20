#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

// Host-testable lazy glyph cache: key is (family, sizePx, codepoint).
// PSRAM holds live bitmaps; this store is the SD-backed (or in-memory) copy.
// Evicting a PSRAM slot before flush is allowed — the glyph is simply gone.

namespace TtfGlyphCache {

struct Key {
  uint32_t familyHash = 0;
  uint16_t sizePx = 0;
  uint32_t cp = 0;
  bool operator<(const Key& o) const {
    if (familyHash != o.familyHash) return familyHash < o.familyHash;
    if (sizePx != o.sizePx) return sizePx < o.sizePx;
    return cp < o.cp;
  }
  bool operator==(const Key& o) const {
    return familyHash == o.familyHash && sizePx == o.sizePx && cp == o.cp;
  }
};

struct Glyph {
  uint8_t width = 0;
  uint8_t height = 0;
  uint8_t advanceX = 0;
  int16_t left = 0;
  int16_t top = 0;
  std::vector<uint8_t> bitmap;
};

inline uint32_t hashFamily(const char* s) {
  uint32_t h = 2166136261u;
  if (!s) return h;
  while (*s) {
    h ^= static_cast<uint8_t>(*s++);
    h *= 16777619u;
  }
  return h;
}

inline uint32_t hashBytes(const uint8_t* data, size_t len) {
  uint32_t h = 2166136261u;
  if (!data) return h;
  for (size_t i = 0; i < len; ++i) {
    h ^= data[i];
    h *= 16777619u;
  }
  return h;
}

// Mix a font-content fingerprint into a path hash so replacing the TTF file
// under the same path yields a different family key and stale glyphs stop
// matching. fp == 0 (unknown) leaves the base hash unchanged.
inline uint32_t mixFingerprint(uint32_t base, uint32_t fp) {
  if (!fp) return base;
  return base ^ (fp + 0x9e3779b9u + (base << 6) + (base >> 2));
}

// Fingerprint from file metadata: size + FAT modify date/time. Pure so host
// tests can cover the mixing rules; device code fills it from FsFile.
inline uint32_t fingerprintFromMeta(uint32_t sizeLo, uint32_t sizeHi,
                                    uint16_t mdate, uint16_t mtime, bool hasTime) {
  uint8_t raw[12];
  raw[0] = static_cast<uint8_t>(sizeLo);
  raw[1] = static_cast<uint8_t>(sizeLo >> 8);
  raw[2] = static_cast<uint8_t>(sizeLo >> 16);
  raw[3] = static_cast<uint8_t>(sizeLo >> 24);
  raw[4] = static_cast<uint8_t>(sizeHi);
  raw[5] = static_cast<uint8_t>(sizeHi >> 8);
  raw[6] = static_cast<uint8_t>(sizeHi >> 16);
  raw[7] = static_cast<uint8_t>(sizeHi >> 24);
  raw[8] = static_cast<uint8_t>(mdate);
  raw[9] = static_cast<uint8_t>(mdate >> 8);
  const uint16_t t = hasTime ? mtime : 0;
  raw[10] = static_cast<uint8_t>(t);
  raw[11] = static_cast<uint8_t>(t >> 8);
  return hashBytes(raw, sizeof(raw));
}

// Fingerprint from buffer content (embedded fonts): FNV over the first 512
// bytes plus the total length, so same-path buffers still differ.
inline uint32_t contentFingerprint(const uint8_t* data, size_t len) {
  const size_t n = len > 512 ? 512 : len;
  uint32_t h = hashBytes(data, n);
  h ^= static_cast<uint32_t>(len & 0xffffffffu);
  h *= 16777619u;
  h ^= static_cast<uint32_t>((static_cast<uint64_t>(len) >> 32) & 0xffffffffu);
  h *= 16777619u;
  return h;
}

constexpr uint32_t kMagic = 0x4D345447u;  // 'M4TG'
constexpr uint16_t kVersion = 1;
constexpr uint16_t kMaxBitmap = 2048;
// Record header bytes: familyHash(4) + sizePx(2) + cp(4) + w/h/adv(3) +
// left(2) + top(2) + bitmapLen(2).
constexpr size_t kRecordHeader = 19;

inline void writeU16(std::vector<uint8_t>& o, uint16_t v) {
  o.push_back(static_cast<uint8_t>(v));
  o.push_back(static_cast<uint8_t>(v >> 8));
}
inline void writeU32(std::vector<uint8_t>& o, uint32_t v) {
  o.push_back(static_cast<uint8_t>(v));
  o.push_back(static_cast<uint8_t>(v >> 8));
  o.push_back(static_cast<uint8_t>(v >> 16));
  o.push_back(static_cast<uint8_t>(v >> 24));
}
inline uint16_t readU16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}
inline uint32_t readU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Appends one record. Refuses (returns false, writes nothing) when the bitmap
// exceeds kMaxBitmap — callers must treat that as a permanent reject, never
// silently truncate, or readers would cache a torn glyph as complete.
inline bool appendRecord(std::vector<uint8_t>& o, const Key& k, const Glyph& g) {
  if (g.bitmap.size() > kMaxBitmap) return false;
  writeU32(o, k.familyHash);
  writeU16(o, k.sizePx);
  writeU32(o, k.cp);
  o.push_back(g.width);
  o.push_back(g.height);
  o.push_back(g.advanceX);
  const uint16_t left = static_cast<uint16_t>(g.left);
  const uint16_t top = static_cast<uint16_t>(g.top);
  writeU16(o, left);
  writeU16(o, top);
  const uint16_t n = static_cast<uint16_t>(g.bitmap.size());
  writeU16(o, n);
  o.insert(o.end(), g.bitmap.begin(), g.bitmap.end());
  return true;
}

inline bool parseRecord(const uint8_t* p, size_t avail, size_t& used, Key& k, Glyph& g) {
  used = 0;
  if (!p || avail < kRecordHeader) return false;
  k.familyHash = readU32(p);
  k.sizePx = readU16(p + 4);
  k.cp = readU32(p + 6);
  g.width = p[10];
  g.height = p[11];
  g.advanceX = p[12];
  g.left = static_cast<int16_t>(readU16(p + 13));
  g.top = static_cast<int16_t>(readU16(p + 15));
  const uint16_t n = readU16(p + 17);
  if (n > kMaxBitmap || avail < kRecordHeader + n) return false;
  g.bitmap.assign(p + kRecordHeader, p + kRecordHeader + n);
  used = kRecordHeader + n;
  return true;
}

// Full-file footprint of one glyph record (header + bitmap, no truncation).
inline size_t recordFootprint(const Glyph& g) { return kRecordHeader + g.bitmap.size(); }

// Pre-check before appending: the record must fit under the file cap,
// counting its own size (sz + rec > max refuses; exact fit is allowed).
inline bool canAppend(uint64_t fileSize, size_t recSize, uint64_t maxBytes) {
  if (recSize > maxBytes) return false;
  return fileSize + static_cast<uint64_t>(recSize) <= maxBytes;
}

class Store {
 public:
  bool get(const Key& k, Glyph& out) const {
    auto it = glyphs_.find(k);
    if (it == glyphs_.end()) return false;
    out = it->second;
    return true;
  }
  bool contains(const Key& k) const { return glyphs_.find(k) != glyphs_.end(); }

  // Insert or replace. New/updated keys are queued for SD flush.
  void put(const Key& k, const Glyph& g) {
    glyphs_[k] = g;
    for (const auto& u : unsynced_) {
      if (u == k) return;
    }
    unsynced_.push_back(k);
  }

  int unsyncedCount() const { return static_cast<int>(unsynced_.size()); }
  size_t recordCount() const { return glyphs_.size(); }

  bool takeUnsynced(Key& k, Glyph& g) {
    while (!unsynced_.empty()) {
      k = unsynced_.back();
      unsynced_.pop_back();
      auto it = glyphs_.find(k);
      if (it == glyphs_.end()) continue;
      g = it->second;
      return true;
    }
    return false;
  }

  void markFlushed(const Key& k) {
    for (size_t i = 0; i < unsynced_.size(); ++i) {
      if (unsynced_[i] == k) {
        unsynced_.erase(unsynced_.begin() + static_cast<std::ptrdiff_t>(i));
        return;
      }
    }
  }

  void drop(const Key& k) {
    glyphs_.erase(k);
    for (size_t i = 0; i < unsynced_.size();) {
      if (unsynced_[i] == k) unsynced_.erase(unsynced_.begin() + static_cast<std::ptrdiff_t>(i));
      else ++i;
    }
  }

  void clear() {
    glyphs_.clear();
    unsynced_.clear();
  }

  bool loadFromBytes(const uint8_t* data, size_t len) {
    clear();
    if (!data || len < 8) return false;
    if (readU32(data) != kMagic) return false;
    if (readU16(data + 4) != kVersion) return false;
    size_t off = 8;
    while (off < len) {
      Key k;
      Glyph g;
      size_t used = 0;
      if (!parseRecord(data + off, len - off, used, k, g)) break;
      glyphs_[k] = std::move(g);
      off += used;
    }
    return true;
  }

  std::vector<uint8_t> serializeAll() const {
    std::vector<uint8_t> o;
    writeU32(o, kMagic);
    writeU16(o, kVersion);
    writeU16(o, 0);
    // Oversize bitmaps are refused by appendRecord (same as the device path);
    // they are simply left out of the image.
    for (const auto& e : glyphs_) appendRecord(o, e.first, e.second);
    return o;
  }

 private:
  std::map<Key, Glyph> glyphs_;
  std::vector<Key> unsynced_;
};

// One indexed record: key + byte offset of the record in the file.
struct IndexEntry {
  Key key;
  uint32_t offset = 0;
};

// Host-testable mirror of the device streaming index scan. Rules (both must
// match or-device/host behavior diverges):
// - bad magic/version (or image < 8 bytes) -> false, no entries; the caller
//   must NOT treat the index as built (a later SD-ready/flush must retry).
// - per record: needs a full 19-byte header; n must be <= kMaxBitmap; and
//   off + header + n must fit in len. Any violation STOPS the scan (the
//   format has no resync marker); a torn tail is not an error.
// - duplicate keys keep the LAST offset.
// - scanning stops once maxEntries unique keys are collected.
inline bool buildIndexFromBytes(const uint8_t* data, size_t len,
                                size_t maxEntries, std::vector<IndexEntry>& out) {
  out.clear();
  if (!data || len < 8) return false;
  if (readU32(data) != kMagic) return false;
  if (readU16(data + 4) != kVersion) return false;
  size_t off = 8;
  while (off < len && out.size() < maxEntries) {
    if (len - off < kRecordHeader) break;  // torn tail
    const uint16_t n = readU16(data + off + 17);
    if (n > kMaxBitmap) break;  // corrupt length, cannot resync
    if (len - off < kRecordHeader + static_cast<size_t>(n)) break;  // torn bitmap
    Key k;
    k.familyHash = readU32(data + off);
    k.sizePx = readU16(data + off + 4);
    k.cp = readU32(data + off + 6);
    bool dup = false;
    for (auto& e : out) {
      if (e.key == k) {
        e.offset = static_cast<uint32_t>(off);
        dup = true;
        break;
      }
    }
    if (!dup) {
      IndexEntry e;
      e.key = k;
      e.offset = static_cast<uint32_t>(off);
      out.push_back(e);
    }
    off += kRecordHeader + n;
  }
  return true;
}

// fileAppend outcome. Callers must handle all four:
// - Ok / Duplicate: record is on disk; clear dirty.
// - TransientFail: SD not ready or I/O error; KEEP dirty and back off.
// - PermanentReject: index full, file full, or oversize bitmap; DROP the
//   glyph (clear dirty) and continue with later glyphs — never head-block.
enum class AppendResult : uint8_t {
  Ok = 0,
  Duplicate,
  TransientFail,
  PermanentReject,
};

// Device file I/O (TransientFail on host). Bitmaps are not kept in the file
// index — lookup reads the record from SD into the caller's Glyph.
bool fileLookup(const Key& k, Glyph& out);
AppendResult fileAppend(const Key& k, const Glyph& g);
void fileResetForTests();

}  // namespace TtfGlyphCache
