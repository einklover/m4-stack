#include "CffReader.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#endif

namespace ttf {
namespace {
uint16_t rd16be(const uint8_t* p) { return static_cast<uint16_t>((uint16_t(p[0]) << 8) | p[1]); }
int16_t rds16be(const uint8_t* p) { return static_cast<int16_t>(rd16be(p)); }
uint32_t rd32be(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint32_t tag(const char s[5]) {
  return (uint32_t(uint8_t(s[0])) << 24) | (uint32_t(uint8_t(s[1])) << 16) |
         (uint32_t(uint8_t(s[2])) << 8) | uint8_t(s[3]);
}
void* reallocPsramFirst(void* p, size_t n) {
#if defined(ARDUINO_ARCH_ESP32)
  void* q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!q) q = heap_caps_realloc(p, n, MALLOC_CAP_8BIT);
  return q;
#else
  return std::realloc(p, n);
#endif
}
void freeMem(void* p) {
  if (!p) return;
#if defined(ARDUINO_ARCH_ESP32)
  heap_caps_free(p);
#else
  std::free(p);
#endif
}
bool decodeDictNumber(const uint8_t* data, size_t len, size_t& pos, int32_t& out) {
  if (pos >= len) return false;
  const uint8_t b0 = data[pos++];
  if (b0 >= 32 && b0 <= 246) { out = int32_t(b0) - 139; return true; }
  if (b0 >= 247 && b0 <= 250) {
    if (pos >= len) return false;
    out = (int32_t(b0) - 247) * 256 + data[pos++] + 108;
    return true;
  }
  if (b0 >= 251 && b0 <= 254) {
    if (pos >= len) return false;
    out = -(int32_t(b0) - 251) * 256 - data[pos++] - 108;
    return true;
  }
  if (b0 == 28) {
    if (len - pos < 2) return false;
    out = rds16be(data + pos); pos += 2; return true;
  }
  if (b0 == 29) {
    if (len - pos < 4) return false;
    out = int32_t(rd32be(data + pos)); pos += 4; return true;
  }
  if (b0 == 30) {
    // DICT real. We only need exact integer values for offsets/counts; real
    // values are hint/FontMatrix operands. Preserve stack shape without
    // materializing a decimal string.
    while (pos < len) {
      const uint8_t b = data[pos++];
      if ((b >> 4) == 15 || (b & 15) == 15) { out = 0; return true; }
    }
  }
  return false;
}
}  // namespace

bool CffFont::readAt(uint32_t off, void* dst, uint32_t n) const {
  return stream_ && off <= fileSize_ && n <= fileSize_ - off && stream_->seek(off) && stream_->read(dst, n) == n;
}

bool CffFont::readOffset(uint32_t off, uint8_t n, uint32_t& v) const {
  v = 0;
  if (n < 1 || n > 4) return false;
  uint8_t b[4] = {};
  if (!readAt(off, b, n)) return false;
  for (uint8_t i = 0; i < n; ++i) v = (v << 8) | b[i];
  return true;
}

bool CffFont::parseIndex(uint32_t rel, IndexInfo& out, uint32_t* next) const {
  out = {};
  if (!cff_.valid() || rel > cff_.len || cff_.len - rel < 2) return false;
  const uint32_t abs = cff_.off + rel;
  uint8_t b[2];
  if (!readAt(abs, b, 2)) return false;
  out.count = rd16be(b);
  out.whole.off = abs;
  if (!out.count) {
    out.whole.len = 2;
    if (next) *next = rel + 2;
    return true;
  }
  uint8_t os = 0;
  if (!readAt(abs + 2, &os, 1) || os < 1 || os > 4) return false;
  const uint64_t offsetsBytes = uint64_t(out.count + 1u) * os;
  const uint64_t dataRel = uint64_t(rel) + 3u + offsetsBytes;
  if (dataRel > cff_.len) return false;
  uint32_t first = 0, last = 0;
  if (!readOffset(abs + 3, os, first) ||
      !readOffset(abs + 3 + out.count * os, os, last) || first != 1 || last < first) return false;
  const uint64_t end = dataRel + last - 1u;
  if (end > cff_.len || end > 0xffffffffu) return false;
  out.offSize = os;
  out.offsetsOff = abs + 3;
  out.dataOff = cff_.off + uint32_t(dataRel);
  out.whole.len = uint32_t(end - rel);
  if (next) *next = uint32_t(end);
  return true;
}

bool CffFont::parseIndex2(uint32_t rel, IndexInfo& out, uint32_t* next) const {
  out = {};
  if (!cff_.valid() || rel > cff_.len || cff_.len - rel < 4) return false;
  const uint32_t abs = cff_.off + rel;
  uint8_t b[4];
  if (!readAt(abs, b, 4)) return false;
  out.count = rd32be(b);
  out.whole.off = abs;
  if (!out.count) {
    out.whole.len = 4;
    if (next) *next = rel + 4;
    return true;
  }
  // A malformed 32-bit count must not turn into an unbounded offset walk.
  // CharStrings are ultimately constrained by maxp.numGlyphs; subr/FD indexes
  // above this safety ceiling are not practical on M4 and would be hostile to
  // both execution time and metadata arithmetic.
  if (out.count > 1024u * 1024u) return false;
  uint8_t os = 0;
  if (!readAt(abs + 4, &os, 1) || os < 1 || os > 4) return false;
  const uint64_t offsetsBytes = uint64_t(out.count + 1u) * os;
  const uint64_t dataRel = uint64_t(rel) + 5u + offsetsBytes;
  if (dataRel > cff_.len) return false;
  uint32_t first = 0, last = 0;
  if (!readOffset(abs + 5, os, first) ||
      !readOffset(abs + 5 + out.count * os, os, last) || first != 1 || last < first) return false;
  const uint64_t end = dataRel + last - 1u;
  if (end > cff_.len || end > 0xffffffffu) return false;
  out.offSize = os;
  out.offsetsOff = abs + 5;
  out.dataOff = cff_.off + uint32_t(dataRel);
  out.whole.len = uint32_t(end - rel);
  if (next) *next = uint32_t(end);
  return true;
}

bool CffFont::indexObject(const IndexInfo& idx, uint32_t item, Slice& o) const {
  o = {};
  if (item >= idx.count || !idx.count || !idx.offSize) return false;
  uint32_t a = 0, b = 0;
  const uint64_t entry = uint64_t(idx.offsetsOff) + uint64_t(item) * idx.offSize;
  if (entry > 0xffffffffu) return false;
  const uint32_t e = uint32_t(entry);
  if (!readOffset(e, idx.offSize, a) || !readOffset(e + idx.offSize, idx.offSize, b) || !a || b < a) return false;
  const uint64_t off = uint64_t(idx.dataOff) + a - 1u;
  const uint64_t len = uint64_t(b) - a;
  const uint64_t end = uint64_t(idx.whole.off) + idx.whole.len;
  if (off + len > end || off > 0xffffffffu || len > 0xffffffffu) return false;
  o = {uint32_t(off), uint32_t(len)};
  return true;
}

bool CffFont::indexObject(Slice idx, uint16_t item, Slice& o) const {
  if (!idx.valid() || idx.off < cff_.off || idx.off - cff_.off >= cff_.len) return false;
  IndexInfo p;
  const bool ok = cff2_ ? parseIndex2(idx.off - cff_.off, p) : parseIndex(idx.off - cff_.off, p);
  if (!ok || p.whole.len != idx.len) return false;
  return indexObject(p, item, o);
}

bool CffFont::ensureOperandScratch(uint32_t count) const {
  if (count <= operandScratchCap_) return true;
  if (!cff2_ || count > 513) return false;
  auto* p = static_cast<float*>(reallocPsramFirst(operandScratch_, size_t(count) * sizeof(float)));
  if (!p) { lastError_ = "CFF2 operand PSRAM scratch OOM"; return false; }
  operandScratch_ = p;
  operandScratchCap_ = count;
  return true;
}

bool CffFont::variationRegionCount(uint16_t vsIndex, uint16_t& count) const {
  count = 0;
  if (!cff2_ || !variationStoreRel_ || variationStoreRel_ >= cff_.len) return false;
  uint8_t lenBytes[2];
  if (!readAt(cff_.off + variationStoreRel_, lenBytes, 2)) return false;
  const uint16_t storeLen = rd16be(lenBytes);
  if (storeLen < 8 || uint32_t(variationStoreRel_) + 2u + storeLen > cff_.len) return false;
  const uint32_t base = cff_.off + variationStoreRel_ + 2u;
  uint8_t hdr[8];
  if (!readAt(base, hdr, sizeof(hdr)) || rd16be(hdr) != 1) return false;
  const uint16_t dataCount = rd16be(hdr + 6);
  if (vsIndex >= dataCount) return false;
  const uint32_t offsetsEnd = 8u + uint32_t(dataCount) * 4u;
  if (offsetsEnd > storeLen) return false;
  uint8_t offBytes[4];
  if (!readAt(base + 8u + uint32_t(vsIndex) * 4u, offBytes, 4)) return false;
  const uint32_t itemOff = rd32be(offBytes);
  if (itemOff > storeLen || storeLen - itemOff < 6) return false;
  uint8_t item[6];
  if (!readAt(base + itemOff, item, sizeof(item))) return false;
  // CFF2 uses ItemVariationData only for regionIndexes; delta sets live in
  // CharStrings/PrivateDICTs, so itemCount and wordDeltaCount must be zero.
  if (rd16be(item) != 0 || rd16be(item + 2) != 0) return false;
  const uint16_t k = rd16be(item + 4);
  if (uint64_t(itemOff) + 6u + uint64_t(k) * 2u > storeLen) return false;
  count = k;
  return true;
}

bool CffFont::parseTopDict(Slice dict) {
  if (!dict.valid() || dict.len > 64u * 1024u) {
    lastError_ = "CFF Top DICT invalid or too large";
    return false;
  }
  if (dict.len > type2ScratchCap_) {
    auto* p = static_cast<uint8_t*>(reallocPsramFirst(type2Scratch_, dict.len));
    if (!p) { lastError_ = "CFF Top DICT PSRAM allocation failed"; return false; }
    type2Scratch_ = p;
    type2ScratchCap_ = dict.len;
  }
  if (!readAt(dict.off, type2Scratch_, dict.len)) return false;
  const uint8_t* bytes = type2Scratch_;
  int32_t stack[48];
  size_t sp = 0, pos = 0;
  uint32_t charStringsRel = 0, privateSize = 0, privateRel = 0;
  bool haveCharStrings = false, haveFdArray = false, haveFdSelect = false;
  while (pos < dict.len) {
    const uint8_t x = bytes[pos];
    if (x >= 28) {
      int32_t v = 0;
      if (!decodeDictNumber(bytes, dict.len, pos, v) || sp >= 48) return false;
      stack[sp++] = v;
      continue;
    }
    ++pos;
    uint16_t op = x;
    if (x == 12) {
      if (pos >= dict.len) return false;
      op = uint16_t(0x0c00 | bytes[pos++]);
    }
    if (op == 17) {  // CharStringINDEXOffset / CFF1 CharStrings
      if (sp != 1 || stack[0] < 0) return false;
      charStringsRel = uint32_t(stack[0]);
      haveCharStrings = true;
    } else if (!cff2_ && op == 18) {  // CFF1 Private
      if (sp != 2 || stack[0] < 0 || stack[1] < 0) return false;
      privateSize = uint32_t(stack[0]);
      privateRel = uint32_t(stack[1]);
    } else if (!cff2_ && op == 0x0c1e) {  // CFF1 ROS
      if (sp != 3 || stack[0] < 0 || stack[1] < 0 || stack[2] < 0) return false;
      cidKeyed_ = true;
    } else if (op == 0x0c24) {  // FDArray / CFF2 FontDICTINDEXOffset
      if (sp != 1 || stack[0] < 0) return false;
      fdArrayRel_ = uint32_t(stack[0]);
      haveFdArray = true;
    } else if (op == 0x0c25) {  // FDSelect / CFF2 FontDICTSelectOffset
      if (sp != 1 || stack[0] < 0) return false;
      fdSelectRel_ = uint32_t(stack[0]);
      haveFdSelect = true;
    } else if (cff2_ && op == 24) {  // VariationStoreOffset
      if (sp != 1 || stack[0] <= 0) return false;
      variationStoreRel_ = uint32_t(stack[0]);
    }
    sp = 0;
  }
  if (!haveCharStrings || charStringsRel >= cff_.len) {
    lastError_ = "CFF Top DICT missing CharStrings";
    return false;
  }
  if (!cff2_ && privateSize) {
    if (privateRel > cff_.len || privateSize > cff_.len - privateRel) return false;
    privateDict_ = {cff_.off + privateRel, privateSize};
  }
  if (cff2_) {
    cidKeyed_ = true;  // CFF2 always uses FontDICTINDEX, even with one font dict.
    if (!haveFdArray || fdArrayRel_ >= cff_.len) {
      lastError_ = "CFF2 Top DICT missing FontDICTINDEX";
      return false;
    }
  } else if (cidKeyed_ && (!haveFdArray || !haveFdSelect || fdArrayRel_ >= cff_.len || fdSelectRel_ >= cff_.len)) {
    lastError_ = "CID CFF missing FDArray or FDSelect";
    return false;
  }
  const bool indexOk = cff2_ ? parseIndex2(charStringsRel, charStringsInfo_)
                             : parseIndex(charStringsRel, charStringsInfo_);
  if (!indexOk || !charStringsInfo_.count || charStringsInfo_.count > 65535u) {
    lastError_ = cff2_ ? "invalid CFF2 CharStringINDEX" : "invalid CFF CharStrings INDEX";
    return false;
  }
  charStrings_ = charStringsInfo_.whole;
  glyphCount_ = uint16_t(charStringsInfo_.count);
  if (cff2_ && variationStoreRel_) {
    uint16_t k = 0;
    if (!variationRegionCount(0, k)) {
      lastError_ = "invalid CFF2 VariationStore";
      return false;
    }
  }
  (void)haveFdSelect;
  return true;
}

bool CffFont::parsePrivateSubrs(Slice dict, IndexInfo& outInfo, Slice& outSlice,
                                uint16_t* defaultVsIndex) {
  outInfo = {};
  outSlice = {};
  if (defaultVsIndex) *defaultVsIndex = 0;
  if (!dict.valid()) return true;
  if (dict.len > 64u * 1024u) { lastError_ = "CFF Private DICT too large"; return false; }
  if (dict.len > type2ScratchCap_) {
    auto* p = static_cast<uint8_t*>(reallocPsramFirst(type2Scratch_, dict.len));
    if (!p) { lastError_ = "CFF Private DICT PSRAM allocation failed"; return false; }
    type2Scratch_ = p;
    type2ScratchCap_ = dict.len;
  }
  if (!readAt(dict.off, type2Scratch_, dict.len)) return false;
  const uint8_t* bytes = type2Scratch_;
  float localStack[48];
  float* stack = localStack;
  const uint32_t stackCap = cff2_ ? 513u : 48u;
  if (cff2_) {
    if (!ensureOperandScratch(stackCap)) return false;
    stack = operandScratch_;
  }
  size_t sp = 0, pos = 0;
  int32_t subrsRel = -1;
  uint16_t vsIndex = 0;
  while (pos < dict.len) {
    const uint8_t x = bytes[pos];
    if (x >= 28) {
      int32_t v = 0;
      if (!decodeDictNumber(bytes, dict.len, pos, v) || sp >= stackCap) return false;
      stack[sp++] = float(v);
      continue;
    }
    ++pos;
    uint16_t op = x;
    if (x == 12) {
      if (pos >= dict.len) return false;
      op = uint16_t(0x0c00 | bytes[pos++]);
    }
    if (op == 19) {  // Subrs, relative to Private DICT start
      if (sp != 1 || stack[0] < 0) return false;
      subrsRel = int32_t(std::lround(stack[0]));
      sp = 0;
      continue;
    }
    if (cff2_ && op == 22) {  // PrivateDICT vsindex
      if (sp != 1 || stack[0] < 0 || stack[0] > 65535.f) return false;
      vsIndex = uint16_t(std::lround(stack[0]));
      uint16_t k = 0;
      if (!variationRegionCount(vsIndex, k)) return false;
      sp = 0;
      continue;
    }
    if (cff2_ && op == 23) {  // PrivateDICT blend, default instance
      if (!sp) return false;
      const int32_t n = int32_t(std::lround(stack[sp - 1]));
      uint16_t k = 0;
      if (n < 0 || !variationRegionCount(vsIndex, k)) return false;
      const uint64_t involved = uint64_t(n) * (uint64_t(k) + 1u) + 1u;
      if (involved > sp) return false;
      const size_t base = sp - size_t(involved);
      // At normalized default coordinates all variation deltas contribute zero;
      // retain only the n default operands and drop n*k deltas plus n.
      sp = base + size_t(n);
      continue;
    }
    // We do not apply hinting in the M4 rasterizer, but all other PrivateDICT
    // operators still delimit their operand sequence.
    sp = 0;
  }
  if (defaultVsIndex) *defaultVsIndex = vsIndex;
  if (subrsRel < 0) return true;
  const uint64_t abs = uint64_t(dict.off) + uint32_t(subrsRel);
  if (abs < cff_.off || abs >= uint64_t(cff_.off) + cff_.len) {
    lastError_ = "CFF local Subrs outside table";
    return false;
  }
  const bool ok = cff2_ ? parseIndex2(uint32_t(abs - cff_.off), outInfo)
                        : parseIndex(uint32_t(abs - cff_.off), outInfo);
  if (!ok) {
    lastError_ = "invalid CFF local Subr INDEX";
    return false;
  }
  outSlice = outInfo.whole;
  return true;
}

bool CffFont::parseFontDict(Slice dict, FdInfo& out) {
  out = {};
  if (!dict.valid() || dict.len > 64u * 1024u) return false;
  if (dict.len > type2ScratchCap_) {
    auto* p = static_cast<uint8_t*>(reallocPsramFirst(type2Scratch_, dict.len));
    if (!p) return false;
    type2Scratch_ = p;
    type2ScratchCap_ = dict.len;
  }
  if (!readAt(dict.off, type2Scratch_, dict.len)) return false;
  const uint8_t* bytes = type2Scratch_;
  int32_t stack[48];
  size_t sp = 0, pos = 0;
  uint32_t privateSize = 0, privateRel = 0;
  bool havePrivate = false;
  while (pos < dict.len) {
    const uint8_t x = bytes[pos];
    if (x >= 28) {
      int32_t v = 0;
      if (!decodeDictNumber(bytes, dict.len, pos, v) || sp >= 48) return false;
      stack[sp++] = v;
      continue;
    }
    ++pos;
    uint16_t op = x;
    if (x == 12) {
      if (pos >= dict.len) return false;
      op = uint16_t(0x0c00 | bytes[pos++]);
    }
    if (op == 18) {
      if (sp != 2 || stack[0] < 0 || stack[1] < 0) return false;
      privateSize = uint32_t(stack[0]);
      privateRel = uint32_t(stack[1]);
      havePrivate = true;
    }
    sp = 0;
  }
  if (cff2_ && !havePrivate) return false;
  if (!privateSize) return !cff2_ || privateRel == 0;
  if (privateRel > cff_.len || privateSize > cff_.len - privateRel) return false;
  out.privateDict = {cff_.off + privateRel, privateSize};
  return parsePrivateSubrs(out.privateDict, out.localSubrsInfo, out.localSubrs,
                           cff2_ ? &out.defaultVsIndex : nullptr);
}

bool CffFont::initFdSelect() {
  fdSelectFormat_ = 0xff;
  fdSelectLen_ = 0;
  if (!fdCount_) return false;
  if (fdCount_ == 1 && fdSelectRel_ == 0) {
    freeMem(fdSelectData_);
    fdSelectData_ = nullptr;
    fdSelectCapacity_ = 0;
    fdSelectFormat_ = 0xfe;  // implicit single FontDICT
    return true;
  }
  if (!fdSelectRel_ || fdSelectRel_ >= cff_.len) return false;
  uint8_t hdr[5] = {};
  if (!readAt(cff_.off + fdSelectRel_, hdr, 1)) return false;

  if (hdr[0] == 0) {
    if (fdCount_ > 256) return false;
    const uint32_t len = 1u + glyphCount_;
    if (len > cff_.len - fdSelectRel_) return false;
    uint8_t block[64];
    uint32_t gid = 0;
    while (gid < glyphCount_) {
      const uint32_t take = std::min<uint32_t>(sizeof(block), uint32_t(glyphCount_) - gid);
      if (!readAt(cff_.off + fdSelectRel_ + 1u + gid, block, take)) return false;
      for (uint32_t i = 0; i < take; ++i) if (block[i] >= fdCount_) return false;
      gid += take;
    }
    freeMem(fdSelectData_);
    fdSelectData_ = nullptr;
    fdSelectCapacity_ = 0;
    fdSelectFormat_ = 0;
    fdSelectLen_ = len;
    return true;
  }

  if (hdr[0] == 3) {
    if (fdCount_ > 256 || cff_.len - fdSelectRel_ < 3 ||
        !readAt(cff_.off + fdSelectRel_, hdr, 3)) return false;
    const uint16_t ranges = rd16be(hdr + 1);
    if (!ranges) return false;
    const uint32_t len = 3u + uint32_t(ranges) * 3u + 2u;
    if (len > cff_.len - fdSelectRel_) return false;
    if (len > fdSelectCapacity_) {
      auto* p = static_cast<uint8_t*>(reallocPsramFirst(fdSelectData_, len));
      if (!p) { lastError_ = "CFF FDSelect PSRAM allocation failed"; return false; }
      fdSelectData_ = p;
      fdSelectCapacity_ = len;
    }
    if (!readAt(cff_.off + fdSelectRel_, fdSelectData_, len)) return false;
    fdSelectFormat_ = 3;
    fdSelectLen_ = len;
    const uint8_t* r = fdSelectData_ + 3;
    uint16_t prev = 0;
    for (uint16_t i = 0; i < ranges; ++i) {
      const uint16_t first = rd16be(r + uint32_t(i) * 3u);
      const uint8_t fd = r[uint32_t(i) * 3u + 2u];
      if ((i == 0 && first != 0) || (i && first <= prev) || fd >= fdCount_) return false;
      prev = first;
    }
    const uint16_t sentinel = rd16be(r + uint32_t(ranges) * 3u);
    if (sentinel != glyphCount_ || sentinel <= prev) return false;
    return true;
  }

  if (cff2_ && hdr[0] == 4) {
    if (cff_.len - fdSelectRel_ < 5 || !readAt(cff_.off + fdSelectRel_, hdr, 5)) return false;
    const uint32_t ranges = rd32be(hdr + 1);
    if (!ranges || ranges > glyphCount_) return false;
    const uint64_t len64 = 5u + uint64_t(ranges) * 6u + 4u;
    if (len64 > cff_.len - fdSelectRel_) return false;
    uint32_t prev = 0;
    for (uint32_t i = 0; i < ranges; ++i) {
      uint8_t rec[6];
      if (!readAt(cff_.off + fdSelectRel_ + 5u + i * 6u, rec, sizeof(rec))) return false;
      const uint32_t first = rd32be(rec);
      const uint16_t fd = rd16be(rec + 4);
      if ((i == 0 && first != 0) || (i && first <= prev) || fd >= fdCount_) return false;
      prev = first;
    }
    uint8_t sentinelBytes[4];
    if (!readAt(cff_.off + fdSelectRel_ + 5u + ranges * 6u, sentinelBytes, 4)) return false;
    const uint32_t sentinel = rd32be(sentinelBytes);
    if (sentinel != glyphCount_ || sentinel <= prev) return false;
    // Format 4 can approach hundreds of KB. Keep it entirely on SD and binary
    // search six-byte range records when a glyph is requested.
    freeMem(fdSelectData_);
    fdSelectData_ = nullptr;
    fdSelectCapacity_ = 0;
    fdSelectFormat_ = 4;
    fdSelectLen_ = uint32_t(len64);
    return true;
  }

  lastError_ = "unsupported CFF FDSelect format";
  return false;
}

bool CffFont::initCidData() {
  IndexInfo fdArray;
  const bool ok = cff2_ ? parseIndex2(fdArrayRel_, fdArray) : parseIndex(fdArrayRel_, fdArray);
  const uint32_t maxFd = cff2_ ? 4096u : 256u;
  if (!ok || !fdArray.count || fdArray.count > maxFd || fdArray.count > 65535u) {
    lastError_ = cff2_ ? "invalid CFF2 FontDICTINDEX" : "invalid CID CFF FDArray";
    return false;
  }
  const uint16_t count = uint16_t(fdArray.count);
  if (count > fdCapacity_) {
    auto* p = static_cast<FdInfo*>(reallocPsramFirst(fdInfos_, size_t(count) * sizeof(FdInfo)));
    if (!p) { lastError_ = "CFF FD metadata PSRAM allocation failed"; return false; }
    fdInfos_ = p;
    fdCapacity_ = count;
  }
  fdCount_ = count;
  for (uint16_t i = 0; i < count; ++i) {
    fdInfos_[i] = FdInfo{};
    Slice dict;
    if (!indexObject(fdArray, i, dict) || !parseFontDict(dict, fdInfos_[i])) {
      lastError_ = cff2_ ? "invalid CFF2 FontDICT" : "invalid CID CFF Font DICT";
      return false;
    }
  }
  if (!initFdSelect()) return false;
  return true;
}

bool CffFont::selectGlyphFd(uint16_t gid, uint16_t& fd) const {
  fd = 0;
  if (!cidKeyed_) return true;
  if (gid >= glyphCount_ || !fdCount_) return false;
  if (fdSelectFormat_ == 0xfe) return fdCount_ == 1;

  if (fdSelectFormat_ == 0) {
    uint8_t selector = 0;
    if (!readAt(cff_.off + fdSelectRel_ + 1u + gid, &selector, 1)) return false;
    fd = selector;
    return fd < fdCount_;
  }

  if (fdSelectFormat_ == 3 && fdSelectData_ && fdSelectLen_ >= 5) {
    const uint16_t ranges = rd16be(fdSelectData_ + 1);
    const uint8_t* r = fdSelectData_ + 3;
    uint16_t lo = 0, hi = ranges;
    while (lo < hi) {
      const uint16_t mid = uint16_t((uint32_t(lo) + hi) / 2u);
      if (rd16be(r + uint32_t(mid) * 3u) <= gid) lo = uint16_t(mid + 1u);
      else hi = mid;
    }
    if (!lo) return false;
    const uint16_t idx = uint16_t(lo - 1u);
    const uint16_t next = idx + 1u < ranges ? rd16be(r + uint32_t(idx + 1u) * 3u)
                                            : rd16be(r + uint32_t(ranges) * 3u);
    if (gid >= next) return false;
    fd = r[uint32_t(idx) * 3u + 2u];
    return fd < fdCount_;
  }

  if (fdSelectFormat_ == 4) {
    uint8_t h[5];
    if (!readAt(cff_.off + fdSelectRel_, h, sizeof(h))) return false;
    const uint32_t ranges = rd32be(h + 1);
    uint32_t lo = 0, hi = ranges;
    while (lo < hi) {
      const uint32_t mid = (lo + hi) / 2u;
      uint8_t rec[4];
      if (!readAt(cff_.off + fdSelectRel_ + 5u + mid * 6u, rec, 4)) return false;
      if (rd32be(rec) <= gid) lo = mid + 1u; else hi = mid;
    }
    if (!lo) return false;
    const uint32_t idx = lo - 1u;
    uint8_t rec[6];
    if (!readAt(cff_.off + fdSelectRel_ + 5u + idx * 6u, rec, sizeof(rec))) return false;
    uint32_t next = 0;
    if (idx + 1u < ranges) {
      uint8_t nextBytes[4];
      if (!readAt(cff_.off + fdSelectRel_ + 5u + (idx + 1u) * 6u, nextBytes, 4)) return false;
      next = rd32be(nextBytes);
    } else {
      uint8_t sentinel[4];
      if (!readAt(cff_.off + fdSelectRel_ + 5u + ranges * 6u, sentinel, 4)) return false;
      next = rd32be(sentinel);
    }
    if (gid >= next) return false;
    fd = rd16be(rec + 4);
    return fd < fdCount_;
  }
  return false;
}

bool CffFont::prepareGlyphLocalSubrs(uint16_t gid) const {
  if (!cidKeyed_) {
    activeDefaultVsIndex_ = 0;
    return true;
  }
  uint16_t fd = 0;
  if (!selectGlyphFd(gid, fd)) return false;
  localSubrsInfo_ = fdInfos_[fd].localSubrsInfo;
  localSubrs_ = fdInfos_[fd].localSubrs;
  activeDefaultVsIndex_ = fdInfos_[fd].defaultVsIndex;
  return true;
}

bool CffFont::initSfntMetrics(uint32_t face, uint16_t nt) {
  head_ = cmap_ = hhea_ = hmtx_ = maxp_ = {};
  for (uint16_t i = 0; i < nt; ++i) {
    uint8_t r[16];
    if (!readAt(face + 12 + uint32_t(i) * 16, r, 16)) return false;
    Table t{rd32be(r + 8), rd32be(r + 12), true};
    if (t.off > fileSize_ || t.len > fileSize_ - t.off) return false;
    const uint32_t k = rd32be(r);
    if (k == tag("head")) head_ = t;
    else if (k == tag("cmap")) cmap_ = t;
    else if (k == tag("hhea")) hhea_ = t;
    else if (k == tag("hmtx")) hmtx_ = t;
    else if (k == tag("maxp")) maxp_ = t;
  }
  if (!head_.present || !cmap_.present || !hhea_.present || !hmtx_.present || !maxp_.present) {
    lastError_ = "CFF OpenType missing head/cmap/hhea/hmtx/maxp";
    return false;
  }
  uint8_t h[54], hh[36], m[6];
  if (head_.len < 54 || hhea_.len < 36 || maxp_.len < 6 ||
      !readAt(head_.off, h, 54) || !readAt(hhea_.off, hh, 36) || !readAt(maxp_.off, m, 6)) return false;
  unitsPerEm_ = rd16be(h + 18);
  bboxYMax_ = rds16be(h + 42);
  ascender_ = rds16be(hh + 4);
  descender_ = rds16be(hh + 6);
  lineGap_ = rds16be(hh + 8);
  numHMetrics_ = rd16be(hh + 34);
  const uint16_t ng = rd16be(m + 4);
  if (!unitsPerEm_ || unitsPerEm_ > 16384 || !numHMetrics_ || ng != glyphCount_) {
    lastError_ = "CFF OpenType metrics inconsistent with CharStrings";
    return false;
  }
  return initCmap();
}

bool CffFont::initCmap() {
  cmapLen_ = 0;
  if (cmap_.len < 4) return false;
  uint8_t h[4];
  if (!readAt(cmap_.off, h, 4)) return false;
  const uint16_t n = rd16be(h + 2);
  if (!n || n > 256 || cmap_.len < 4 + uint32_t(n) * 8) return false;
  uint32_t best = 0;
  int score = -1;
  for (uint16_t i = 0; i < n; ++i) {
    uint8_t e[8];
    if (!readAt(cmap_.off + 4u + uint32_t(i) * 8u, e, 8)) return false;
    const uint32_t o = rd32be(e + 4);
    if (o + 2 > cmap_.len) continue;
    uint8_t f[2];
    if (!readAt(cmap_.off + o, f, 2)) continue;
    const uint16_t fmt = rd16be(f);
    if (fmt != 4 && fmt != 12) continue;
    int s = fmt == 12 ? 100 : 0;
    const uint16_t platform = rd16be(e), encoding = rd16be(e + 2);
    if (platform == 3) s += encoding == 10 ? 40 : 30;
    else if (platform == 0) s += 20;
    if (s > score) { score = s; best = o; }
  }
  if (score < 0) { lastError_ = "CFF OpenType has no cmap format 4/12"; return false; }
  const uint32_t available = cmap_.len - best;
  uint8_t prefix[8] = {};
  if (available < sizeof(prefix) || !readAt(cmap_.off + best, prefix, sizeof(prefix))) return false;
  const uint16_t fmt = rd16be(prefix);
  uint32_t exactLen = 0;
  if (fmt == 4) {
    exactLen = rd16be(prefix + 2);
    if (exactLen < 16) return false;
  } else if (fmt == 12) {
    exactLen = rd32be(prefix + 4);
    if (exactLen < 16) return false;
  } else return false;
  if (exactLen > available || exactLen > 2u * 1024u * 1024u) {
    lastError_ = "CFF cmap subtable length out of range";
    return false;
  }
  if (exactLen > cmapScratchCap_) {
    auto* p = static_cast<uint8_t*>(reallocPsramFirst(cmapData_, exactLen));
    if (!p) { lastError_ = "CFF cmap PSRAM allocation failed"; return false; }
    cmapData_ = p;
    cmapScratchCap_ = exactLen;
  }
  if (!readAt(cmap_.off + best, cmapData_, exactLen)) return false;
  cmapLen_ = exactLen;
  if (fmt == 4) {
    cmapIs12_ = false;
    cmapGroups_ = rd16be(cmapData_ + 6) / 2;
    if (!cmapGroups_ || exactLen < 16u + cmapGroups_ * 8u) return false;
  } else {
    cmapIs12_ = true;
    cmapGroups_ = rd32be(cmapData_ + 12);
    if (exactLen < 16u + uint64_t(cmapGroups_) * 12u) return false;
  }
  return true;
}

bool CffFont::findGlyph(uint32_t cp, uint16_t& gid) const {
  gid = 0;
  if (!ready_ || !cmapData_ || !cmapLen_) return false;
  const uint8_t* p = cmapData_;
  if (!cmapIs12_) {
    if (cp > 0xffff) return true;
    const uint32_t ss = cmapGroups_ * 2;
    const uint8_t* end = p + 14;
    const uint8_t* start = end + ss + 2;
    const uint8_t* delta = start + ss;
    const uint8_t* range = delta + ss;
    uint32_t lo = 0, hi = cmapGroups_;
    while (lo < hi) {
      const uint32_t m = (lo + hi) / 2;
      if (cp > rd16be(end + m * 2)) lo = m + 1; else hi = m;
    }
    if (lo >= cmapGroups_ || cp < rd16be(start + lo * 2)) return true;
    const int16_t de = rds16be(delta + lo * 2);
    const uint16_t ro = rd16be(range + lo * 2);
    if (!ro) { gid = uint16_t((int32_t(cp) + de) & 0xffff); return true; }
    const uint32_t off = uint32_t(range - p) + lo * 2 + ro + (cp - rd16be(start + lo * 2)) * 2;
    if (off + 2 > cmapLen_) return true;
    gid = rd16be(p + off);
    if (gid) gid = uint16_t(gid + de);
    return true;
  }
  const uint8_t* groups = p + 16;
  uint32_t lo = 0, hi = cmapGroups_;
  while (lo < hi) {
    const uint32_t m = (lo + hi) / 2;
    const uint8_t* x = groups + m * 12;
    if (cp > rd32be(x + 4)) lo = m + 1; else hi = m;
  }
  if (lo >= cmapGroups_) return true;
  const uint8_t* x = groups + lo * 12;
  const uint32_t st = rd32be(x);
  if (cp < st) return true;
  const uint32_t id = rd32be(x + 8) + (cp - st);
  if (id < glyphCount_) gid = uint16_t(id);
  return true;
}

bool CffFont::glyphHMetrics(uint16_t gid, int32_t& adv, int32_t& lsb) const {
  adv = lsb = 0;
  if (!ready_ || gid >= glyphCount_ || !numHMetrics_) return false;
  uint8_t b[4];
  if (gid < numHMetrics_) {
    if (!readAt(hmtx_.off + uint32_t(gid) * 4, b, 4)) return false;
    adv = rd16be(b); lsb = rds16be(b + 2); return true;
  }
  if (!readAt(hmtx_.off + uint32_t(numHMetrics_ - 1) * 4, b, 2)) return false;
  adv = rd16be(b);
  const uint32_t o = hmtx_.off + uint32_t(numHMetrics_) * 4 + uint32_t(gid - numHMetrics_) * 2;
  if (!readAt(o, b, 2)) return false;
  lsb = rds16be(b);
  return true;
}

void CffFont::fontVMetrics(int32_t& a, int32_t& d, int32_t& g) const {
  a = ascender_; d = descender_; g = lineGap_;
}

bool CffFont::init(TtfStream& stream, uint32_t face) {
  ready_ = false;
  lastError_ = "not initialized";
  stream_ = &stream;
  fileSize_ = stream.size();
  cff2_ = false;
  cff_ = {};
  charStringsInfo_ = globalSubrsInfo_ = {};
  localSubrsInfo_ = {};
  charStrings_ = globalSubrs_ = privateDict_ = {};
  localSubrs_ = {};
  glyphCount_ = unitsPerEm_ = numHMetrics_ = 0;
  cmapLen_ = 0;
  cidKeyed_ = false;
  fdArrayRel_ = fdSelectRel_ = variationStoreRel_ = 0;
  fdCount_ = 0;
  activeDefaultVsIndex_ = 0;
  fdSelectFormat_ = 0xff;
  fdSelectLen_ = 0;
  if (face > fileSize_ || fileSize_ - face < 12) return false;
  uint8_t h[12];
  if (!readAt(face, h, 12) || rd32be(h) != 0x4f54544f) {
    lastError_ = "not an OTTO OpenType face";
    return false;
  }
  const uint16_t nt = rd16be(h + 4);
  if (!nt || nt > 128 || uint64_t(face) + 12u + uint64_t(nt) * 16u > fileSize_) return false;
  Slice cff1, cff2;
  for (uint16_t i = 0; i < nt; ++i) {
    uint8_t r[16];
    if (!readAt(face + 12u + uint32_t(i) * 16u, r, 16)) return false;
    const uint32_t k = rd32be(r), o = rd32be(r + 8), l = rd32be(r + 12);
    if ((k == tag("CFF ") || k == tag("CFF2")) &&
        (o > fileSize_ || l > fileSize_ - o || l < 4)) return false;
    if (k == tag("CFF ")) cff1 = {o, l};
    else if (k == tag("CFF2")) cff2 = {o, l};
  }
  if (cff2.valid()) {
    cff2_ = true;
    cff_ = cff2;
    uint8_t ch[5];
    if (cff_.len < 5 || !readAt(cff_.off, ch, sizeof(ch)) || ch[0] != 2 || ch[1] != 0 || ch[2] < 5) {
      lastError_ = "invalid CFF2 header";
      return false;
    }
    const uint32_t headerSize = ch[2];
    const uint32_t topSize = rd16be(ch + 3);
    if (!topSize || headerSize > cff_.len || topSize > cff_.len - headerSize) {
      lastError_ = "invalid CFF2 TopDICT size";
      return false;
    }
    const Slice top{cff_.off + headerSize, topSize};
    const uint32_t globalRel = headerSize + topSize;
    if (!parseIndex2(globalRel, globalSubrsInfo_)) {
      lastError_ = "invalid CFF2 GlobalSubrINDEX";
      return false;
    }
    globalSubrs_ = globalSubrsInfo_.whole;
    if (!parseTopDict(top) || !initCidData() || !initSfntMetrics(face, nt)) return false;
  } else if (cff1.valid()) {
    cff_ = cff1;
    uint8_t ch[4];
    if (!readAt(cff_.off, ch, 4) || ch[0] != 1 || ch[2] < 4 || ch[2] > cff_.len || ch[3] < 1 || ch[3] > 4) return false;
    uint32_t rel = ch[2];
    IndexInfo names, top, strings;
    if (!parseIndex(rel, names, &rel) || !names.count ||
        !parseIndex(rel, top, &rel) || top.count != 1 ||
        !parseIndex(rel, strings, &rel) ||
        !parseIndex(rel, globalSubrsInfo_)) return false;
    globalSubrs_ = globalSubrsInfo_.whole;
    Slice td;
    if (!indexObject(top, 0, td) || !parseTopDict(td)) return false;
    if (cidKeyed_) {
      if (!initCidData()) return false;
    } else {
      if (!parsePrivateSubrs(privateDict_, localSubrsInfo_, localSubrs_)) return false;
    }
    if (!initSfntMetrics(face, nt)) return false;
  } else {
    lastError_ = "OTTO face has no CFF/CFF2 table";
    return false;
  }
  ready_ = true;
  lastError_ = "ok";
  return true;
}
}  // namespace ttf
