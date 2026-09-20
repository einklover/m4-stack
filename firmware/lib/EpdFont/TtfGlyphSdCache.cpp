#include "TtfGlyphSdCache.h"

#if defined(ESP32)
#include <Arduino.h>
#include <SDCardManager.h>
#include <vector>
#endif

namespace TtfGlyphCache {
namespace {

#if defined(ESP32)
constexpr const char* kDir = "/apps_data/m4_font";
constexpr const char* kPath = "/apps_data/m4_font/ttf_glyphs.bin";
constexpr uint64_t kMaxFileBytes = 4u * 1024u * 1024u;
constexpr int kMaxIndex = 8192;

struct DiskEnt {
  Key key;
  uint32_t offset = 0;
};

std::vector<DiskEnt> gIndex;
// True only when gIndex reflects the file on SD. Stays false across
// SD-not-ready / I/O failures so a later flush retries the scan instead of
// appending blind duplicates of records it could not see (B1).
bool gIndexed = false;

// One-shot logs for permanent rejects (index/file full): the file only
// grows, so these conditions never clear within a boot.
bool sLoggedIndexFull = false;
bool sLoggedFileFull = false;
bool sLoggedOversize = false;

// Rebuild outcome. Only Ok (including an empty-but-valid file) marks the
// index built. NoFile means "nothing to index yet"; BadFile means the header
// is garbage (self-heal on next append); IoError means retry later.
enum class RebuildResult { Ok, NoFile, BadFile, IoError };

RebuildResult rebuildIndex() {
  gIndex.clear();
  gIndexed = false;
  if (!SdMan.ready()) return RebuildResult::IoError;
  if (!SdMan.exists(kPath)) return RebuildResult::NoFile;
  FsFile f;
  if (!SdMan.openFileForRead("TtfGlyphCache", kPath, f)) return RebuildResult::IoError;
  const uint64_t fileSize = f.fileSize();
  if (fileSize < 8) {
    // Fresh/empty file: valid index, zero entries.
    f.close();
    gIndexed = true;
    return RebuildResult::Ok;
  }
  uint8_t hdr[8];
  if (f.read(hdr, 8) != 8) {
    f.close();
    return RebuildResult::IoError;  // transient short read, not corruption
  }
  if (readU32(hdr) != kMagic || readU16(hdr + 4) != kVersion) {
    f.close();
    return RebuildResult::BadFile;
  }
  uint32_t pos = 8;
  uint8_t recHdr[kRecordHeader];
  // Mirrors buildIndexFromBytes: full-header + bounds validation per record,
  // stop on violation (no resync marker), duplicates keep the LAST offset,
  // stop adding once kMaxIndex unique keys are collected.
  while (gIndex.size() < static_cast<size_t>(kMaxIndex)) {
    if (static_cast<uint64_t>(pos) + kRecordHeader > fileSize) break;  // torn tail
    if (!f.seekSet(pos)) break;
    const int got = f.read(recHdr, sizeof(recHdr));
    if (got != static_cast<int>(sizeof(recHdr))) break;
    Key k;
    k.familyHash = readU32(recHdr);
    k.sizePx = readU16(recHdr + 4);
    k.cp = readU32(recHdr + 6);
    const uint16_t n = readU16(recHdr + 17);
    if (n > kMaxBitmap) break;  // corrupt length
    if (static_cast<uint64_t>(pos) + kRecordHeader + n > fileSize) break;  // torn bitmap
    bool dup = false;
    for (auto& e : gIndex) {
      if (e.key == k) {
        e.offset = pos;
        dup = true;
        break;
      }
    }
    if (!dup) {
      DiskEnt e;
      e.key = k;
      e.offset = pos;
      gIndex.push_back(e);
    }
    pos += static_cast<uint32_t>(kRecordHeader + n);
  }
  f.close();
  gIndexed = true;
  return RebuildResult::Ok;
}

int findIndex(const Key& k) {
  for (size_t i = 0; i < gIndex.size(); ++i) {
    if (gIndex[i].key == k) return static_cast<int>(i);
  }
  return -1;
}

void dropIndex(int i) {
  if (i >= 0 && static_cast<size_t>(i) < gIndex.size()) {
    gIndex.erase(gIndex.begin() + i);
  }
}

bool writeFileHeader(FsFile& f) {
  uint8_t hdr[8] = {};
  hdr[0] = 0x47;
  hdr[1] = 0x54;
  hdr[2] = 0x34;
  hdr[3] = 0x4D;  // little-endian 'M4TG'
  hdr[4] = static_cast<uint8_t>(kVersion);
  hdr[5] = 0;
  hdr[6] = 0;
  hdr[7] = 0;
  return f.write(hdr, 8) == 8;
}
#endif

}  // namespace

bool fileLookup(const Key& k, Glyph& out) {
#if defined(ESP32)
  if (!gIndexed) rebuildIndex();
  const int i = findIndex(k);
  if (i < 0) return false;
  FsFile f;
  if (!SdMan.openFileForRead("TtfGlyphCache", kPath, f)) return false;
  if (!f.seekSet(gIndex[static_cast<size_t>(i)].offset)) {
    f.close();
    dropIndex(i);
    return false;
  }
  uint8_t recHdr[kRecordHeader];
  if (f.read(recHdr, sizeof(recHdr)) != static_cast<int>(sizeof(recHdr))) {
    // Torn header: forget the entry so a later append can rewrite it (B3).
    f.close();
    dropIndex(i);
    return false;
  }
  Key onDisk;
  onDisk.familyHash = readU32(recHdr);
  onDisk.sizePx = readU16(recHdr + 4);
  onDisk.cp = readU32(recHdr + 6);
  if (!(onDisk == k)) {
    // Index/file drift (should not happen single-task): drop, never serve.
    f.close();
    dropIndex(i);
    return false;
  }
  const uint16_t n = readU16(recHdr + 17);
  if (n > kMaxBitmap) {
    f.close();
    dropIndex(i);
    return false;
  }
  out.width = recHdr[10];
  out.height = recHdr[11];
  out.advanceX = recHdr[12];
  out.left = static_cast<int16_t>(readU16(recHdr + 13));
  out.top = static_cast<int16_t>(readU16(recHdr + 15));
  out.bitmap.resize(n);
  if (n && f.read(out.bitmap.data(), n) != static_cast<int>(n)) {
    // Torn bitmap (power loss mid-append): drop the entry so the glyph can
    // be re-rastered and re-appended instead of missing forever (B3).
    f.close();
    out.bitmap.clear();
    dropIndex(i);
    return false;
  }
  f.close();
  return true;
#else
  (void)k;
  (void)out;
  return false;
#endif
}

AppendResult fileAppend(const Key& k, const Glyph& g) {
#if defined(ESP32)
  if (!SdMan.ready()) return AppendResult::TransientFail;
  if (g.bitmap.size() > kMaxBitmap) {
    if (!sLoggedOversize) {
      sLoggedOversize = true;
      Serial.printf("[TTF-GLYPH] cache reject: bitmap %u > %u, dropping glyph\n",
                    static_cast<unsigned>(g.bitmap.size()),
                    static_cast<unsigned>(kMaxBitmap));
    }
    return AppendResult::PermanentReject;
  }
  if (!gIndexed) {
    const RebuildResult r = rebuildIndex();
    if (r == RebuildResult::IoError) return AppendResult::TransientFail;
    if (r == RebuildResult::BadFile) {
      // Our own cache file has a garbage header: reset it (truncate + fresh
      // header) instead of appending after garbage forever. Cached glyphs
      // are regenerable by design (eviction may drop them anyway).
      FsFile rf = SdMan.open(kPath, O_RDWR | O_CREAT | O_TRUNC);
      if (!rf) return AppendResult::TransientFail;
      if (!writeFileHeader(rf)) {
        rf.close();
        return AppendResult::TransientFail;
      }
      rf.close();
      gIndex.clear();
      gIndexed = true;
      Serial.printf("[TTF-GLYPH] cache reset: bad header, started fresh\n");
    }
    // NoFile: fall through to the create path below.
  }
  if (findIndex(k) >= 0) return AppendResult::Duplicate;
  if (static_cast<int>(gIndex.size()) >= kMaxIndex) {
    if (!sLoggedIndexFull) {
      sLoggedIndexFull = true;
      Serial.printf("[TTF-GLYPH] cache index full (%d); new glyphs stay PSRAM-only\n", kMaxIndex);
    }
    return AppendResult::PermanentReject;
  }

  SdMan.ensureDirectoryExists(kDir);
  const bool exists = SdMan.exists(kPath);
  FsFile f;
  if (exists) {
    f = SdMan.open(kPath, O_RDWR);
  } else {
    f = SdMan.open(kPath, O_RDWR | O_CREAT);
  }
  if (!f) return AppendResult::TransientFail;
  uint64_t sz = f.fileSize();
  if (!exists || sz < 8) {
    // Fresh file, or a torn-empty file from a crashed create: (re)write the
    // header instead of appending a header-less record no scan could read.
    if (!f.seekSet(0)) {
      f.close();
      return AppendResult::TransientFail;
    }
    if (!writeFileHeader(f)) {
      f.close();
      return AppendResult::TransientFail;
    }
    sz = 8;
  }
  std::vector<uint8_t> rec;
  if (!appendRecord(rec, k, g)) {
    f.close();
    return AppendResult::PermanentReject;  // unreachable (checked above), be safe
  }
  if (!canAppend(sz, rec.size(), kMaxFileBytes)) {
    f.close();
    if (!sLoggedFileFull) {
      sLoggedFileFull = true;
      Serial.printf("[TTF-GLYPH] cache file full (%llu bytes); new glyphs stay PSRAM-only\n",
                    static_cast<unsigned long long>(kMaxFileBytes));
    }
    return AppendResult::PermanentReject;
  }
  if (!f.seekSet(static_cast<uint32_t>(sz))) {
    f.close();
    return AppendResult::TransientFail;
  }
  if (f.write(rec.data(), rec.size()) != static_cast<int>(rec.size())) {
    f.close();
    return AppendResult::TransientFail;  // torn tail handled by lookup/drop (B3)
  }
  if (!f.sync()) {
    // Best-effort durability: close() still flushes orderly state; only a
    // power cut inside this window loses the tail (then B3 drops it).
    Serial.printf("[TTF-GLYPH] cache sync failed, keeping record anyway\n");
  }
  DiskEnt e;
  e.key = k;
  e.offset = static_cast<uint32_t>(sz);
  gIndex.push_back(e);
  f.close();
  // The file only grows through this function, so after a successful append
  // the in-memory index is exact even when it started from NoFile.
  gIndexed = true;
  return AppendResult::Ok;
#else
  (void)k;
  (void)g;
  return AppendResult::TransientFail;
#endif
}

void fileResetForTests() {
#if defined(ESP32)
  gIndex.clear();
  gIndexed = false;
#endif
}

}  // namespace TtfGlyphCache
