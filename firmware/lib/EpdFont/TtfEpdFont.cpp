#include "TtfEpdFont.h"

#include <HardwareSerial.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <vector>

extern void m4AppendFontDiagnostic(const char* line);

namespace {

uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
void putBe32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}
uint32_t align4(uint32_t v) { return (v + 3u) & ~3u; }

class SdTtfStream : public ttf::TtfStream {
 public:
  bool open(const String& path) {
    close();
    traceOps_ = 0;
    const bool ok = SdMan.openFileForRead("TtfFont", path.c_str(), file_);
    char line[256];
    snprintf(line, sizeof(line), "stream_open path=%s ok=%d size=%llu", path.c_str(), ok ? 1 : 0,
             static_cast<unsigned long long>(ok ? file_.fileSize() : 0));
    m4AppendFontDiagnostic(line);
    return ok;
  }
  void close() {
    if (file_.isOpen()) file_.close();
  }
  bool isOpen() const { return file_.isOpen(); }
  uint32_t size() const override { return file_.isOpen() ? file_.fileSize() : 0; }

  bool seek(uint32_t pos) override {
    bool ok = file_.isOpen() && file_.seekSet(pos);
    for (uint8_t attempt = 0; !ok && attempt < 3 && file_.isOpen(); ++attempt) {
      delay(2);
      ok = file_.seekSet(pos);
    }
    bool sequentialFallback = false;
    if (!ok && file_.isOpen() && pos <= file_.fileSize()) {
      file_.rewind();
      uint8_t discard[512];
      uint32_t remaining = pos;
      while (remaining > 0) {
        const uint32_t want = std::min<uint32_t>(remaining, sizeof(discard));
        const int got = file_.read(discard, want);
        if (got <= 0) break;
        remaining -= static_cast<uint32_t>(got);
      }
      ok = remaining == 0;
      sequentialFallback = ok;
    }
    if (traceOps_ < 100) {
      char line[200];
      snprintf(line, sizeof(line), "stream_seek pos=%lu ok=%d seq=%d cur=%llu size=%llu err=%u",
               static_cast<unsigned long>(pos), ok ? 1 : 0, sequentialFallback ? 1 : 0,
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
        delay(2);
        got = file_.read(out + total, n - total);
      }
      if (got <= 0) break;
      total += static_cast<uint32_t>(got);
    }
    if (traceOps_ < 100) {
      char line[180];
      snprintf(line, sizeof(line), "stream_read pos=%llu want=%lu got=%lu end=%llu",
               static_cast<unsigned long long>(start), static_cast<unsigned long>(n),
               static_cast<unsigned long>(total), static_cast<unsigned long long>(file_.curPosition()));
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

// Presents one glyf-based face from a TTC as a normal standalone sfnt stream.
// TTC table offsets are absolute in the collection. We keep all table payloads
// on SD and synthesize only the small offset-table/directory in RAM, so a large
// CJK collection never has to fit in PSRAM.
class TtcFaceStream : public ttf::TtfStream {
 public:
  explicit TtcFaceStream(SdTtfStream* source) : source_(source) {}
  ~TtcFaceStream() override { delete source_; }

  bool init() {
    if (!source_ || source_->size() < 16) return false;
    uint8_t hdr[12];
    if (!readSource(0, hdr, sizeof(hdr)) || std::memcmp(hdr, "ttcf", 4) != 0) return false;
    const uint32_t count = be32(hdr + 8);
    if (count == 0 || count > 64 || 12u + count * 4u > source_->size()) return false;
    for (uint32_t i = 0; i < count; ++i) {
      uint8_t offBuf[4];
      if (!readSource(12u + i * 4u, offBuf, 4)) return false;
      if (buildFace(be32(offBuf), i)) {
        char line[160];
        snprintf(line, sizeof(line), "ttc_face_selected index=%lu faces=%lu virtual_size=%lu",
                 static_cast<unsigned long>(i), static_cast<unsigned long>(count),
                 static_cast<unsigned long>(size_));
        m4AppendFontDiagnostic(line);
        return true;
      }
    }
    return false;
  }

  uint32_t size() const override { return size_; }
  bool seek(uint32_t pos) override {
    if (pos > size_) return false;
    pos_ = pos;
    return true;
  }

  uint32_t read(void* dst, uint32_t n) override {
    if (!dst || pos_ >= size_) return 0;
    uint8_t* out = static_cast<uint8_t*>(dst);
    uint32_t total = 0;
    n = std::min(n, size_ - pos_);
    while (total < n) {
      if (pos_ < directory_.size()) {
        const uint32_t take = std::min<uint32_t>(n - total, static_cast<uint32_t>(directory_.size()) - pos_);
        std::memcpy(out + total, directory_.data() + pos_, take);
        pos_ += take;
        total += take;
        continue;
      }

      const TableMap* hit = nullptr;
      uint32_t next = size_;
      for (const auto& t : tables_) {
        if (pos_ >= t.virtualOff && pos_ < t.virtualOff + t.len) {
          hit = &t;
          break;
        }
        if (t.virtualOff > pos_) next = std::min(next, t.virtualOff);
      }
      if (hit) {
        const uint32_t delta = pos_ - hit->virtualOff;
        const uint32_t take = std::min<uint32_t>(n - total, hit->len - delta);
        if (!source_->seek(hit->sourceOff + delta)) break;
        const uint32_t got = source_->read(out + total, take);
        if (!got) break;
        pos_ += got;
        total += got;
        continue;
      }

      const uint32_t take = std::min<uint32_t>(n - total, next > pos_ ? next - pos_ : 1u);
      std::memset(out + total, 0, take);
      pos_ += take;
      total += take;
    }
    return total;
  }

 private:
  struct TableMap { uint32_t virtualOff = 0, sourceOff = 0, len = 0; };

  bool readSource(uint32_t off, void* dst, uint32_t n) {
    return off <= source_->size() && n <= source_->size() - off && source_->seek(off) && source_->read(dst, n) == n;
  }

  bool buildFace(uint32_t faceOff, uint32_t faceIndex) {
    (void)faceIndex;
    if (faceOff > source_->size() || source_->size() - faceOff < 12) return false;
    uint8_t faceHdr[12];
    if (!readSource(faceOff, faceHdr, 12)) return false;
    const uint32_t signature = be32(faceHdr);
    if (signature != 0x00010000u && signature != 0x74727565u) return false;
    const uint16_t numTables = be16(faceHdr + 4);
    if (!numTables || numTables > 64) return false;
    const uint32_t dirBytes = 12u + static_cast<uint32_t>(numTables) * 16u;
    if (faceOff > source_->size() || dirBytes > source_->size() - faceOff) return false;

    std::vector<uint8_t> dir(dirBytes);
    if (!readSource(faceOff, dir.data(), dirBytes)) return false;
    std::vector<TableMap> maps;
    maps.reserve(numTables);
    bool head = false, maxp = false, loca = false, cmap = false;
    bool hhea = false, hmtx = false, glyf = false;
    uint32_t virtualOff = align4(dirBytes);

    for (uint16_t i = 0; i < numTables; ++i) {
      uint8_t* rec = dir.data() + 12u + static_cast<uint32_t>(i) * 16u;
      const uint32_t tag = be32(rec);
      const uint32_t sourceOff = be32(rec + 8);
      const uint32_t len = be32(rec + 12);
      if (sourceOff > source_->size() || len > source_->size() - sourceOff) return false;
      if (len > 0 && virtualOff > 0xffffffffu - align4(len)) return false;
      maps.push_back({virtualOff, sourceOff, len});
      putBe32(rec + 8, virtualOff);
      virtualOff += align4(len);
      head |= tag == 0x68656164u;
      maxp |= tag == 0x6d617870u;
      loca |= tag == 0x6c6f6361u;
      cmap |= tag == 0x636d6170u;
      hhea |= tag == 0x68686561u;
      hmtx |= tag == 0x686d7478u;
      glyf |= tag == 0x676c7966u;
    }
    if (!(head && maxp && loca && cmap && hhea && hmtx && glyf)) return false;
    directory_.swap(dir);
    tables_.swap(maps);
    size_ = virtualOff;
    pos_ = 0;
    return true;
  }

  SdTtfStream* source_ = nullptr;
  std::vector<uint8_t> directory_;
  std::vector<TableMap> tables_;
  uint32_t size_ = 0;
  uint32_t pos_ = 0;
};

void* ttfAlloc(size_t n) {
#if defined(ESP32) && defined(BOARD_HAS_PSRAM)
  if (psramFound()) return ps_malloc(n);
#endif
  return malloc(n);
}
void ttfFree(void* p) { free(p); }

struct VisualReference {
  bool valid = false;
  uint32_t cp = 0;
  int topPx = 0;
  int bottomPx = 0;
};

VisualReference findVisualReference(const ttf::TtfFont& font, uint16_t sizePx) {
  static constexpr uint32_t kSamples[] = {0x56FD, 0x7530, 0x4E2D, 0x6C38, 0x4E00, 'H', 'M'};
  for (uint32_t cp : kSamples) {
    uint16_t gid = 0;
    if (!font.findGlyph(cp, gid) || gid == 0) continue;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (!font.glyphPixelBox(gid, sizePx, x0, y0, x1, y1)) continue;
    const int top = std::max(0, -y0);
    const int bottom = std::max(0, y1);
    const int height = top + bottom;
    if (height < std::max(2, static_cast<int>(sizePx) / 2) || height > 255) continue;
    return {true, cp, top, bottom};
  }
  return {};
}

int clampMetric(int value, int low, int high) {
  if (low > high) std::swap(low, high);
  return std::max(low, std::min(high, value));
}

}  // namespace

void* TtfEpdFont::ttfAlloc(size_t n) { return ::ttfAlloc(n); }
void TtfEpdFont::ttfFree(void* p) { ::ttfFree(p); }

bool TtfEpdFont::allocateEntries() {
  if (maxSlots_ == 0) maxSlots_ = 1;
  if (cacheBudget_ == 0) cacheBudget_ = 1;
  entries_ = static_cast<Entry*>(ttfAlloc(sizeof(Entry) * maxSlots_));
  if (!entries_) {
    Serial.printf("[TTF] Failed to allocate glyph metadata slots=%u bytes=%u\n",
                  static_cast<unsigned>(maxSlots_), static_cast<unsigned>(sizeof(Entry) * maxSlots_));
    return false;
  }
  for (uint16_t i = 0; i < maxSlots_; ++i) new (&entries_[i]) Entry();
  return true;
}

bool TtfEpdFont::finishInit(const char* sourceLabel) {
  if (!stream_) return false;
  if (!font_.init(*stream_)) {
    Serial.printf("[TTF] Invalid TTF %s: %s\n", sourceLabel ? sourceLabel : "?", font_.lastError());
    return false;
  }
  const float scale = static_cast<float>(sizePx_) / static_cast<float>(font_.unitsPerEm());
  int32_t asc = 0, desc = 0, gap = 0;
  font_.fontVMetrics(asc, desc, gap);
  const int hheaAscPx = static_cast<int>(std::lround(asc * scale));
  const int hheaDescPx = static_cast<int>(std::lround(desc * scale));
  const int hheaGapPx = std::max(0, static_cast<int>(std::lround(gap * scale)));
  const int bboxTopPx = std::max(0, static_cast<int>(std::lround(font_.fontBBoxYMax() * scale)));
  const VisualReference ref = findVisualReference(font_, sizePx_);
  const int nominal = std::max(1, static_cast<int>(sizePx_));

  int ascPx = hheaAscPx;
  if (ref.valid) {
    ascPx = clampMetric(ref.topPx, std::max(1, static_cast<int>(std::lround(nominal * 0.55f))),
                        std::max(1, static_cast<int>(std::lround(nominal * 1.10f))));
  } else {
    ascPx = clampMetric(hheaAscPx, std::max(1, static_cast<int>(std::lround(nominal * 0.60f))),
                        std::max(1, static_cast<int>(std::lround(nominal * 1.10f))));
  }
  int descMag = std::max(0, -hheaDescPx);
  if (ref.valid) descMag = std::max(descMag, ref.bottomPx);
  descMag = clampMetric(descMag, 0, std::max(1, static_cast<int>(std::lround(nominal * 0.30f))));
  const int descPx = -descMag;
  const int gapPx = std::min(hheaGapPx, std::max(0, static_cast<int>(std::lround(nominal * 0.25f))));
  int linePx = std::max(ascPx + descMag + gapPx, nominal);
  linePx = std::min(linePx, std::max(nominal, static_cast<int>(std::lround(nominal * 1.35f))));
  linePx = std::max(1, std::min(255, linePx));

  data_.bitmap = nullptr;
  data_.glyph = nullptr;
  data_.intervals = nullptr;
  data_.intervalCount = 0;
  data_.advanceY = static_cast<uint8_t>(linePx);
  data_.ascender = ascPx;
  data_.descender = descPx;
  data_.is2Bit = true;
  valid_ = true;
  Serial.printf("[TTF] Loaded %s @%upx (unitsPerEm=%u lineH=%u asc=%d desc=%d rawAsc=%d rawDesc=%d bboxTop=%d ref=U+%04X refTop=%d refBottom=%d slots=%u meta=%u budget=%u)\n",
                sourceLabel ? sourceLabel : "?", sizePx_, font_.unitsPerEm(), data_.advanceY, data_.ascender,
                data_.descender, hheaAscPx, hheaDescPx, bboxTopPx,
                static_cast<unsigned>(ref.valid ? ref.cp : 0), ref.topPx, ref.bottomPx,
                static_cast<unsigned>(maxSlots_), static_cast<unsigned>(sizeof(Entry) * maxSlots_),
                static_cast<unsigned>(cacheBudget_));
  return true;
}

TtfEpdFont::TtfEpdFont(const String& path, uint16_t sizePx, uint16_t maxSlots, size_t cacheBudget)
    : EpdFont(&data_), path_(path), sizePx_(sizePx), maxSlots_(maxSlots), cacheBudget_(cacheBudget) {
#if defined(ESP32)
  mutex_ = xSemaphoreCreateMutex();
#endif
  if (!allocateEntries()) return;
  auto* sd = new (std::nothrow) SdTtfStream();
  if (!sd || !sd->open(path_)) {
    Serial.printf("[TTF] Failed to open %s\n", path_.c_str());
    delete sd;
    return;
  }

  uint8_t magic[4] = {};
  const bool haveMagic = sd->seek(0) && sd->read(magic, 4) == 4;
  if (haveMagic && std::memcmp(magic, "ttcf", 4) == 0) {
    auto* collection = new (std::nothrow) TtcFaceStream(sd);
    if (!collection || !collection->init()) {
      Serial.printf("[TTF] TTC has no supported glyf face: %s\n", path_.c_str());
      delete collection;  // also owns/deletes sd when allocated
      if (!collection) delete sd;
      return;
    }
    stream_ = collection;
  } else {
    sd->seek(0);
    stream_ = sd;
  }
  finishInit(path_.c_str());
}

TtfEpdFont::TtfEpdFont(const uint8_t* data, uint32_t dataSize, uint16_t sizePx, uint16_t maxSlots,
                       size_t cacheBudget)
    : EpdFont(&data_), path_("<flash>"), sizePx_(sizePx), maxSlots_(maxSlots), cacheBudget_(cacheBudget) {
#if defined(ESP32)
  mutex_ = xSemaphoreCreateMutex();
#endif
  if (!data || dataSize == 0) {
    Serial.printf("[TTF] Empty embedded TTF\n");
    return;
  }
  if (!allocateEntries()) return;
  stream_ = new (std::nothrow) MemoryTtfStream(data, dataSize);
  finishInit("<flash>");
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
  if (!entries_ || slot < 0 || slot >= static_cast<int>(maxSlots_)) return;
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
  if (entries_) for (uint16_t i = 0; i < maxSlots_; ++i) evictSlot(static_cast<int>(i));
  font_.clearScratch();
#if defined(ESP32)
  if (mutex_) xSemaphoreGive(mutex_);
#endif
}

int TtfEpdFont::ensureGlyph(uint32_t cp) const {
  if (!valid_ || !entries_) return -1;
  for (uint16_t i = 0; i < maxSlots_; ++i) {
    if (entries_[i].cp == cp) {
      entries_[i].lastAccess = ++accessCounter_;
      return static_cast<int>(i);
    }
  }
  uint16_t gid = 0;
  if (!font_.findGlyph(cp, gid)) return -1;
  if (gid == 0 && cp != '?') return ensureGlyph('?');
  ttf::GlyphBitmap gb;
  if (!font_.rasterize(gid, sizePx_, gb)) {
    if (gid != 0 && !font_.rasterize(0, sizePx_, gb)) return -1;
  }

  int slot = -1;
  uint32_t minAccess = 0xffffffffu;
  for (uint16_t i = 0; i < maxSlots_; ++i) {
    if (entries_[i].cp == 0xffffffffu) { slot = static_cast<int>(i); break; }
    if (entries_[i].lastAccess < minAccess) {
      minAccess = entries_[i].lastAccess;
      slot = static_cast<int>(i);
    }
  }
  if (slot < 0) return -1;
  evictSlot(slot);

  uint8_t* bmp = nullptr;
  const uint32_t packedLen = gb.packedLen;
  if (packedLen > 0 && gb.data) {
    bmp = static_cast<uint8_t*>(ttfAlloc(packedLen));
    if (bmp) std::memcpy(bmp, gb.data, packedLen);
    else Serial.printf("[TTF] glyph alloc %u failed (OOM)\n", packedLen);
  }
  entries_[slot].cp = cp;
  entries_[slot].lastAccess = ++accessCounter_;
  entries_[slot].glyph.width = static_cast<uint8_t>(gb.width);
  entries_[slot].glyph.height = static_cast<uint8_t>(gb.height);
  entries_[slot].glyph.advanceX = static_cast<uint8_t>(std::max(0, std::min(255, static_cast<int>(gb.advance))));
  entries_[slot].glyph.left = gb.xoff;
  entries_[slot].glyph.top = gb.yoff;
  entries_[slot].glyph.dataLength = packedLen;
  entries_[slot].glyph.dataOffset = cp;
  entries_[slot].bitmap = bmp;
  entries_[slot].bitmapSize = packedLen;
  cacheBytes_ += packedLen;

  if (cacheBytes_ > cacheBudget_) {
    for (uint16_t pass = 0; pass < maxSlots_ && cacheBytes_ > cacheBudget_; ++pass) {
      int victim = -1;
      uint32_t la = 0xffffffffu;
      for (uint16_t i = 0; i < maxSlots_; ++i) {
        if (static_cast<int>(i) == slot || entries_[i].cp == 0xffffffffu) continue;
        if (entries_[i].lastAccess < la) { la = entries_[i].lastAccess; victim = static_cast<int>(i); }
      }
      if (victim < 0) break;
      evictSlot(victim);
    }
  }
  return slot;
}

const EpdGlyph* TtfEpdFont::getGlyph(uint32_t cp, const EpdFontStyles::Style style) const {
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
  const uint32_t cp = glyph->dataOffset;
#if defined(ESP32)
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
#endif
  const int slot = ensureGlyph(cp);
  const uint8_t* result = nullptr;
  if (slot >= 0 && entries_[slot].bitmap && entries_[slot].bitmapSize > 0) {
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
