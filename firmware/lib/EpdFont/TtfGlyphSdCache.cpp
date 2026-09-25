#include "TtfGlyphSdCache.h"

#include <M4MemoryManager.h>

#if defined(ESP32) && M4_SD_GLYPH_CACHE_ENABLED
#include <Arduino.h>
#include <SDCardManager.h>
#include <esp_heap_caps.h>
#include <vector>
#include <atomic>
#endif

namespace TtfGlyphCache {
namespace {

#if defined(ESP32) && M4_SD_GLYPH_CACHE_ENABLED
constexpr const char* kDir = "/apps_data/m4_font";
constexpr const char* kPath = "/apps_data/m4_font/ttf_glyphs.bin";
constexpr uint64_t kMaxFileBytes = 4u * 1024u * 1024u;
constexpr int kMaxIndex = 8192;

// All faces share this on-disk index. Face-local locks cannot protect it.
std::atomic<SemaphoreHandle_t> gSdMutex{nullptr};
class ScopedSdCacheLock {
 public:
  ScopedSdCacheLock();
  ~ScopedSdCacheLock() { if (held_) xSemaphoreGive(mutex_); }
  bool acquired() const { return held_; }
 private:
  SemaphoreHandle_t mutex_ = nullptr;
  bool held_ = false;
};


ScopedSdCacheLock::ScopedSdCacheLock() {
  mutex_ = gSdMutex.load(std::memory_order_acquire);
  if (!mutex_) {
    SemaphoreHandle_t created = xSemaphoreCreateMutex();
    if (!created) return;
    SemaphoreHandle_t expected = nullptr;
    if (gSdMutex.compare_exchange_strong(expected, created,
                                         std::memory_order_acq_rel)) mutex_ = created;
    else { vSemaphoreDelete(created); mutex_ = expected; }
  }
  held_ = mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(1200)) == pdTRUE;
}

struct DiskEnt {
  Key key;
  uint32_t offset = 0;
  bool valid = false;
};

// The persistent SD glyph index used to be std::vector-backed, which could pin
// ~100KB+ of the scarce internal heap and then linearly scan it on every
// lookup/rebuild. Keep both entries and the open-addressing hash table in
// PSRAM-only storage. If PSRAM is unavailable, disable the cache for this turn
// instead of stealing the contiguous internal heap needed by Wi-Fi/TLS.
constexpr size_t kHashSlots = 16384;  // 2x kMaxIndex, power of two.
DiskEnt* gIndex = nullptr;
int32_t* gHash = nullptr;             // 0=empty, -1=tombstone, n=index+1.
size_t gIndexSize = 0;
size_t gActiveCount = 0;

uint32_t indexHash(const Key& k) {
  uint32_t h = k.familyHash ^ (static_cast<uint32_t>(k.sizePx) * 0x9e3779b9u) ^
               (k.cp * 0x85ebca6bu);
  h ^= h >> 16;
  h *= 0x7feb352du;
  h ^= h >> 15;
  return h;
}

bool ensureIndexStorage() {
  if (gIndex && gHash) return true;
  auto* entries = static_cast<DiskEnt*>(
      M4Memory::allocTtf(sizeof(DiskEnt) * kMaxIndex));
  auto* hash = static_cast<int32_t*>(
      M4Memory::allocTtf(sizeof(int32_t) * kHashSlots));
  if (!entries || !hash) {
    if (entries) M4Memory::free(entries);
    if (hash) M4Memory::free(hash);
    Serial.printf("[TTF-GLYPH] PSRAM index alloc failed; cache disabled this turn\n");
    return false;
  }
  gIndex = entries;
  gHash = hash;
  gIndexSize = 0;
  gActiveCount = 0;
  memset(gHash, 0, sizeof(int32_t) * kHashSlots);
  return true;
}

void resetIndex() {
  gIndexSize = 0;
  gActiveCount = 0;
  if (gHash) memset(gHash, 0, sizeof(int32_t) * kHashSlots);
}

int findIndex(const Key& k) {
  if (!gHash || !gIndex) return -1;
  size_t slot = static_cast<size_t>(indexHash(k)) & (kHashSlots - 1);
  for (size_t probe = 0; probe < kHashSlots; ++probe) {
    const int32_t v = gHash[slot];
    if (v == 0) return -1;
    if (v > 0) {
      const size_t idx = static_cast<size_t>(v - 1);
      if (idx < gIndexSize && gIndex[idx].valid && gIndex[idx].key == k) return static_cast<int>(idx);
    }
    slot = (slot + 1) & (kHashSlots - 1);
  }
  return -1;
}

bool upsertIndex(const Key& k, uint32_t offset) {
  if (!ensureIndexStorage()) return false;
  size_t slot = static_cast<size_t>(indexHash(k)) & (kHashSlots - 1);
  size_t firstTombstone = kHashSlots;
  for (size_t probe = 0; probe < kHashSlots; ++probe) {
    const int32_t v = gHash[slot];
    if (v > 0) {
      const size_t idx = static_cast<size_t>(v - 1);
      if (idx < gIndexSize && gIndex[idx].valid && gIndex[idx].key == k) {
        gIndex[idx].offset = offset;  // duplicates keep the LAST record.
        return true;
      }
    } else if (v == -1) {
      if (firstTombstone == kHashSlots) firstTombstone = slot;
    } else {
      const size_t target = firstTombstone == kHashSlots ? slot : firstTombstone;
      size_t idx = gIndexSize;
      if (idx >= static_cast<size_t>(kMaxIndex)) {
        idx = 0;
        while (idx < gIndexSize && gIndex[idx].valid) ++idx;
        if (idx >= gIndexSize) return false;
      } else {
        ++gIndexSize;
      }
      gIndex[idx].key = k;
      gIndex[idx].offset = offset;
      gIndex[idx].valid = true;
      gHash[target] = static_cast<int32_t>(idx + 1);
      ++gActiveCount;
      return true;
    }
    slot = (slot + 1) & (kHashSlots - 1);
  }
  return false;
}

void dropIndex(int i) {
  if (i < 0 || !gIndex || !gHash || static_cast<size_t>(i) >= gIndexSize || !gIndex[i].valid) return;
  const Key k = gIndex[i].key;
  size_t slot = static_cast<size_t>(indexHash(k)) & (kHashSlots - 1);
  for (size_t probe = 0; probe < kHashSlots; ++probe) {
    const int32_t v = gHash[slot];
    if (v == 0) break;
    if (v == i + 1) {
      gHash[slot] = -1;
      break;
    }
    slot = (slot + 1) & (kHashSlots - 1);
  }
  gIndex[i].valid = false;
  if (gActiveCount) --gActiveCount;
}

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
  if (!ensureIndexStorage()) return RebuildResult::IoError;
  resetIndex();
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
  while (gActiveCount < static_cast<size_t>(kMaxIndex)) {
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
    if (!upsertIndex(k, pos)) break;
    pos += static_cast<uint32_t>(kRecordHeader + n);
  }
  f.close();
  gIndexed = true;
  return RebuildResult::Ok;
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
#if defined(ESP32) && M4_SD_GLYPH_CACHE_ENABLED
  ScopedSdCacheLock lock;
  if (!lock.acquired()) return false;
  if (!ensureIndexStorage()) return false;
  if (!gIndexed && rebuildIndex() == RebuildResult::IoError) return false;
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
#if defined(ESP32) && M4_SD_GLYPH_CACHE_ENABLED
  ScopedSdCacheLock lock;
  if (!lock.acquired()) return AppendResult::TransientFail;
  if (!SdMan.ready() || !ensureIndexStorage()) return AppendResult::TransientFail;
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
      resetIndex();
      gIndexed = true;
      Serial.printf("[TTF-GLYPH] cache reset: bad header, started fresh\n");
    }
    // NoFile: fall through to the create path below.
  }
  if (findIndex(k) >= 0) return AppendResult::Duplicate;
  if (gActiveCount >= static_cast<size_t>(kMaxIndex)) {
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
  if (!upsertIndex(k, static_cast<uint32_t>(sz))) {
    f.close();
    return AppendResult::TransientFail;
  }
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
#if defined(ESP32) && M4_SD_GLYPH_CACHE_ENABLED
  ScopedSdCacheLock lock;
  if (!lock.acquired()) return;
  resetIndex();
  gIndexed = false;
#endif
}

}  // namespace TtfGlyphCache
