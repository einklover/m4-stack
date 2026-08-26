#include "TtfEpdFont.h"

#include <HardwareSerial.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

extern void m4AppendFontDiagnostic(const char* line);
extern "C" void m4YieldToDebugBridge() __attribute__((weak));
extern "C" void m4YieldToDebugBridge() {}

namespace {

uint16_t be16(const uint8_t* p) {
  return static_cast<uint16_t>((uint16_t(p[0]) << 8) | p[1]);
}
uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) |
         static_cast<uint32_t>(p[3]);
}

class SdTtfStream : public ttf::TtfStream {
 public:
  bool open(const String& path) {
    close();
    traceOps_ = 0;
    const bool ok = SdMan.openFileForRead("TtfFont", path.c_str(), file_);
    char line[256];
    snprintf(line, sizeof(line), "stream_open path=%s ok=%d size=%llu",
             path.c_str(), ok ? 1 : 0,
             static_cast<unsigned long long>(ok ? file_.fileSize() : 0));
    m4AppendFontDiagnostic(line);
    return ok;
  }

  void close() {
    if (file_.isOpen()) file_.close();
  }

  uint32_t size() const override {
    return file_.isOpen() ? file_.fileSize() : 0;
  }

  bool seek(uint32_t pos) override {
    bool ok = file_.isOpen() && file_.seekSet(pos);
    for (uint8_t attempt = 0; !ok && attempt < 3 && file_.isOpen(); ++attempt) {
      if (m4YieldToDebugBridge) m4YieldToDebugBridge();
      delay(2);
      ok = file_.seekSet(pos);
    }

    bool sequentialFallback = false;
    if (!ok && file_.isOpen() && pos <= file_.fileSize()) {
      file_.rewind();
      uint8_t discard[512];
      uint32_t remaining = pos;
      uint32_t chunksSinceYield = 0;
      while (remaining) {
        const uint32_t want = std::min<uint32_t>(remaining, sizeof(discard));
        const int got = file_.read(discard, want);
        if (got <= 0) break;
        remaining -= static_cast<uint32_t>(got);
        if (++chunksSinceYield >= 8 && m4YieldToDebugBridge) {
          m4YieldToDebugBridge();
          chunksSinceYield = 0;
        }
      }
      ok = remaining == 0;
      sequentialFallback = ok;
    }

    if (traceOps_ < 100) {
      char line[200];
      snprintf(line, sizeof(line),
               "stream_seek pos=%lu ok=%d seq=%d cur=%llu size=%llu err=%u",
               static_cast<unsigned long>(pos), ok ? 1 : 0,
               sequentialFallback ? 1 : 0,
               static_cast<unsigned long long>(file_.isOpen() ? file_.curPosition() : 0),
               static_cast<unsigned long long>(file_.isOpen() ? file_.fileSize() : 0),
               static_cast<unsigned>(file_.isOpen() ? file_.getError() : 0xff));
      m4AppendFontDiagnostic(line);
      ++traceOps_;
    }
    return ok;
  }

  uint32_t read(void* dst, uint32_t n) override {
    if (!file_.isOpen()) return 0;
    auto* out = static_cast<uint8_t*>(dst);
    uint32_t total = 0;
    const uint64_t start = file_.curPosition();
    while (total < n) {
      int got = file_.read(out + total, n - total);
      for (uint8_t attempt = 0; got <= 0 && attempt < 3; ++attempt) {
        if (m4YieldToDebugBridge) m4YieldToDebugBridge();
        delay(2);
        got = file_.read(out + total, n - total);
      }
      if (got <= 0) break;
      total += static_cast<uint32_t>(got);
      // Conservative throttle: yield every 8KB (not every 4KB) to avoid excessive context switches
      if ((total & 0x1FFF) == 0 && m4YieldToDebugBridge) m4YieldToDebugBridge();
    }
    if (traceOps_ < 100) {
      char line[180];
      snprintf(line, sizeof(line),
               "stream_read pos=%llu want=%lu got=%lu end=%llu",
               static_cast<unsigned long long>(start),
               static_cast<unsigned long>(n),
               static_cast<unsigned long>(total),
               static_cast<unsigned long long>(file_.curPosition()));
      m4AppendFontDiagnostic(line);
      ++traceOps_;
    }
    return total;
  }

 private:
  FsFile file_;
  uint8_t traceOps_ = 0;
};

class MemoryTtfStream : public ttf::TtfStream {
 public:
  MemoryTtfStream(const uint8_t* data, uint32_t size) : data_(data), size_(size) {}
  uint32_t size() const override { return size_; }
  bool seek(uint32_t pos) override {
    if (pos > size_) return false;
    pos_ = pos;
    return true;
  }
  uint32_t read(void* dst, uint32_t n) override {
    if (!data_ || !dst || pos_ >= size_) return 0;
    const uint32_t take = std::min(n, size_ - pos_);
    if (take) {
      std::memcpy(dst, data_ + pos_, take);
      pos_ += take;
    }
    return take;
  }

 private:
  const uint8_t* data_ = nullptr;
  uint32_t size_ = 0;
  uint32_t pos_ = 0;
};

enum class OpenTypeKind : uint8_t { Glyf, Cff1, Cff2, Invalid };

const char* kindName(OpenTypeKind kind) {
  switch (kind) {
    case OpenTypeKind::Glyf: return "glyf";
    case OpenTypeKind::Cff1: return "cff1";
    case OpenTypeKind::Cff2: return "cff2-default";
    default: return "invalid";
  }
}

OpenTypeKind probeSfntFace(SdTtfStream& stream, uint32_t faceOffset) {
  if (faceOffset > stream.size() || stream.size() - faceOffset < 12) {
    return OpenTypeKind::Invalid;
  }

  uint8_t header[12];
  if (!stream.seek(faceOffset) || stream.read(header, sizeof(header)) != sizeof(header)) {
    return OpenTypeKind::Invalid;
  }
  const uint32_t signature = be32(header);
  if (signature != 0x00010000u && signature != 0x74727565u &&
      signature != 0x4f54544fu) {
    return OpenTypeKind::Invalid;
  }

  const uint16_t numTables = be16(header + 4);
  if (!numTables || numTables > 128 ||
      uint64_t(faceOffset) + 12u + uint64_t(numTables) * 16u > stream.size()) {
    return OpenTypeKind::Invalid;
  }

  bool head = false, maxp = false, loca = false, cmap = false;
  bool hhea = false, hmtx = false, glyf = false, cff = false, cff2 = false;
  for (uint16_t i = 0; i < numTables; ++i) {
    uint8_t record[16];
    if (!stream.seek(faceOffset + 12u + uint32_t(i) * 16u) ||
        stream.read(record, sizeof(record)) != sizeof(record)) {
      return OpenTypeKind::Invalid;
    }
    const uint32_t tableTag = be32(record);
    const uint32_t tableOffset = be32(record + 8);
    const uint32_t tableLength = be32(record + 12);
    if (tableOffset > stream.size() || tableLength > stream.size() - tableOffset) {
      return OpenTypeKind::Invalid;
    }
    head |= tableTag == 0x68656164u;
    maxp |= tableTag == 0x6d617870u;
    loca |= tableTag == 0x6c6f6361u;
    cmap |= tableTag == 0x636d6170u;
    hhea |= tableTag == 0x68686561u;
    hmtx |= tableTag == 0x686d7478u;
    glyf |= tableTag == 0x676c7966u;
    cff |= tableTag == 0x43464620u;
    cff2 |= tableTag == 0x43464632u;
  }

  if (head && maxp && loca && cmap && hhea && hmtx && glyf) {
    return OpenTypeKind::Glyf;
  }
  const bool cffCore = signature == 0x4f54544fu && head && maxp && cmap && hhea && hmtx;
  if (cffCore && cff2) return OpenTypeKind::Cff2;
  if (cffCore && cff) return OpenTypeKind::Cff1;
  return OpenTypeKind::Invalid;
}

OpenTypeKind probeOpenType(SdTtfStream& stream) {
  return probeSfntFace(stream, 0);
}

bool probeCollection(SdTtfStream& stream, uint32_t& faceOffset,
                     OpenTypeKind& kind, bool& sawCff2) {
  faceOffset = 0;
  kind = OpenTypeKind::Invalid;
  sawCff2 = false;
  if (stream.size() < 16) return false;

  uint8_t header[12];
  if (!stream.seek(0) || stream.read(header, sizeof(header)) != sizeof(header) ||
      std::memcmp(header, "ttcf", 4) != 0) {
    return false;
  }
  const uint32_t count = be32(header + 8);
  if (!count || count > 64 || 12u + uint64_t(count) * 4u > stream.size()) {
    return false;
  }

  for (uint32_t i = 0; i < count; ++i) {
    uint8_t rawOffset[4];
    if (!stream.seek(12u + i * 4u) || stream.read(rawOffset, 4) != 4) return false;
    const uint32_t candidateOffset = be32(rawOffset);
    const OpenTypeKind candidate = probeSfntFace(stream, candidateOffset);
    if (candidate == OpenTypeKind::Cff2) sawCff2 = true;
    if (candidate == OpenTypeKind::Cff1 || candidate == OpenTypeKind::Cff2 ||
        candidate == OpenTypeKind::Glyf) {
      faceOffset = candidateOffset;
      kind = candidate;
      char line[160];
      snprintf(line, sizeof(line),
               "collection_face_probe index=%lu offset=%lu backend=%s",
               static_cast<unsigned long>(i),
               static_cast<unsigned long>(candidateOffset), kindName(candidate));
      m4AppendFontDiagnostic(line);
      return true;
    }
  }
  return false;
}

void* ttfAlloc(size_t n) {
#if defined(ESP32) && defined(BOARD_HAS_PSRAM)
  if (psramFound()) return ps_malloc(n);
#endif
  return malloc(n);
}

void ttfFree(void* p) { free(p); }

int clampMetric(int value, int low, int high) {
  if (low > high) std::swap(low, high);
  return std::max(low, std::min(high, value));
}

}  // namespace

void* TtfEpdFont::ttfAlloc(size_t n) { return ::ttfAlloc(n); }
void TtfEpdFont::ttfFree(void* p) { ::ttfFree(p); }

const char* TtfEpdFont::backendName() const {
  switch (backend_) {
    case Backend::Glyf: return "glyf";
    case Backend::Cff1: return "cff1";
    case Backend::Cff2: return "cff2-default";
  }
  return "unknown";
}
const char* TtfEpdFont::backendError() const {
  return usesCffBackend() ? cffFont_.lastError() : font_.lastError();
}
uint16_t TtfEpdFont::backendUnitsPerEm() const {
  return usesCffBackend() ? cffFont_.unitsPerEm() : font_.unitsPerEm();
}
int32_t TtfEpdFont::backendBBoxYMax() const {
  return usesCffBackend() ? cffFont_.fontBBoxYMax() : font_.fontBBoxYMax();
}
bool TtfEpdFont::backendFindGlyph(uint32_t cp, uint16_t& gid) const {
  return usesCffBackend() ? cffFont_.findGlyph(cp, gid) : font_.findGlyph(cp, gid);
}
bool TtfEpdFont::backendHMetrics(uint16_t gid, int32_t& advUnits, int32_t& lsbUnits) const {
  return usesCffBackend() ? cffFont_.glyphHMetrics(gid, advUnits, lsbUnits)
                          : font_.glyphHMetrics(gid, advUnits, lsbUnits);
}

int TtfEpdFont::lookupAdvancePx(uint32_t cp) const {
  if (!valid_) return 0;
  // Spaces: prefer real hmtx advance (keeps per-font proportional), fallback to em/3 only on failure.
  if (cp == 0x20 || cp == 0x3000 || cp == 0x00A0) {
    const uint16_t slot = static_cast<uint16_t>(cp % kAdvanceCache);
    if (advCp_[slot] == cp && advPx_[slot] != 0) return advPx_[slot];
    uint16_t gid = 0;
    int32_t advUnits = 0, lsb = 0;
    if (backendFindGlyph(cp, gid) && gid != 0 && backendHMetrics(gid, advUnits, lsb)) {
      const uint16_t upm = backendUnitsPerEm();
      const int px = upm ? std::max(1, int(std::lround(float(advUnits) * float(sizePx_) / float(upm)))) : 0;
      if (px > 0) {
        const int clamped = std::max(1, std::min(255, px));
        advCp_[slot] = cp;
        advPx_[slot] = static_cast<uint8_t>(clamped);
        return clamped;
      }
    }
    const int sp = std::max(1, int(sizePx_) / 3);
    const int clamped = std::max(1, std::min(255, sp));
    advCp_[slot] = cp;
    advPx_[slot] = static_cast<uint8_t>(clamped);
    return clamped;
  }
  for (uint16_t i = 0; i < maxSlots_ && entries_; ++i) {
    if (entries_[i].cp == cp) return entries_[i].glyph.advanceX;
  }
  const uint16_t slot = static_cast<uint16_t>(cp % kAdvanceCache);
  if (advCp_[slot] == cp && advPx_[slot] != 0) return advPx_[slot];

  uint16_t gid = 0;
  if (!backendFindGlyph(cp, gid) || (gid == 0 && cp != static_cast<uint32_t>('?'))) {
    if (cp == static_cast<uint32_t>('?')) return std::max(1, int(sizePx_) / 2);
    return lookupAdvancePx(static_cast<uint32_t>('?'));
  }
  int32_t advUnits = 0, lsb = 0;
  if (!backendHMetrics(gid, advUnits, lsb)) {
    if (cp == static_cast<uint32_t>('?')) return std::max(1, int(sizePx_) / 2);
    return lookupAdvancePx(static_cast<uint32_t>('?'));
  }
  const uint16_t upm = backendUnitsPerEm();
  const int px = upm ? std::max(1, int(std::lround(float(advUnits) * float(sizePx_) / float(upm))))
                     : std::max(1, int(sizePx_));
  const int clamped = std::max(1, std::min(255, px));
  advCp_[slot] = cp;
  advPx_[slot] = static_cast<uint8_t>(clamped);
  ++advClock_;
  return clamped;
}

int TtfEpdFont::glyphAdvanceX(uint32_t cp, const EpdFontStyles::Style style) const {
  (void)style;
#if defined(ESP32)
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
#endif
  const int adv = lookupAdvancePx(cp);
#if defined(ESP32)
  if (mutex_) xSemaphoreGive(mutex_);
#endif
  return adv;
}
bool TtfEpdFont::backendRasterize(uint16_t gid, ttf::GlyphBitmap& out) const {
  return usesCffBackend() ? cffFont_.rasterize(gid, renderSizePx_, out)
                          : font_.rasterize(gid, renderSizePx_, out);
}
bool TtfEpdFont::backendPixelBox(uint16_t gid, int& x0, int& y0, int& x1, int& y1) const {
  return usesCffBackend()
      ? cffFont_.glyphPixelBox(gid, renderSizePx_, x0, y0, x1, y1)
      : font_.glyphPixelBox(gid, renderSizePx_, x0, y0, x1, y1);
}
void TtfEpdFont::backendVMetrics(int32_t& asc, int32_t& desc, int32_t& gap) const {
  if (usesCffBackend()) cffFont_.fontVMetrics(asc, desc, gap);
  else font_.fontVMetrics(asc, desc, gap);
}
void TtfEpdFont::backendClearScratch() {
  if (usesCffBackend()) cffFont_.clearScratch();
  else font_.clearScratch();
}
const char* TtfEpdFont::lastError() const {
  return runtimeError_.length() ? runtimeError_.c_str() : backendError();
}
bool TtfEpdFont::hasCodepoint(uint32_t cp) const {
  if (!valid_) return false;
  // Whitespace codepoints are considered present even if glyf slice is empty;
  // callers use hasCodepoint to decide fallback to '?' - spaces must not become '?'.
  if (cp == 0x20 || cp == 0x3000 || cp == 0x00A0 || cp == 0x09 || cp == 0x0A) return true;
  uint16_t gid = 0;
  return backendFindGlyph(cp, gid) && gid != 0;
}

bool TtfEpdFont::allocateEntries() {
  if (maxSlots_ == 0) maxSlots_ = 1;
  if (cacheBudget_ == 0) cacheBudget_ = 1;
  entries_ = static_cast<Entry*>(ttfAlloc(sizeof(Entry) * maxSlots_));
  if (!entries_) {
    runtimeError_ = "glyph cache metadata allocation failed";
    return false;
  }
  for (uint16_t i = 0; i < maxSlots_; ++i) new (&entries_[i]) Entry();
  return true;
}

bool TtfEpdFont::finishInit(const char* label) {
  if (!stream_) return false;
  const bool ok = usesCffBackend()
      ? cffFont_.init(*stream_, faceOffset_)
      : font_.init(*stream_, faceOffset_);
  if (!ok) {
    Serial.printf("[TTF] Invalid runtime font %s backend=%s: %s\n",
                  label ? label : "?", backendName(), backendError());
    return false;
  }
  if (usesCffBackend()) {
    const bool expectedCff2 = backend_ == Backend::Cff2;
    if (cffFont_.isCff2() != expectedCff2) {
      runtimeError_ = "OpenType CFF backend probe/parser mismatch";
      return false;
    }
  }

  const uint16_t upm = backendUnitsPerEm();
  if (!upm) return false;
  // Preserve the configured nominal reader size. Do not rewrite raster size from
  // a CJK bbox heuristic: that over-normalized many faces on hardware. Only the
  // baseline is derived from a box reference; horizontal bearings stay native.
  // renderSizePx_ tracks sizePx_ for API compatibility.
  const int nominal = std::max(1, int(sizePx_));
  renderSizePx_ = static_cast<uint16_t>(nominal);
  visualScale_ = 1.0f;
  visualReferenceCodepoint_ = 0;

  const float scale = float(renderSizePx_) / float(upm);
  int32_t asc = 0, desc = 0, gap = 0;
  backendVMetrics(asc, desc, gap);
  const int rawAsc = int(std::lround(asc * scale));
  const int rawDesc = int(std::lround(desc * scale));
  const int gapPx = std::max(0, int(std::lround(gap * scale)));
  const int bboxTop = std::max(0, int(std::lround(backendBBoxYMax() * scale)));

  bool refValid = false;
  int refTop = 0, refBottom = 0;
  // Prefer a box-like visual reference for baseline metrics only. `口` is first
  // because it gives a stable vertical ink box without changing the font's
  // advances, bearings, or nominal raster size.
  for (size_t i = 0; i < M4TtfVisualNormalization::kReferenceCodepointCount; ++i) {
    const uint32_t cp = M4TtfVisualNormalization::kReferenceCodepoints[i];
    uint16_t gid = 0;
    if (!backendFindGlyph(cp, gid) || gid == 0) continue;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (!backendPixelBox(gid, x0, y0, x1, y1)) continue;
    const int top = std::max(0, -y0);
    const int bottom = std::max(0, y1);
    const int height = top + bottom;
    if (height < std::max(2, nominal / 2) || height > 255) continue;
    refValid = true;
    visualReferenceCodepoint_ = cp;
    refTop = top;
    refBottom = bottom;
    break;
  }

  int ascPx = refValid
      ? clampMetric(refTop,
                    std::max(1, int(std::lround(nominal * .55f))),
                    std::max(1, int(std::lround(nominal * 1.10f))))
      : clampMetric(rawAsc,
                    std::max(1, int(std::lround(nominal * .60f))),
                    std::max(1, int(std::lround(nominal * 1.10f))));
  int descMag = std::max(0, -rawDesc);
  if (refValid) descMag = std::max(descMag, refBottom);
  descMag = clampMetric(descMag, 0, std::max(1, int(std::lround(nominal * .30f))));
  const int descPx = -descMag;
  const int clippedGap = std::min(gapPx, std::max(0, int(std::lround(nominal * .25f))));
  int line = std::max(ascPx + descMag + clippedGap, nominal);
  line = std::min(line, std::max(nominal, int(std::lround(nominal * 1.35f))));
  line = std::max(1, std::min(255, line));

  data_.bitmap = nullptr;
  data_.glyph = nullptr;
  data_.intervals = nullptr;
  data_.intervalCount = 0;
  data_.advanceY = uint8_t(line);
  data_.ascender = ascPx;
  data_.descender = descPx;
  data_.is2Bit = true;
  valid_ = true;

  Serial.printf("[TTF] Loaded ptr=%p %s backend=%s face=%lu nominal=%upx raster=%upx upm=%u lineH=%u asc=%d desc=%d bboxTop=%d ref=U+%04X bearing=native slots=%u budget=%u\n",
                static_cast<void*>(this), label ? label : "?", backendName(),
                static_cast<unsigned long>(faceOffset_), sizePx_, renderSizePx_, upm,
                data_.advanceY, data_.ascender, data_.descender, bboxTop,
                static_cast<unsigned>(visualReferenceCodepoint_),
                static_cast<unsigned>(maxSlots_), static_cast<unsigned>(cacheBudget_));
  return true;
}

TtfEpdFont::TtfEpdFont(const String& path, uint16_t sizePx,
                       uint16_t maxSlots, size_t budget)
    : EpdFont(&data_), path_(path), sizePx_(sizePx),
      maxSlots_(maxSlots), cacheBudget_(budget) {
#if defined(ESP32)
  mutex_ = xSemaphoreCreateMutex();
#endif

  auto* sd = new (std::nothrow) SdTtfStream();
  if (!sd || !sd->open(path_)) {
    runtimeError_ = "font file open failed";
    delete sd;
    return;
  }

  uint8_t magic[4] = {};
  const bool haveMagic = sd->seek(0) && sd->read(magic, sizeof(magic)) == sizeof(magic);
  if (haveMagic && std::memcmp(magic, "ttcf", 4) == 0) {
    OpenTypeKind kind = OpenTypeKind::Invalid;
    bool sawCff2 = false;
    if (!probeCollection(*sd, faceOffset_, kind, sawCff2)) {
      runtimeError_ = sawCff2
          ? "font collection contains no usable CFF2 default face"
          : "font collection contains no supported glyf/CFF face";
      delete sd;
      return;
    }
    stream_ = sd;
    backend_ = kind == OpenTypeKind::Cff2 ? Backend::Cff2
             : kind == OpenTypeKind::Cff1 ? Backend::Cff1
                                          : Backend::Glyf;
    char diag[96];
    snprintf(diag, sizeof(diag), "collection_%s_zero_copy enabled", kindName(kind));
    m4AppendFontDiagnostic(diag);
  } else if (haveMagic && std::memcmp(magic, "OTTO", 4) == 0) {
    const OpenTypeKind kind = probeOpenType(*sd);
    if (kind == OpenTypeKind::Cff1 || kind == OpenTypeKind::Cff2 || kind == OpenTypeKind::Glyf) {
      stream_ = sd;
      faceOffset_ = 0;
      backend_ = kind == OpenTypeKind::Cff2 ? Backend::Cff2
               : kind == OpenTypeKind::Cff1 ? Backend::Cff1
                                            : Backend::Glyf;
      char diag[96];
      snprintf(diag, sizeof(diag), "opentype_%s_backend enabled", kindName(kind));
      m4AppendFontDiagnostic(diag);
    } else {
      runtimeError_ = "invalid OpenType font";
      delete sd;
      return;
    }
  } else {
    sd->seek(0);
    stream_ = sd;
    faceOffset_ = 0;
    backend_ = Backend::Glyf;
  }

  if (!finishInit(path_.c_str())) return;
  if (!allocateEntries()) valid_ = false;
}

TtfEpdFont::TtfEpdFont(const uint8_t* data, uint32_t dataSize,
                       uint16_t sizePx, uint16_t maxSlots, size_t budget)
    : EpdFont(&data_), path_("<flash>"), sizePx_(sizePx),
      maxSlots_(maxSlots), cacheBudget_(budget) {
#if defined(ESP32)
  mutex_ = xSemaphoreCreateMutex();
#endif
  if (!data || !dataSize) {
    runtimeError_ = "empty embedded TTF";
    return;
  }
  stream_ = new (std::nothrow) MemoryTtfStream(data, dataSize);
  backend_ = Backend::Glyf;
  faceOffset_ = 0;
  if (!stream_) {
    runtimeError_ = "embedded font stream allocation failed";
    return;
  }
  if (!finishInit("<flash>")) return;
  if (!allocateEntries()) valid_ = false;
}

TtfEpdFont::~TtfEpdFont() {
  clearCaches();
  if (entries_) {
    for (uint16_t i = 0; i < maxSlots_; ++i) entries_[i].~Entry();
    ttfFree(entries_);
    entries_ = nullptr;
  }
  delete stream_;
  stream_ = nullptr;
#if defined(ESP32)
  if (mutex_) {
    vSemaphoreDelete(mutex_);
    mutex_ = nullptr;
  }
#endif
}

void TtfEpdFont::evictSlot(int slot) const {
  if (!entries_ || slot < 0 || slot >= int(maxSlots_)) return;
  if (entries_[slot].bitmap) {
    ttfFree(entries_[slot].bitmap);
    cacheBytes_ -= entries_[slot].bitmapSize;
  }
  entries_[slot] = Entry{};
}

void TtfEpdFont::clearCaches() {
#if defined(ESP32)
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
#endif
  if (entries_) {
    for (uint16_t i = 0; i < maxSlots_; ++i) evictSlot(i);
  }
  backendClearScratch();
#if defined(ESP32)
  if (mutex_) xSemaphoreGive(mutex_);
#endif
}

int TtfEpdFont::ensureGlyph(uint32_t cp) const {
  if (!valid_ || !entries_) return -1;
  for (uint16_t i = 0; i < maxSlots_; ++i) {
    if (entries_[i].cp == cp) {
      entries_[i].lastAccess = ++accessCounter_;
      return i;
    }
  }

  // Spaces and controls: synthesize empty glyph, never fallback to '?'.
  // Prefer real hmtx advance for proportional layout; fallback to em/3 only if metrics unavailable.
  if (cp == 0x20 || cp == 0x3000 || cp == 0x00A0 || cp == 0x09 || cp == 0x0A || cp == 0x0D) {
    int slot = -1;
    uint32_t minAccess = 0xffffffffu;
    for (uint16_t i = 0; i < maxSlots_; ++i) {
      if (entries_[i].cp == 0xffffffffu) { slot = i; break; }
      if (entries_[i].lastAccess < minAccess) { minAccess = entries_[i].lastAccess; slot = i; }
    }
    if (slot < 0) return -1;
    evictSlot(slot);
    int adv = 0;
    uint16_t gid = 0;
    int32_t advUnits = 0, lsb = 0;
    if (backendFindGlyph(cp, gid) && gid != 0 && backendHMetrics(gid, advUnits, lsb)) {
      const uint16_t upm = backendUnitsPerEm();
      if (upm) adv = int(std::lround(float(advUnits) * float(sizePx_) / float(upm)));
    }
    if (adv <= 0) adv = std::max(1, int(sizePx_) / 3);
    adv = std::max(1, std::min(255, adv));
    entries_[slot].cp = cp;
    entries_[slot].lastAccess = ++accessCounter_;
    entries_[slot].glyph.width = 0;
    entries_[slot].glyph.height = 0;
    entries_[slot].glyph.advanceX = uint8_t(adv);
    entries_[slot].glyph.left = 0;
    entries_[slot].glyph.top = 0;
    entries_[slot].glyph.dataLength = 0;
    entries_[slot].glyph.dataOffset = cp;
    entries_[slot].bitmap = nullptr;
    entries_[slot].bitmapSize = 0;
    return slot;
  }

  uint16_t gid = 0;
  if (!backendFindGlyph(cp, gid)) return -1;
  if (gid == 0 && cp != '?') return ensureGlyph('?');

  ttf::GlyphBitmap gb;
  if (!backendRasterize(gid, gb)) {
    char line[120];
    snprintf(line, sizeof(line), "glyph_raster_fail cp=U+%04lX gid=%u err=%s",
             static_cast<unsigned long>(cp), static_cast<unsigned>(gid), backendError());
    m4AppendFontDiagnostic(line);
    if (gid == 0 || !backendRasterize(0, gb)) return -1;
  }
  // Owner-loop TTF first-paint can take hundreds of ms per CJK glyph on QEMU.
  // Yield so m4adb tap/key is ACKed instead of looking frozen until the page
  // finishes. Weak no-op on host tests; firmware overrides on the main task.
  if (m4YieldToDebugBridge) m4YieldToDebugBridge();

  int slot = -1;
  uint32_t minAccess = 0xffffffffu;
  for (uint16_t i = 0; i < maxSlots_; ++i) {
    if (entries_[i].cp == 0xffffffffu) {
      slot = i;
      break;
    }
    if (entries_[i].lastAccess < minAccess) {
      minAccess = entries_[i].lastAccess;
      slot = i;
    }
  }
  if (slot < 0) return -1;
  evictSlot(slot);

  uint8_t* bitmap = nullptr;
  const uint32_t len = gb.packedLen;
  if (len && gb.data) {
    bitmap = static_cast<uint8_t*>(ttfAlloc(len));
    if (!bitmap) return -1;
    std::memcpy(bitmap, gb.data, len);
  }

  // Resolve hmtx before publishing this cache slot. lookupAdvancePx() first
  // checks resident entries; publishing cp while advanceX is still zero makes
  // it read this half-built entry and collapses every rendered glyph onto one x.
  const int advance = std::max(0, std::min(255, lookupAdvancePx(cp)));
  entries_[slot].cp = cp;
  entries_[slot].lastAccess = ++accessCounter_;
  entries_[slot].glyph.width = uint8_t(gb.width);
  entries_[slot].glyph.height = uint8_t(gb.height);
  // Advances and bearings both stay on the source font metrics at the
  // configured reader px.
  entries_[slot].glyph.advanceX = static_cast<uint8_t>(advance);
  entries_[slot].glyph.left = gb.xoff;
  entries_[slot].glyph.top = gb.yoff;
  entries_[slot].glyph.dataLength = len;
  entries_[slot].glyph.dataOffset = cp;
  entries_[slot].bitmap = bitmap;
  entries_[slot].bitmapSize = len;
  cacheBytes_ += len;

  if (glyphDiagnosticsLogged_ < 12) {
    Serial.printf("[TTF-GLYPH] ptr=%p cp=U+%04lX advance=%d bitmap=%ux%u left=%d top=%d nominal=%u raster=%u lineH=%u asc=%d desc=%d\n",
                  static_cast<const void*>(this), static_cast<unsigned long>(cp), advance,
                  static_cast<unsigned>(gb.width), static_cast<unsigned>(gb.height),
                  static_cast<int>(entries_[slot].glyph.left), static_cast<int>(gb.yoff),
                  sizePx_, renderSizePx_, data_.advanceY, data_.ascender, data_.descender);
    ++glyphDiagnosticsLogged_;
  }

  while (cacheBytes_ > cacheBudget_) {
    int victim = -1;
    uint32_t leastAccess = 0xffffffffu;
    for (uint16_t i = 0; i < maxSlots_; ++i) {
      if (int(i) == slot || entries_[i].cp == 0xffffffffu) continue;
      if (entries_[i].lastAccess < leastAccess) {
        leastAccess = entries_[i].lastAccess;
        victim = i;
      }
    }
    if (victim < 0) break;
    evictSlot(victim);
  }
  return slot;
}

const EpdGlyph* TtfEpdFont::getGlyph(uint32_t cp,
                                    const EpdFontStyles::Style style) const {
  (void)style;
#if defined(ESP32)
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
#endif
  const int slot = ensureGlyph(cp);
#if defined(ESP32)
  if (mutex_) xSemaphoreGive(mutex_);
#endif
  return slot < 0 ? nullptr : &entries_[slot].glyph;
}

const uint8_t* TtfEpdFont::loadGlyphBitmap(const EpdGlyph* glyph, uint8_t* buffer,
                                           const EpdFontStyles::Style style) const {
  (void)style;
  if (!glyph) return nullptr;
  // Empty glyphs (spaces) have zero dataLength - caller should treat as whitespace, not missing.
  if (glyph->dataLength == 0 && glyph->width == 0 && glyph->height == 0) {
    // Return non-null sentinel for explicit spaces so renderer does not fallback to '?'.
    static const uint8_t kEmptySentinel = 0;
    if (glyph->dataOffset == 0x20 || glyph->dataOffset == 0x3000 || glyph->dataOffset == 0x00A0) {
      return &kEmptySentinel;
    }
    return nullptr;
  }
  const uint32_t cp = glyph->dataOffset;
#if defined(ESP32)
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
#endif
  const int slot = ensureGlyph(cp);
  const uint8_t* result = nullptr;
  if (slot >= 0 && entries_[slot].bitmap && entries_[slot].bitmapSize) {
    if (buffer) {
      std::memcpy(buffer, entries_[slot].bitmap, entries_[slot].bitmapSize);
      result = buffer;
    } else {
      result = entries_[slot].bitmap;
    }
  }
#if defined(ESP32)
  if (mutex_) xSemaphoreGive(mutex_);
#endif
  return result;
}
