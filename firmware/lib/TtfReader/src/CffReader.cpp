#include "CffReader.h"

#include <algorithm>
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
      !readOffset(abs + 3 + uint32_t(out.count) * os, os, last) || first != 1 || last < first) return false;
  const uint64_t end = dataRel + last - 1u;
  if (end > cff_.len || end > 0xffffffffu) return false;
  out.offSize = os;
  out.offsetsOff = abs + 3;
  out.dataOff = cff_.off + uint32_t(dataRel);
  out.whole.len = uint32_t(end - rel);
  if (next) *next = uint32_t(end);
  return true;
}

bool CffFont::indexObject(const IndexInfo& idx, uint16_t item, Slice& o) const {
  o = {};
  if (item >= idx.count || !idx.count || !idx.offSize) return false;
  uint32_t a = 0, b = 0;
  const uint32_t e = idx.offsetsOff + uint32_t(item) * idx.offSize;
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
  if (!parseIndex(idx.off - cff_.off, p) || p.whole.len != idx.len) return false;
  return indexObject(p, item, o);
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
    if (op == 17) {  // CharStrings
      if (sp != 1 || stack[0] < 0) return false;
      charStringsRel = uint32_t(stack[0]);
      haveCharStrings = true;
    } else if (op == 18) {  // Private
      if (sp != 2 || stack[0] < 0 || stack[1] < 0) return false;
      privateSize = uint32_t(stack[0]);
      privateRel = uint32_t(stack[1]);
    } else if (op == 0x0c1e) {  // ROS
      if (sp != 3 || stack[0] < 0 || stack[1] < 0 || stack[2] < 0) return false;
      cidKeyed_ = true;
    } else if (op == 0x0c24) {  // FDArray
      if (sp != 1 || stack[0] < 0) return false;
      fdArrayRel_ = uint32_t(stack[0]);
      haveFdArray = true;
    } else if (op == 0x0c25) {  // FDSelect
      if (sp != 1 || stack[0] < 0) return false;
      fdSelectRel_ = uint32_t(stack[0]);
      haveFdSelect = true;
    }
    sp = 0;
  }
  if (!haveCharStrings || charStringsRel >= cff_.len) {
    lastError_ = "CFF Top DICT missing CharStrings";
    return false;
  }
  if (privateSize) {
    if (privateRel > cff_.len || privateSize > cff_.len - privateRel) return false;
    privateDict_ = {cff_.off + privateRel, privateSize};
  }
  if (cidKeyed_ && (!haveFdArray || !haveFdSelect || fdArrayRel_ >= cff_.len || fdSelectRel_ >= cff_.len)) {
    lastError_ = "CID CFF missing FDArray or FDSelect";
    return false;
  }
  if (!parseIndex(charStringsRel, charStringsInfo_) || !charStringsInfo_.count) {
    lastError_ = "invalid CFF CharStrings INDEX";
    return false;
  }
  charStrings_ = charStringsInfo_.whole;
  glyphCount_ = charStringsInfo_.count;
  return true;
}

bool CffFont::parsePrivateSubrs(Slice dict, IndexInfo& outInfo, Slice& outSlice) {
  outInfo = {};
  outSlice = {};
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
  int32_t stack[48];
  size_t sp = 0, pos = 0;
  int32_t subrsRel = -1;
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
    if (op == 19) {  // Subrs, relative to Private DICT start
      if (sp != 1 || stack[0] < 0) return false;
      subrsRel = stack[0];
    }
    sp = 0;
  }
  if (subrsRel < 0) return true;
  const uint64_t abs = uint64_t(dict.off) + uint32_t(subrsRel);
  if (abs < cff_.off || abs >= uint64_t(cff_.off) + cff_.len) {
    lastError_ = "CFF local Subrs outside table";
    return false;
  }
  if (!parseIndex(uint32_t(abs - cff_.off), outInfo, nullptr)) {
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
    }
    sp = 0;
  }
  if (!privateSize) return true;
  if (privateRel > cff_.len || privateSize > cff_.len - privateRel) return false;
  out.privateDict = {cff_.off + privateRel, privateSize};
  return parsePrivateSubrs(out.privateDict, out.localSubrsInfo, out.localSubrs);
}

bool CffFont::initFdSelect() {
  fdSelectLen_ = 0;
  if (!fdCount_ || fdSelectRel_ >= cff_.len) return false;
  uint8_t hdr[3] = {};
  if (!readAt(cff_.off + fdSelectRel_, hdr, 1)) return false;
  uint32_t len = 0;
  if (hdr[0] == 0) {
    len = 1u + glyphCount_;
  } else if (hdr[0] == 3) {
    if (cff_.len - fdSelectRel_ < 3 || !readAt(cff_.off + fdSelectRel_, hdr, 3)) return false;
    const uint16_t ranges = rd16be(hdr + 1);
    if (!ranges) return false;
    len = 3u + uint32_t(ranges) * 3u + 2u;
  } else {
    lastError_ = "unsupported CFF FDSelect format";
    return false;
  }
  if (len > cff_.len - fdSelectRel_) return false;
  if (len > fdSelectCapacity_) {
    auto* p = static_cast<uint8_t*>(reallocPsramFirst(fdSelectData_, len));
    if (!p) { lastError_ = "CFF FDSelect PSRAM allocation failed"; return false; }
    fdSelectData_ = p;
    fdSelectCapacity_ = len;
  }
  if (!readAt(cff_.off + fdSelectRel_, fdSelectData_, len)) return false;
  fdSelectLen_ = len;
  if (fdSelectData_[0] == 0) {
    for (uint32_t gid = 0; gid < glyphCount_; ++gid) {
      if (fdSelectData_[1u + gid] >= fdCount_) return false;
    }
  } else {
    const uint16_t ranges = rd16be(fdSelectData_ + 1);
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
  }
  return true;
}

bool CffFont::initCidData() {
  IndexInfo fdArray;
  if (!parseIndex(fdArrayRel_, fdArray, nullptr) || !fdArray.count || fdArray.count > 256) {
    lastError_ = "invalid CID CFF FDArray";
    return false;
  }
  const uint16_t count = fdArray.count;
  if (count > fdCapacity_) {
    auto* p = static_cast<FdInfo*>(reallocPsramFirst(fdInfos_, size_t(count) * sizeof(FdInfo)));
    if (!p) { lastError_ = "CID CFF FD metadata PSRAM allocation failed"; return false; }
    fdInfos_ = p;
    fdCapacity_ = count;
  }
  fdCount_ = count;
  for (uint16_t i = 0; i < count; ++i) {
    fdInfos_[i] = FdInfo{};
    Slice dict;
    if (!indexObject(fdArray, i, dict) || !parseFontDict(dict, fdInfos_[i])) {
      lastError_ = "invalid CID CFF Font DICT";
      return false;
    }
  }
  if (!initFdSelect()) return false;
  return true;
}

bool CffFont::selectGlyphFd(uint16_t gid, uint16_t& fd) const {
  fd = 0;
  if (!cidKeyed_) return true;
  if (!fdSelectData_ || !fdSelectLen_ || gid >= glyphCount_) return false;
  if (fdSelectData_[0] == 0) {
    fd = fdSelectData_[1u + gid];
    return fd < fdCount_;
  }
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

bool CffFont::prepareGlyphLocalSubrs(uint16_t gid) const {
  if (!cidKeyed_) return true;
  uint16_t fd = 0;
  if (!selectGlyphFd(gid, fd)) return false;
  localSubrsInfo_ = fdInfos_[fd].localSubrsInfo;
  localSubrs_ = fdInfos_[fd].localSubrs;
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
  // Scan each 8-byte encoding record directly from SD. This is a one-time
  // font-open cost and avoids a 2KB internal-stack directory buffer.
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
  const uint32_t avail = cmap_.len - best;
  if (avail > 2u * 1024u * 1024u) { lastError_ = "CFF cmap exceeds safety limit"; return false; }
  if (avail > cmapScratchCap_) {
    auto* p = static_cast<uint8_t*>(reallocPsramFirst(cmapData_, avail));
    if (!p) { lastError_ = "CFF cmap PSRAM allocation failed"; return false; }
    cmapData_ = p;
    cmapScratchCap_ = avail;
  }
  if (!readAt(cmap_.off + best, cmapData_, avail)) return false;
  cmapLen_ = avail;
  const uint16_t fmt = rd16be(cmapData_);
  if (fmt == 4) {
    if (avail < 14) return false;
    cmapIs12_ = false;
    cmapGroups_ = rd16be(cmapData_ + 6) / 2;
    if (!cmapGroups_ || avail < 16u + cmapGroups_ * 8u) return false;
  } else {
    if (avail < 16) return false;
    cmapIs12_ = true;
    cmapGroups_ = rd32be(cmapData_ + 12);
    if (avail < 16u + uint64_t(cmapGroups_) * 12u) return false;
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
  cff_ = {};
  charStringsInfo_ = globalSubrsInfo_ = {};
  localSubrsInfo_ = {};
  charStrings_ = globalSubrs_ = privateDict_ = {};
  localSubrs_ = {};
  glyphCount_ = unitsPerEm_ = numHMetrics_ = 0;
  cmapLen_ = 0;
  cidKeyed_ = false;
  fdArrayRel_ = fdSelectRel_ = 0;
  fdCount_ = 0;
  fdSelectLen_ = 0;
  if (face > fileSize_ || fileSize_ - face < 12) return false;
  uint8_t h[12];
  if (!readAt(face, h, 12) || rd32be(h) != 0x4f54544f) {
    lastError_ = "not an OTTO OpenType face";
    return false;
  }
  const uint16_t nt = rd16be(h + 4);
  if (!nt || nt > 128 || uint64_t(face) + 12u + uint64_t(nt) * 16u > fileSize_) return false;
  bool sawCff2 = false;
  for (uint16_t i = 0; i < nt; ++i) {
    uint8_t r[16];
    if (!readAt(face + 12u + uint32_t(i) * 16u, r, 16)) return false;
    const uint32_t k = rd32be(r), o = rd32be(r + 8), l = rd32be(r + 12);
    if (k == tag("CFF2")) sawCff2 = true;
    if (k == tag("CFF ")) {
      if (o > fileSize_ || l > fileSize_ - o || l < 4) return false;
      cff_ = {o, l};
    }
  }
  if (!cff_.valid()) {
    lastError_ = sawCff2 ? "CFF2 face is not supported yet" : "OTTO face has no CFF table";
    return false;
  }
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
  ready_ = true;
  lastError_ = "ok";
  return true;
}
}  // namespace ttf
