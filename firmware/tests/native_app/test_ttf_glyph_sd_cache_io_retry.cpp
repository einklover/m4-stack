// Host fault injection for the device rebuildIndex() path.
// Compile with -DESP32=1 and the shim include dir ahead of the firmware headers:
//   c++ -std=c++17 -DESP32=1 -I ttf_sd_cache_io_shim -I ../../lib/EpdFont \
//       test_ttf_glyph_sd_cache_io_retry.cpp
#include <cassert>
#include <cstdio>

#include "TtfGlyphSdCache.cpp"

using namespace TtfGlyphCache;

static void plant(int count) {
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
  return 0;
}
