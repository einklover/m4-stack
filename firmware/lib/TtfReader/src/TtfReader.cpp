// Streamed TrueType parser + 2-bit rasterizer. See TtfReader.h.
// Parser layout follows the OpenType spec; rasterizer mirrors stb_truetype's
// bitmap conventions so host tests can cross-check against stb.

#include "TtfReader.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#endif

namespace ttf {

namespace {

constexpr uint32_t kTagHead = 0x68656164;  // 'head'
constexpr uint32_t kTagMaxp = 0x6D617870;  // 'maxp'
constexpr uint32_t kTagLoca = 0x6C6F6361;  // 'loca'
constexpr uint32_t kTagCmap = 0x636D6170;  // 'cmap'
constexpr uint32_t kTagHhea = 0x68686561;  // 'hhea'
constexpr uint32_t kTagHmtx = 0x686D7478;  // 'hmtx'
constexpr uint32_t kTagGlyf = 0x676C7966;  // 'glyf'
constexpr uint32_t kTagKern = 0x6B65726E;  // 'kern'
constexpr uint32_t kTagGvar = 0x67766172;  // 'gvar' -> optional variation deltas; default instance ignores them
constexpr uint32_t kTagFvar = 0x66766172;  // 'fvar' -> optional variation axes; default instance ignores them

uint16_t rd16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
int16_t rds16(const uint8_t* p) { return (int16_t)rd16(p); }
uint32_t rd32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

Pt applyXform(const Xform& m, float x, float y, bool on) {
  return {m.a * x + m.c * y + m.tx, m.b * x + m.d * y + m.ty, on};
}

// PSRAM-first ONLY for the font's persistent cmap table: a CJK TTF keeps a
// tens-of-KB cmap subtable resident, which on internal RAM permanently
// starves mbedTLS handshakes (jjwxc "list too large; back to shelf").
// Rasterizer scratch stays on internal RAM — it is touched per-pixel on the
// render hot path and PSRAM cache access measurably slowed page rendering.
void* ttfAllocPsram(size_t n) {
  if (n == 0) return nullptr;
#if defined(ARDUINO_ARCH_ESP32)
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
  return p;
#else
  return std::malloc(n);
#endif
}

void* ttfAlloc(size_t n) {
  if (n == 0) return nullptr;
#if defined(ARDUINO_ARCH_ESP32)
  return heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
  return std::malloc(n);
#endif
}

void ttfFree(void* p) {
  if (!p) return;
#if defined(ARDUINO_ARCH_ESP32)
  heap_caps_free(p);
#else
  std::free(p);
#endif
}

void* ttfRealloc(void* ptr, size_t n) {
  if (n == 0) {
    ttfFree(ptr);
    return nullptr;
  }
#if defined(ARDUINO_ARCH_ESP32)
  return heap_caps_realloc(ptr, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
  return std::realloc(ptr, n);
#endif
}

// PSRAM-first realloc for the font-file slice and packed-bitmap scratch: these
// are high-water buffers that grow to the largest rendered glyph and stay
// resident. glyf slice is read+parsed once per glyph (not per-pixel hot), and
// packed is the final 2-bit output — both are fine on PSRAM and it keeps them
// out of the ~320KB internal heap (which must host mbedTLS/WiFi/reader).
void* ttfReallocPsram(void* ptr, size_t n) {
  if (n == 0) {
    ttfFree(ptr);
    return nullptr;
  }
#if defined(ARDUINO_ARCH_ESP32)
  // heap_caps_realloc migrates the block if caps differ.
  void* p = heap_caps_realloc(ptr, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_realloc(ptr, n, MALLOC_CAP_8BIT);
  return p;
#else
  return std::realloc(ptr, n);
#endif
}

}  // namespace

uint32_t TtfFont::tagKey(const char tag[4]) {
  return ((uint32_t)(uint8_t)tag[0] << 24) | ((uint32_t)(uint8_t)tag[1] << 16) | ((uint32_t)(uint8_t)tag[2] << 8) |
         (uint32_t)(uint8_t)tag[3];
}

TtfFont::TtfFont() = default;

TtfFont::~TtfFont() {
  ttfFree(cmapData_);
  ttfFree(glyfScratch_);
  ttfFree(covScratch_);
  ttfFree(packedScratch_);
}

bool TtfFont::readAt(uint32_t off, void* dst, uint32_t n) const {
  if (!s_ || off > fileSize_ || n > fileSize_ - off) return false;
  if (!s_->seek(off)) return false;
  return s_->read(dst, n) == n;
}

TtfFont::Table TtfFont::findTable(uint32_t key) const {
  const Table* t = nullptr;
  switch (key) {
    case kTagHead: t = &head_; break;
    case kTagMaxp: t = &maxp_; break;
    case kTagLoca: t = &loca_; break;
    case kTagCmap: t = &cmap_; break;
    case kTagHhea: t = &hhea_; break;
    case kTagHmtx: t = &hmtx_; break;
    case kTagGlyf: t = &glyf_; break;
    case kTagKern: t = &kern_; break;
    case kTagGvar: t = &gvar_; break;
    case kTagFvar: t = &fvar_; break;
    default: return Table{};
  }
  return *t;
}

bool TtfFont::init(TtfStream& s) {
  ready_ = false;
  lastError_ = "not initialized";
  s_ = &s;
  fileSize_ = s.size();
  if (fileSize_ < 12) {
    lastError_ = "file too small for sfnt header";
    return false;
  }

  uint8_t hdr[12];
  if (!readAt(0, hdr, 12)) {
    lastError_ = "failed to read sfnt header";
    return false;
  }
  const uint32_t sfnt = rd32(hdr);
  if (sfnt != 0x00010000u && sfnt != 0x74727565u) {  // 0x00010000 or 'true'
    lastError_ = "not a TrueType font (OTTO/ttcf/WOFF rejected)";
    return false;
  }
  const uint16_t numTables = rd16(hdr + 4);
  if (numTables == 0 || numTables > 64) {
    lastError_ = "invalid table count";
    return false;
  }
  if (fileSize_ < 12 + (uint32_t)numTables * 16) {
    lastError_ = "table directory out of range";
    return false;
  }

  Table* slots[9] = {&head_, &maxp_, &loca_, &cmap_, &hhea_, &hmtx_, &glyf_, &gvar_, &fvar_};
  const uint32_t keys[9] = {kTagHead, kTagMaxp, kTagLoca, kTagCmap, kTagHhea, kTagHmtx, kTagGlyf, kTagGvar, kTagFvar};
  bool got[9] = {false};

  // Variable TrueType still stores the default-instance outlines in glyf.
  // gvar/fvar only describe deltas/axes away from that default. M4 has no UI
  // for selecting variation axes, so accepting the base glyf is both bounded
  // and much more useful than rejecting modern variable .ttf files outright.
  for (uint16_t i = 0; i < numTables; i++) {
    uint8_t rec[16];
    if (!readAt(12 + (uint32_t)i * 16, rec, 16)) {
      lastError_ = "failed to read table directory";
      return false;
    }
    const uint32_t key = rd32(rec);
    const uint32_t off = rd32(rec + 8);
    const uint32_t len = rd32(rec + 12);
    for (int k = 0; k < 9; k++) {
      if (key == keys[k]) {
        slots[k]->off = off;
        slots[k]->len = len;
        slots[k]->present = true;
        got[k] = true;
      }
    }
  }

  if (!(got[0] && got[1] && got[2] && got[3] && got[4] && got[5] && got[6])) {
    lastError_ = "missing required TTF tables (head/maxp/loca/cmap/hhea/hmtx/glyf)";
    return false;
  }
  kern_ = findTable(kTagKern);  // optional

  if (head_.len < 54) {
    lastError_ = "head table too small";
    return false;
  }
  uint8_t head[54];
  if (!readAt(head_.off, head, 54)) {
    lastError_ = "failed to read head table";
    return false;
  }
  unitsPerEm_ = rd16(head + 18);
  bboxYMax_ = rds16(head + 42);
  const int16_t indexToLocFormat = rds16(head + 50);
  longLoca_ = (indexToLocFormat != 0);
  if (unitsPerEm_ == 0 || unitsPerEm_ > 16384) {
    lastError_ = "invalid unitsPerEm";
    return false;
  }

  if (maxp_.len < 6) {
    lastError_ = "maxp table too small";
    return false;
  }
  uint8_t maxp[6];
  if (!readAt(maxp_.off, maxp, 6)) {
    lastError_ = "failed to read maxp table";
    return false;
  }
  numGlyphs_ = rd16(maxp + 4);
  if (numGlyphs_ <= 0) {
    lastError_ = "no glyphs";
    return false;
  }

  if (hhea_.len < 36) {
    lastError_ = "hhea table too small";
    return false;
  }
  uint8_t hhea[36];
  if (!readAt(hhea_.off, hhea, 36)) {
    lastError_ = "failed to read hhea table";
    return false;
  }
  ascender_ = rds16(hhea + 4);
  descender_ = rds16(hhea + 6);
  lineGap_ = rds16(hhea + 8);
  numHMetrics_ = rd16(hhea + 34);
  if (numHMetrics_ <= 0) {
    lastError_ = "no horizontal metrics";
    return false;
  }

  const uint32_t locaEntry = longLoca_ ? 4 : 2;
  if (loca_.len < locaEntry * (uint32_t)numGlyphs_) {
    lastError_ = "loca table too small";
    return false;
  }

  if (!initCmap()) return false;

  ready_ = true;
  lastError_ = "ok";
  return true;
}

bool TtfFont::initCmap() {
  if (cmap_.len < 4) {
    lastError_ = "cmap table too small";
    return false;
  }
  uint8_t cmapHdr[4];
  if (!readAt(cmap_.off, cmapHdr, 4)) {
    lastError_ = "failed to read cmap header";
    return false;
  }
  const uint16_t numTables = rd16(cmapHdr + 2);
  if (numTables == 0 || cmap_.len < 4 + (uint32_t)numTables * 8) {
    lastError_ = "cmap directory too small";
    return false;
  }
  if (numTables > 256) {
    lastError_ = "too many cmap subtables";
    return false;
  }

  uint8_t dir[2048];
  if (!readAt(cmap_.off + 4, dir, (uint32_t)numTables * 8)) {
    lastError_ = "failed to read cmap directory";
    return false;
  }

  uint32_t bestOff = 0;
  int bestScore = -1;
  for (uint16_t i = 0; i < numTables; i++) {
    const uint8_t* e = dir + (size_t)i * 8;
    const uint16_t platform = rd16(e);
    const uint16_t encoding = rd16(e + 2);
    const uint32_t off = rd32(e + 4);
    if (off + 2 > cmap_.len) continue;
    uint8_t fbuf[2];
    if (!readAt(cmap_.off + off, fbuf, 2)) continue;
    const uint16_t fmt = rd16(fbuf);
    if (fmt != 4 && fmt != 12) continue;
    int score = (fmt == 12) ? 100 : 0;
    if (platform == 3 && encoding == 10) score += 40;
    else if (platform == 3 && encoding == 1) score += 30;
    else if (platform == 0) score += 20;
    if (score > bestScore) {
      bestScore = score;
      bestOff = off;
    }
  }
  if (bestScore < 0) {
    lastError_ = "no suitable cmap subtable (need format 4 or 12)";
    return false;
  }

  const uint32_t avail = cmap_.len - bestOff;
  if (avail > 2u * 1024u * 1024u) {
    lastError_ = "cmap subtable exceeds memory safety limit";
    return false;
  }
  uint8_t* data = (uint8_t*)ttfAllocPsram(avail);
  if (!data) {
    lastError_ = "cmap alloc failed";
    return false;
  }
  if (!readAt(cmap_.off + bestOff, data, avail)) {
    ttfFree(data);
    lastError_ = "failed to read cmap subtable";
    return false;
  }
  ttfFree(cmapData_);
  cmapData_ = data;
  cmapLen_ = avail;

  const uint16_t fmt = rd16(data);
  if (fmt == 4) {
    if (avail < 14) {
      lastError_ = "cmap4 too small";
      return false;
    }
    cmapIs12_ = false;
    const uint16_t segCount = (uint16_t)(rd16(data + 6) / 2);
    const uint32_t minLen = 16u + static_cast<uint32_t>(segCount) * 8u;
    if (segCount == 0 || avail < minLen) {
      lastError_ = "cmap4 invalid segment count";
      return false;
    }
    nGroups_ = segCount;
  } else {
    if (avail < 16) {
      lastError_ = "cmap12 too small";
      return false;
    }
    cmapIs12_ = true;
    nGroups_ = rd32(data + 12);
    if (avail < 16u + nGroups_ * 12u) {
      lastError_ = "cmap12 invalid group count";
      return false;
    }
  }
  lastError_ = "ok";
  return true;
}

bool TtfFont::findGlyph(uint32_t cp, uint16_t& gid) const {
  gid = 0;
  if (!ready_ || !cmapData_) return false;
  if (!cmapIs12_) {
    const uint8_t* p = cmapData_;
    const uint32_t segSize = nGroups_ * 2;
    const uint8_t* endCode = p + 14;
    const uint8_t* startCode = endCode + segSize + 2;
    const uint8_t* idDelta = startCode + segSize;
    const uint8_t* idRange = idDelta + segSize;

    uint32_t lo = 0, hi = nGroups_;
    while (lo < hi) {
      const uint32_t mid = (lo + hi) / 2;
      const uint16_t end = rd16(endCode + mid * 2);
      if (cp > end) lo = mid + 1;
      else hi = mid;
    }
    if (lo >= nGroups_) return true;
    const uint16_t start = rd16(startCode + lo * 2);
    if (cp < start) return true;
    const int16_t delta = rds16(idDelta + lo * 2);
    const uint16_t rangeOff = rd16(idRange + lo * 2);
    if (rangeOff == 0) {
      gid = (uint16_t)(((int32_t)cp + delta) & 0xFFFF);
      return true;
    }
    const uint32_t base = (uint32_t)(idRange - p) + lo * 2;
    const uint32_t idxOff = base + rangeOff + (cp - start) * 2;
    if (idxOff + 2 > cmapLen_) return true;
    gid = rd16(p + idxOff);
    if (gid != 0) gid = (uint16_t)((int32_t)gid + delta);
    return true;
  }
  const uint8_t* groups = cmapData_ + 16;
  uint32_t lo = 0, hi = nGroups_;
  while (lo < hi) {
    const uint32_t mid = (lo + hi) / 2;
    const uint8_t* g = groups + (size_t)mid * 12;
    const uint32_t start = rd32(g);
    const uint32_t end = rd32(g + 4);
    if (cp > end) lo = mid + 1;
    else if (cp < start) hi = mid;
    else {
      gid = (uint16_t)(rd32(g + 8) + (cp - start));
      return true;
    }
  }
  return true;
}

bool TtfFont::glyphHMetrics(uint16_t gid, int32_t& advUnits, int32_t& lsbUnits) const {
  advUnits = 0;
  lsbUnits = 0;
  if (!ready_) return false;
  if (gid >= (uint16_t)numGlyphs_) gid = 0;
  const uint32_t longEntry = (uint32_t)numHMetrics_ * 4;
  if (gid < (uint32_t)numHMetrics_) {
    if (hmtx_.len < (uint32_t)gid * 4 + 4) return false;
    uint8_t buf[4];
    if (!readAt(hmtx_.off + (uint32_t)gid * 4, buf, 4)) return false;
    advUnits = rd16(buf);
    lsbUnits = (int16_t)rd16(buf + 2);
  } else {
    if (hmtx_.len < longEntry) return false;
    uint8_t advBuf[2];
    if (!readAt(hmtx_.off + longEntry - 4, advBuf, 2)) return false;
    advUnits = rd16(advBuf);
    const uint32_t idx = (uint32_t)gid - (uint32_t)numHMetrics_;
    if (hmtx_.len < longEntry + idx * 2 + 2) return false;
    uint8_t lb[2];
    if (!readAt(hmtx_.off + longEntry + idx * 2, lb, 2)) return false;
    lsbUnits = (int16_t)rd16(lb);
  }
  return true;
}

void TtfFont::fontVMetrics(int32_t& ascUnits, int32_t& descUnits, int32_t& gapUnits) const {
  ascUnits = ascender_;
  descUnits = descender_;
  gapUnits = lineGap_;
}

namespace {
constexpr uint16_t kOnCurve = 0x01;
constexpr uint16_t kXShort = 0x02;
constexpr uint16_t kYShort = 0x04;
constexpr uint16_t kRepeat = 0x08;
constexpr uint16_t kXSameOrPositive = 0x10;
constexpr uint16_t kYSameOrPositive = 0x20;
constexpr uint16_t kArgWords = 0x0001;
constexpr uint16_t kArgsAreXY = 0x0002;
constexpr uint16_t kMoreComps = 0x0020;
constexpr uint16_t kScale = 0x0008;
constexpr uint16_t kXyScale = 0x0040;
constexpr uint16_t kTwoByTwo = 0x0080;

struct Component {
  uint16_t gid = 0;
  bool pointAttach = false;
  uint16_t parentPoint = 0;
  uint16_t childPoint = 0;
  float tx = 0, ty = 0;
  float a = 1, b = 0, c = 0, d = 1;
};

bool flattenedPointAt(const std::vector<Contour>& contours, size_t startContour,
                      uint32_t pointIndex, Pt& out) {
  for (size_t i = startContour; i < contours.size(); ++i) {
    if (pointIndex < contours[i].pts.size()) {
      out = contours[i].pts[pointIndex];
      return true;
    }
    pointIndex -= static_cast<uint32_t>(contours[i].pts.size());
  }
  return false;
}

void translateContours(std::vector<Contour>& contours, float dx, float dy) {
  for (auto& contour : contours) {
    for (auto& p : contour.pts) {
      p.x += dx;
      p.y += dy;
    }
  }
}
}  // namespace

bool TtfFont::collectGlyph(uint16_t gid, const Xform& xf, std::vector<Contour>& out, int depth) const {
  if (depth > 8 || gid >= (uint16_t)numGlyphs_) return false;
  uint32_t gStart = 0, gEnd = 0;
  const uint32_t base = loca_.off + (uint32_t)gid * (longLoca_ ? 4 : 2);
  uint8_t lb[8];
  if (longLoca_) {
    if (!readAt(base, lb, 8)) return false;
    gStart = rd32(lb);
    gEnd = rd32(lb + 4);
  } else {
    if (!readAt(base, lb, 4)) return false;
    gStart = (uint32_t)rd16(lb) * 2;
    gEnd = (uint32_t)rd16(lb + 2) * 2;
  }
  if (gStart == gEnd) return true;
  if (gStart > gEnd || gStart > glyf_.len || gEnd > glyf_.len) return false;

  const uint32_t sliceLen = gEnd - gStart;
  if (sliceLen > glyfScratchCap_) {
    uint8_t* nb = (uint8_t*)ttfReallocPsram(glyfScratch_, sliceLen);
    if (!nb) return false;
    glyfScratch_ = nb;
    glyfScratchCap_ = sliceLen;
  }
  if (!readAt(glyf_.off + gStart, glyfScratch_, sliceLen)) return false;
  const uint8_t* p = glyfScratch_;
  if (sliceLen < 10) return false;

  const int16_t numContours = rds16(p);
  if (numContours >= 0) {
    if (numContours == 0) return true;
    if (sliceLen < 10 + (size_t)numContours * 2 + 2) return false;
    const uint8_t* endPts = p + 10;
    const uint16_t lastEndPt = rd16(endPts + (size_t)(numContours - 1) * 2);
    const uint32_t pointCount = (uint32_t)lastEndPt + 1;
    if (pointCount > 4096) return false;
    const uint16_t instLen = rd16(endPts + (size_t)numContours * 2);
    const uint8_t* flagsPtr = endPts + (size_t)numContours * 2 + 2 + instLen;
    if (flagsPtr >= p + sliceLen) return false;

    std::vector<uint8_t> flags;
    flags.reserve(pointCount);
    while (flags.size() < pointCount) {
      if (flagsPtr >= p + sliceLen) return false;
      uint8_t f = *flagsPtr++;
      flags.push_back(f);
      if ((f & kRepeat) != 0) {
        if (flagsPtr >= p + sliceLen) return false;
        uint8_t rep = *flagsPtr++;
        if (flags.size() + rep > pointCount) return false;
        for (uint8_t i = 0; i < rep; i++) flags.push_back(f);
      }
    }

    std::vector<int16_t> xs(pointCount);
    int32_t x = 0;
    const uint8_t* xp = flagsPtr;
    for (uint32_t i = 0; i < pointCount; i++) {
      const uint8_t f = flags[i];
      if ((f & kXShort) != 0) {
        if (xp >= p + sliceLen) return false;
        const uint8_t dx = *xp++;
        x += ((f & kXSameOrPositive) != 0) ? (int32_t)dx : -(int32_t)dx;
      } else if ((f & kXSameOrPositive) == 0) {
        if (p + sliceLen - xp < 2) return false;
        x += (int16_t)rd16(xp);
        xp += 2;
      }
      xs[i] = (int16_t)x;
    }

    std::vector<int16_t> ys(pointCount);
    int32_t y = 0;
    const uint8_t* yp = xp;
    for (uint32_t i = 0; i < pointCount; i++) {
      const uint8_t f = flags[i];
      if ((f & kYShort) != 0) {
        if (yp >= p + sliceLen) return false;
        const uint8_t dy = *yp++;
        y += ((f & kYSameOrPositive) != 0) ? (int32_t)dy : -(int32_t)dy;
      } else if ((f & kYSameOrPositive) == 0) {
        if (p + sliceLen - yp < 2) return false;
        y += (int16_t)rd16(yp);
        yp += 2;
      }
      ys[i] = (int16_t)y;
    }

    uint32_t idx = 0;
    for (int c = 0; c < numContours; c++) {
      const uint16_t lastPt = rd16(endPts + (size_t)c * 2);
      if (lastPt < idx || lastPt >= pointCount) return false;
      Contour ct;
      ct.pts.reserve((size_t)lastPt - idx + 1);
      for (uint32_t i = idx; i <= (uint32_t)lastPt; i++) {
        ct.pts.push_back(applyXform(xf, (float)xs[i], (float)ys[i], (flags[i] & kOnCurve) != 0));
      }
      out.push_back(std::move(ct));
      idx = (uint32_t)lastPt + 1;
    }
    return true;
  }

  std::vector<Component> comps;
  uint32_t pos = 10;
  while (true) {
    if (pos > sliceLen || sliceLen - pos < 4) return false;
    const uint16_t flags = rd16(p + pos);
    const uint16_t compGid = rd16(p + pos + 2);
    pos += 4;
    if (compGid >= (uint16_t)numGlyphs_) return false;
    Component comp;
    comp.gid = compGid;
    comp.pointAttach = (flags & kArgsAreXY) == 0;
    if ((flags & kArgWords) != 0) {
      if (sliceLen - pos < 4) return false;
      if (comp.pointAttach) {
        comp.parentPoint = rd16(p + pos);
        comp.childPoint = rd16(p + pos + 2);
      } else {
        comp.tx = (int16_t)rd16(p + pos);
        comp.ty = (int16_t)rd16(p + pos + 2);
      }
      pos += 4;
    } else {
      if (sliceLen - pos < 2) return false;
      if (comp.pointAttach) {
        comp.parentPoint = p[pos];
        comp.childPoint = p[pos + 1];
      } else {
        comp.tx = (int8_t)p[pos];
        comp.ty = (int8_t)p[pos + 1];
      }
      pos += 2;
    }
    if ((flags & kTwoByTwo) != 0) {
      if (sliceLen - pos < 8) return false;
      comp.a = (int16_t)rd16(p + pos) / 16384.0f;
      comp.b = (int16_t)rd16(p + pos + 2) / 16384.0f;
      comp.c = (int16_t)rd16(p + pos + 4) / 16384.0f;
      comp.d = (int16_t)rd16(p + pos + 6) / 16384.0f;
      pos += 8;
    } else if ((flags & kXyScale) != 0) {
      if (sliceLen - pos < 4) return false;
      comp.a = (int16_t)rd16(p + pos) / 16384.0f;
      comp.d = (int16_t)rd16(p + pos + 2) / 16384.0f;
      pos += 4;
    } else if ((flags & kScale) != 0) {
      if (sliceLen - pos < 2) return false;
      comp.a = comp.d = (int16_t)rd16(p + pos) / 16384.0f;
      pos += 2;
    }
    comps.push_back(comp);
    if ((flags & kMoreComps) == 0) break;
  }

  const size_t compoundStartContour = out.size();
  for (const auto& c : comps) {
    Xform child;
    child.a = xf.a * c.a + xf.c * c.b;
    child.b = xf.b * c.a + xf.d * c.b;
    child.c = xf.a * c.c + xf.c * c.d;
    child.d = xf.b * c.c + xf.d * c.d;
    if (!c.pointAttach) {
      child.tx = xf.a * c.tx + xf.c * c.ty + xf.tx;
      child.ty = xf.b * c.tx + xf.d * c.ty + xf.ty;
      if (!collectGlyph(c.gid, child, out, depth + 1)) return false;
      continue;
    }
    child.tx = xf.tx;
    child.ty = xf.ty;
    std::vector<Contour> childContours;
    if (!collectGlyph(c.gid, child, childContours, depth + 1)) return false;
    Pt parentAnchor{};
    Pt childAnchor{};
    if (!flattenedPointAt(out, compoundStartContour, c.parentPoint, parentAnchor) ||
        !flattenedPointAt(childContours, 0, c.childPoint, childAnchor)) {
      return false;
    }
    translateContours(childContours, parentAnchor.x - childAnchor.x, parentAnchor.y - childAnchor.y);
    for (auto& contour : childContours) out.push_back(std::move(contour));
  }
  return true;
}

bool TtfFont::glyphPixelBox(uint16_t gid, uint16_t sizePx, int& x0, int& y0, int& x1, int& y1) const {
  x0 = y0 = x1 = y1 = 0;
  std::vector<Contour> contours;
  if (!collectGlyph(gid, Xform{}, contours, 0)) return false;
  const float scale = (float)sizePx / (float)unitsPerEm_;
  float minX = 0, maxX = 0, minY = 0, maxY = 0;
  bool first = true;
  for (const auto& c : contours) {
    for (const auto& p : c.pts) {
      if (first) {
        minX = maxX = p.x;
        minY = maxY = p.y;
        first = false;
      } else {
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
      }
    }
  }
  if (first) return true;
  x0 = (int)std::floor(minX * scale);
  y0 = (int)std::floor(-maxY * scale);
  x1 = (int)std::ceil(maxX * scale);
  y1 = (int)std::ceil(-minY * scale);
  return true;
}

namespace {
struct Seg { float x0, y0, x1, y1; };
void emitQuad(const Pt& p0, const Pt& ctrl, const Pt& end, std::vector<Seg>& out) {
  const float dx = end.x - p0.x;
  const float dy = end.y - p0.y;
  const float chordLen = std::sqrt(dx * dx + dy * dy);
  const float dev = std::fabs((ctrl.x - p0.x) * dy - (ctrl.y - p0.y) * dx) / std::max(chordLen, 1e-3f);
  const int k = (dev > 0.5f) ? std::min(32, 1 + (int)(dev / 0.5f)) : 1;
  for (int s = 0; s < k; s++) {
    const float t0 = (float)s / k;
    const float t1 = (float)(s + 1) / k;
    const float mt0 = 1.0f - t0;
    const float mt1 = 1.0f - t1;
    out.push_back({mt0 * mt0 * p0.x + 2 * mt0 * t0 * ctrl.x + t0 * t0 * end.x,
                   mt0 * mt0 * p0.y + 2 * mt0 * t0 * ctrl.y + t0 * t0 * end.y,
                   mt1 * mt1 * p0.x + 2 * mt1 * t1 * ctrl.x + t1 * t1 * end.x,
                   mt1 * mt1 * p0.y + 2 * mt1 * t1 * ctrl.y + t1 * t1 * end.y});
  }
}
void tesselateContour(const std::vector<Pt>& pts, std::vector<Seg>& out) {
  const size_t n = pts.size();
  if (n < 2) return;
  size_t startIdx = n;
  for (size_t i = 0; i < n; i++) if (pts[i].on) { startIdx = i; break; }
  if (startIdx == n) {
    std::vector<Pt> mids;
    mids.reserve(n);
    for (size_t i = 0; i < n; i++) {
      const Pt& a = pts[i];
      const Pt& b = pts[(i + 1) % n];
      mids.push_back({(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, true});
    }
    for (size_t i = 0; i < mids.size(); i++) {
      const Pt& a = mids[i];
      const Pt& b = mids[(i + 1) % mids.size()];
      out.push_back({a.x, a.y, b.x, b.y});
    }
    return;
  }
  std::vector<Pt> seq;
  seq.reserve(n);
  for (size_t i = 0; i < n; i++) seq.push_back(pts[(startIdx + i) % n]);
  const size_t m = seq.size();
  Pt cur = seq[0];
  size_t i = 1;
  while (i < m) {
    const Pt next = seq[i % m];
    if (next.on) {
      out.push_back({cur.x, cur.y, next.x, next.y});
      cur = next;
      i++;
    } else {
      const Pt ctrl = next;
      const Pt after = seq[(i + 1) % m];
      if (after.on) {
        emitQuad(cur, ctrl, after, out);
        cur = after;
        i += 2;
      } else {
        const Pt mid = {(ctrl.x + after.x) * 0.5f, (ctrl.y + after.y) * 0.5f, true};
        emitQuad(cur, ctrl, mid, out);
        cur = mid;
        i += 1;
      }
    }
  }
  if (std::fabs(cur.x - seq[0].x) > 0.01f || std::fabs(cur.y - seq[0].y) > 0.01f)
    out.push_back({cur.x, cur.y, seq[0].x, seq[0].y});
}
}  // namespace

bool TtfFont::rasterize(uint16_t gid, uint16_t sizePx, GlyphBitmap& out) {
  out = GlyphBitmap{};
  if (!ready_) return false;
  const float scale = (float)sizePx / (float)unitsPerEm_;
  int32_t advUnits = 0, lsbUnits = 0;
  glyphHMetrics(gid, advUnits, lsbUnits);
  out.advance = (int16_t)std::lround(advUnits * scale);
  std::vector<Contour> contours;
  if (!collectGlyph(gid, Xform{}, contours, 0)) return false;
  float minX = 0, maxX = 0, minY = 0, maxY = 0;
  bool first = true;
  for (const auto& c : contours) for (const auto& p : c.pts) {
    if (first) { minX=maxX=p.x; minY=maxY=p.y; first=false; }
    else { minX=std::min(minX,p.x); maxX=std::max(maxX,p.x); minY=std::min(minY,p.y); maxY=std::max(maxY,p.y); }
  }
  if (first) return true;
  const int x0=(int)std::floor(minX*scale), y0=(int)std::floor(-maxY*scale);
  const int x1=(int)std::ceil(maxX*scale), y1=(int)std::ceil(-minY*scale);
  const int w=x1-x0, h=y1-y0;
  if (w<=0||h<=0) return true;
  if (w>255||h>255) { lastError_="glyph larger than 255px (EpdGlyph limit)"; return false; }
  std::vector<Seg> segs;
  std::vector<Pt> pxs;
  for (const auto& c : contours) {
    if (c.pts.size()<2) continue;
    pxs.clear(); pxs.reserve(c.pts.size());
    for (const auto& p : c.pts) pxs.push_back({p.x*scale-x0,-p.y*scale-y0,p.on});
    tesselateContour(pxs,segs);
  }
  if (segs.empty()) return true;
  const uint32_t npix=(uint32_t)w*(uint32_t)h;
  if (npix>covScratchCap_) { uint8_t* nb=(uint8_t*)ttfRealloc(covScratch_,npix); if(!nb)return false; covScratch_=nb; covScratchCap_=npix; }
  std::memset(covScratch_,0,npix);
  std::vector<float> xs;
  std::vector<int8_t> signs;
  xs.reserve(segs.size()); signs.reserve(segs.size());
  for(int py=0;py<h;py++) for(int sub=0;sub<2;sub++) {
    const float y=py+(sub==0?0.25f:0.75f);
    xs.clear(); signs.clear();
    for(const auto& seg:segs){ const float ymin=std::min(seg.y0,seg.y1),ymax=std::max(seg.y0,seg.y1); if(y>=ymin&&y<ymax){ xs.push_back(seg.x0+(y-seg.y0)*(seg.x1-seg.x0)/(seg.y1-seg.y0)); signs.push_back((seg.y1>seg.y0)?1:-1); } }
    for(size_t i=1;i<xs.size();i++){const float v=xs[i];const int8_t s=signs[i];size_t j=i;while(j>0&&xs[j-1]>v){xs[j]=xs[j-1];signs[j]=signs[j-1];j--;}xs[j]=v;signs[j]=s;}
    int winding=0;for(size_t i=0;i+1<xs.size();i++){winding+=signs[i];if(winding==0)continue;const float xa=xs[i],xb=xs[i+1];const int pxa=(int)std::floor(xa),pxb=(int)std::ceil(xb);for(int px=pxa;px<pxb;px++){if(px<0||px>=w)continue;const float xlo=(px>xa)?(float)px:xa;const float xhi=(px+1<xb)?(float)(px+1):xb;if(xhi>xlo)covScratch_[(size_t)py*w+px]+=(uint8_t)((xhi-xlo)*4.0f+0.5f);}}
  }
  const uint32_t packedLen=(npix+3)/4;
  if(packedLen>packedScratchCap_){uint8_t*nb=(uint8_t*)ttfReallocPsram(packedScratch_,packedLen);if(!nb)return false;packedScratch_=nb;packedScratchCap_=packedLen;}
  std::memset(packedScratch_,0,packedLen);
  for(uint32_t i=0;i<npix;i++){uint8_t lvl=(uint8_t)(((uint32_t)covScratch_[i]*3u+7u)/8u);if(lvl>3)lvl=3;const uint8_t shift=(3-(i%4))*2;packedScratch_[i/4]|=(uint8_t)(lvl<<shift);}
  out.data=packedScratch_; out.width=(int16_t)w; out.height=(int16_t)h; out.xoff=(int16_t)x0; out.yoff=(int16_t)(-y0); out.packedLen=(uint16_t)packedLen;
  lastError_="ok"; return true;
}

void TtfFont::clearScratch() {
  ttfFree(glyfScratch_); glyfScratch_=nullptr; glyfScratchCap_=0;
  ttfFree(covScratch_); covScratch_=nullptr; covScratchCap_=0;
  ttfFree(packedScratch_); packedScratch_=nullptr; packedScratchCap_=0;
}

}  // namespace ttf
