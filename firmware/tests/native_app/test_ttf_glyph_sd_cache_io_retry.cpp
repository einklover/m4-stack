// Host fault injection for the device rebuildIndex() path.
// Compile with -DESP32=1 and the shim include dir ahead of the firmware headers:
//   c++ -std=c++17 -DESP32=1 -I ttf_sd_cache_io_shim -I ../../lib/EpdFont \
//       test_ttf_glyph_sd_cache_io_retry.cpp
#include <cassert>
#include <cstdio>

#include <cstdlib>
#include <new>
static bool failNextAllocation = false;
void* operator new(size_t n) {
  if(failNextAllocation) { failNextAllocation=false; throw std::bad_alloc(); }
  if(void* p=std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
#include "TtfGlyphSdCache.cpp"

using namespace TtfGlyphCache;

static void plant(int count) {
  ioDelayMs = 0;
  fakeMillis = 0;
  disk.clear();
  writeU32(disk, kMagic);
  writeU16(disk, kVersion);
  writeU16(disk, 0);
  for (int i = 0; i < count; ++i) {
    Key k{123, 24, static_cast<uint32_t>(65 + i)};
    Glyph g;
    g.width = 8;
    g.height = 8;
    g.advanceX = 8;
    g.bitmap.assign(16, 0xff);
    assert(appendRecord(disk, k, g));
  }
  fileResetForTests();
  reads = 0;
  seeks = 0;
  failRead = -1;
  failSeek = -1;
}

int main() {
  Glyph hit;
  Glyph miss;

  // Record-header short read while fileSize still covers the record.
  // First lookup must fail and leave the index unbuilt; the retry must read again and hit.
  plant(3);
  failRead = 3;
  assert(!fileLookup(Key{123, 24, 67}, miss));
  const int readsAfterShort = reads;
  failRead = -1;
  assert(fileLookup(Key{123, 24, 67}, hit));
  assert(hit.bitmap.size() == 16);
  assert(reads > readsAfterShort);
  std::printf("PASS short-read retry reads %d -> %d bitmap %zu\n", readsAfterShort, reads, hit.bitmap.size());

  // seekSet failure on the first record. Same retry contract.
  plant(3);
  failSeek = 1;
  assert(!fileLookup(Key{123, 24, 67}, miss));
  const int seeksAfter = seeks;
  const int readsAfterSeek = reads;
  failSeek = -1;
  assert(fileLookup(Key{123, 24, 67}, hit));
  assert(hit.bitmap.size() == 16);
  assert(seeks > seeksAfter);
  assert(reads > readsAfterSeek);
  std::printf("PASS seek-fail retry seeks %d -> %d reads %d -> %d\n", seeksAfter, seeks, readsAfterSeek, reads);

  // A torn tail still stops as a complete prefix: the intact glyph is found,
  // and a later lookup of the missing tail does no extra index IO.
  plant(2);
  assert(disk.size() > 8);
  assert((disk.size() - 8) % 2 == 0);
  const size_t rec = (disk.size() - 8) / 2;
  disk.resize(8 + rec + 4);
  fileResetForTests();
  reads = 0;
  assert(fileLookup(Key{123, 24, 65}, hit));
  assert(hit.bitmap.size() == 16);
  const int stable = reads;
  assert(!fileLookup(Key{123, 24, 66}, miss));
  assert(reads == stable);
  std::printf("PASS torn-tail prefix kept reads=%d\n", reads);

  // No partial index hits or appends, bounded records, continuation survives I/O faults.
  plant(100);
  Glyph valid; valid.width=8; valid.height=8; valid.bitmap.assign(16, 0xab);
  const auto originalSize=disk.size();
  assert(!fileLookup(Key{123,24,65}, hit));
  assert(!gIndexed && gScanActive && gScanPos==8+48*(kRecordHeader+16));
  const auto continuation=gScanPos;
  failRead=reads+1;
  assert(!fileLookup(Key{123,24,65}, hit));
  assert(gScanPos==continuation);
  failRead=-1;
  assert(fileAppend(Key{123,24,999},valid)==AppendResult::TransientFail);
  assert(disk.size()==originalSize);
  assert(fileLookup(Key{123,24,164},hit));
  assert(gIndexed && hit.bitmap.size()==16);

  // Duplicate-heavy records consume budget even if there is only one key.
  plant(1);
  for(int i=0;i<100;++i) { valid.bitmap.assign(16,static_cast<uint8_t>(i)); assert(appendRecord(disk,Key{123,24,65},valid)); }
  fileResetForTests(); reads=0;
  assert(!fileLookup(Key{123,24,65},hit));
  assert(reads<=49);
  assert(!fileLookup(Key{123,24,65},hit));
  assert(fileLookup(Key{123,24,65},hit));
  assert(hit.bitmap[0]==99);

  // Capacity does not prevent later duplicate updates.
  plant(kMaxIndex);
  valid.bitmap.assign(16,0xac); assert(appendRecord(disk,Key{123,24,65},valid));
  fileResetForTests();
  int calls=0;
  while(!fileLookup(Key{123,24,65},hit) && ++calls<200) {}
  assert(gIndexed && hit.bitmap[0]==0xac);

  // Slow reads bound each scan slice; no restart from record zero.
  plant(20); ioDelayMs=11;
  assert(!fileLookup(Key{123,24,65},hit));
  assert(gScanActive && gScanPos>8 && gScanPos<disk.size());
  const auto slowPos=gScanPos;
  ioDelayMs=0;
  assert(fileLookup(Key{123,24,65},hit));
  assert(gScanPos>=slowPos);

  // Malformed cached geometry never reaches publishGlyph, without hiding valid later entries.
  plant(2); disk[8+10]=255; disk[8+11]=255;
  assert(!fileLookup(Key{123,24,65},hit));
  assert(fileLookup(Key{123,24,66},hit));
  assert(validGeometry(0,0,0)); assert(validGeometry(0,8,0));
  assert(!validGeometry(8,8,15)); assert(!validGeometry(8,8,17));
  assert(validGeometry(128,64,2048)); assert(!validGeometry(255,255,2048));
  assert(appendRecord(disk,Key{123,24,99},Glyph{})); // zero-area advance-only is valid
  plant(1);
  Glyph emptyHit;
  failNextAllocation=true;
  bool threw=false;
  try { (void)fileLookup(Key{123,24,65},emptyHit); } catch(const std::bad_alloc&) { threw=true; }
  assert(threw && openedFiles==0 && *gSdMutex.load()==0);
  assert(fileLookup(Key{123,24,65},emptyHit));
  failNextAllocation=true; threw=false;
  try { (void)fileAppend(Key{123,24,999},valid); } catch(const std::bad_alloc&) { threw=true; }
  assert(threw && openedFiles==0 && *gSdMutex.load()==0);
  assert(fileAppend(Key{123,24,999},valid)==AppendResult::Ok);
  assert(openedFiles==0);
  std::printf("PASS continuation, full index, duplicates, slow IO, geometry, allocation-failure file/lock release\n");
  return 0;
}
