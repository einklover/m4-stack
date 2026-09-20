// Host test for TtfGlyphCache::Store (no ESP32 / SD).
//   /opt/homebrew/bin/g++-14 -std=c++17 -I firmware/lib/EpdFont \
//     firmware/tests/native_app/test_ttf_glyph_sd_cache.cpp \
//     -o /tmp/test_ttf_glyph_sd_cache && /tmp/test_ttf_glyph_sd_cache

#include "TtfGlyphSdCache.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

int main() {
  using namespace TtfGlyphCache;

  Store s;
  Key k14{hashFamily("Noto.ttf"), 14, 0x4E00};
  Key k18{hashFamily("Noto.ttf"), 18, 0x4E00};
  Key k14b{hashFamily("Noto.ttf"), 14, 0x4E01};
  assert(!(k14 == k18));

  Glyph g14;
  g14.width = 12;
  g14.height = 14;
  g14.advanceX = 13;
  g14.left = -1;
  g14.top = 12;
  g14.bitmap = {0xAA, 0x55, 0xFF};

  Glyph g18 = g14;
  g18.height = 18;
  g18.bitmap = {0x11, 0x22};

  s.put(k14, g14);
  s.put(k18, g18);
  assert(s.recordCount() == 2);
  assert(s.unsyncedCount() == 2);
  assert(s.contains(k14));
  assert(!s.contains(k14b));

  Glyph out;
  assert(s.get(k14, out));
  assert(out.height == 14);
  assert(out.bitmap.size() == 3 && out.bitmap[0] == 0xAA);
  assert(s.get(k18, out));
  assert(out.height == 18);

  const std::vector<uint8_t> blob = s.serializeAll();
  assert(blob.size() >= 8);
  assert(readU32(blob.data()) == kMagic);

  Store loaded;
  assert(loaded.loadFromBytes(blob.data(), blob.size()));
  assert(loaded.recordCount() == 2);
  assert(loaded.unsyncedCount() == 0);
  assert(loaded.get(k14, out) && out.height == 14);
  assert(loaded.get(k18, out) && out.height == 18);

  // Drop without flush: glyph is gone (lossy eviction).
  s.drop(k14);
  assert(!s.contains(k14));
  assert(s.contains(k18));
  assert(s.recordCount() == 1);

  Key uk;
  Glyph ug;
  int n = 0;
  while (s.takeUnsynced(uk, ug)) ++n;
  assert(n == 1);
  assert(uk == k18);

  Store empty;
  assert(!empty.loadFromBytes(nullptr, 0));
  std::vector<uint8_t> bad = blob;
  bad[0] ^= 0xFF;
  assert(!empty.loadFromBytes(bad.data(), bad.size()) || empty.recordCount() == 0);

  // ---- B5: fingerprint mixing ----
  {
    const uint32_t base = hashFamily("/FONT/Noto.ttf");
    assert(mixFingerprint(base, 0) == base);  // unknown fp keeps path hash
    const uint32_t fpA = fingerprintFromMeta(1000, 0, 0x1234, 0x5678, true);
    const uint32_t fpB = fingerprintFromMeta(1001, 0, 0x1234, 0x5678, true);  // size change
    const uint32_t fpC = fingerprintFromMeta(1000, 0, 0x1234, 0x5679, true);  // mtime change
    const uint32_t fpD = fingerprintFromMeta(1000, 0, 0x1234, 0x5679, false);  // no stamp
    assert(fpA != fpB && fpA != fpC);
    assert(fpD == fingerprintFromMeta(1000, 0, 0x1234, 0x0000, true));  // hasTime=false masks mtime
    assert(mixFingerprint(base, fpA) != base);
    assert(mixFingerprint(base, fpA) != mixFingerprint(base, fpB));  // replaced file => new key
    // Embedded same-path buffers still differ (old code shared "<flash>" key).
    uint8_t bufA[600];
    uint8_t bufB[600];
    for (int i = 0; i < 600; ++i) { bufA[i] = (uint8_t)i; bufB[i] = (uint8_t)(i + 1); }
    assert(contentFingerprint(bufA, 600) != contentFingerprint(bufB, 600));
    assert(contentFingerprint(bufA, 600) != contentFingerprint(bufA, 601));
    assert(contentFingerprint(bufA, 600) == contentFingerprint(bufA, 600));
  }

  // ---- B1/B3: index build rules (mirror of device streaming scan) ----
  {
    Key kA{hashFamily("F.ttf"), 16, 0x4E00};
    Key kB{hashFamily("F.ttf"), 16, 0x4E01};
    Glyph gA;
    gA.width = 8; gA.height = 8; gA.advanceX = 9; gA.left = 0; gA.top = 8;
    gA.bitmap = {0x01, 0x02, 0x03, 0x04};
    Glyph gB = gA;
    gB.bitmap = {0xAA, 0xBB};
    // Layout: [kA][kB][kA again] — duplicate must keep the LAST offset.
    std::vector<uint8_t> img;
    writeU32(img, kMagic); writeU16(img, kVersion); writeU16(img, 0);
    const size_t offA1 = img.size();
    assert(appendRecord(img, kA, gA));
    const size_t offB = img.size();
    assert(appendRecord(img, kB, gB));
    Glyph gA2 = gA;
    gA2.bitmap = {0xFF};
    const size_t offA2 = img.size();
    assert(appendRecord(img, kA, gA2));
    std::vector<IndexEntry> idx;
    assert(buildIndexFromBytes(img.data(), img.size(), 8192, idx));
    assert(idx.size() == 2);
    for (const auto& e : idx) {
      if (e.key == kA) assert(e.offset == offA2);  // keep-last
      else if (e.key == kB) assert(e.offset == offB);
      else assert(false);
    }
    assert(offA1 != offA2);

    // loadFromBytes (Store) also keeps the last duplicate.
    Store dup;
    assert(dup.loadFromBytes(img.data(), img.size()));
    assert(dup.recordCount() == 2);
    assert(dup.get(kA, out) && out.bitmap.size() == 1 && out.bitmap[0] == 0xFF);

    // Torn tail (last record bitmap cut by 1): index stops before it,
    // earlier complete records are kept.
    std::vector<uint8_t> torn = img;
    torn.pop_back();
    std::vector<IndexEntry> idxTorn;
    assert(buildIndexFromBytes(torn.data(), torn.size(), 8192, idxTorn));
    assert(idxTorn.size() == 2);
    assert(idxTorn[0].key == kA && idxTorn[0].offset == offA1);
    assert(idxTorn[1].key == kB && idxTorn[1].offset == offB);

    // Corrupt length (n > kMaxBitmap): scan stops, earlier entries kept.
    std::vector<uint8_t> corrupt = img;
    corrupt[offB + 17] = 0xFF;  // n low byte
    corrupt[offB + 18] = 0xFF;  // n high byte -> 65535
    std::vector<IndexEntry> idxCorrupt;
    assert(buildIndexFromBytes(corrupt.data(), corrupt.size(), 8192, idxCorrupt));
    assert(idxCorrupt.size() == 1 && idxCorrupt[0].key == kA && idxCorrupt[0].offset == offA1);

    // Bad magic/version -> false, no entries (caller must retry, not append blind).
    std::vector<uint8_t> badMagic = img;
    badMagic[0] ^= 0xFF;
    std::vector<IndexEntry> idxBad;
    assert(!buildIndexFromBytes(badMagic.data(), badMagic.size(), 8192, idxBad));
    assert(idxBad.empty());
    assert(!buildIndexFromBytes(img.data(), 7, 8192, idxBad));  // < 8 bytes

    // Index cap respected.
    std::vector<IndexEntry> idxCap;
    assert(buildIndexFromBytes(img.data(), img.size(), 1, idxCap));
    assert(idxCap.size() == 1);
  }

  // ---- B2/P2: append pre-checks, no silent truncation ----
  {
    assert(canAppend(100, 19, 4u * 1024u * 1024u));
    assert(canAppend(4u * 1024u * 1024u - 19, 19, 4u * 1024u * 1024u));  // exact fit ok
    assert(!canAppend(4u * 1024u * 1024u - 18, 19, 4u * 1024u * 1024u));  // sz+rec > max
    assert(!canAppend(0, 4u * 1024u * 1024u + 1, 4u * 1024u * 1024u));

    Key k{hashFamily("F.ttf"), 16, 1};
    Glyph big;
    big.bitmap.assign(kMaxBitmap + 1, 0x5A);
    std::vector<uint8_t> out2;
    assert(!appendRecord(out2, k, big));  // refused, never truncated
    assert(out2.empty());
    assert(recordFootprint(big) == 19 + kMaxBitmap + 1);

    // Oversize glyphs are left out of serializeAll (device refuses them too).
    Store s2;
    Glyph small;
    small.bitmap = {0x11};
    s2.put(k, big);
    Key k2{hashFamily("F.ttf"), 16, 2};
    s2.put(k2, small);
    Store reloaded;
    const std::vector<uint8_t> blob2 = s2.serializeAll();
    assert(reloaded.loadFromBytes(blob2.data(), blob2.size()));
    assert(reloaded.recordCount() == 1);
    assert(reloaded.contains(k2) && !reloaded.contains(k));
  }

  printf("test_ttf_glyph_sd_cache PASS\n");
  return 0;
}
