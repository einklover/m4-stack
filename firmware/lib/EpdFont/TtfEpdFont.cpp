#include "TtfEpdFont.h"

#include <HardwareSerial.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

// Implemented by the firmware-side FontManager. Keeping this as a tiny
// bridge avoids making the reusable EpdFont library depend on src/ headers.
extern void m4AppendFontDiagnostic(const char* line);

namespace {

// FsFile-backed seekable stream for normal user fonts on SD.
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
      // Some SDMMC/exFAT cards fail the FAT-chain walk for a large random
      // seek even though sequential reads are healthy. TTF tables are
      // scattered, so retry from the beginning by consuming the file. This
      // is slower only on the failing path and keeps the reader functional.
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
    // SdFat explicitly permits short reads. TtfReader::readAt() expects the
    // stream to fill the requested slice unless it reaches EOF, so keep
    // reading instead of turning a normal SD short read into a fake
    // "failed to read head table" font-load failure.
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

// Borrowed memory-backed stream for an embedded emergency/UI subset. ESP32-S3
// flash is memory mapped, so memcpy reads the const array without copying the
// font into heap/PSRAM. The array must outlive this stream (normally static).
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
    const uint32_t remaining = size_ - pos_;
    const uint32_t take = std::min(n, remaining);
    if (take > 0) {
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

void* ttfAlloc(size_t n) {
#if defined(ESP32) && defined(BOARD_HAS_PSRAM)
  if (psramFound()) return ps_malloc(n);
#endif
  return malloc(n);
}

void ttfFree(void* p) { free(p); }

// TrueType's hhea/head vertical metrics are not visually comparable across
// families. Some CJK fonts put rare accents/symbols near head.yMax, while the
// normal ideograph box sits much lower. GfxRenderer treats EpdFontData::ascender
// as the distance from the requested top-left y to the baseline; therefore
// feeding head.yMax into ascender shifts ordinary text downward and also makes
// UI scale calculations shrink the selected TTF too aggressively.
//
// Pick one representative glyph once at face initialization and use its actual
// raster-space box as the visual baseline reference. The preference order uses
// square/common CJK ideographs first, then Latin capitals for non-CJK fonts.
struct VisualReference {
  bool valid = false;
  uint32_t cp = 0;
  int topPx = 0;     // baseline -> visible top, positive
  int bottomPx = 0;  // baseline -> visible bottom, positive/downward
};

VisualReference findVisualReference(const ttf::TtfFont& font, uint16_t sizePx) {
  static constexpr uint32_t kSamples[] = {
      0x56FD,  // 国: outer square gives a stable CJK em-box sample
      0x7530,  // 田
      0x4E2D,  // 中
      0x6C38,  // 永
      0x4E00,  // 一
      'H',
      'M',
  };

  for (uint32_t cp : kSamples) {
    uint16_t gid = 0;
    if (!font.findGlyph(cp, gid) || gid == 0) continue;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (!font.glyphPixelBox(gid, sizePx, x0, y0, x1, y1)) continue;
    const int top = std::max(0, -y0);
    const int bottom = std::max(0, y1);
    const int height = top + bottom;
    // Reject pathological/empty samples. A useful reference should occupy at
    // least half of the nominal em but must still fit EpdGlyph's 8-bit box.
    if (height < std::max(2, static_cast<int>(sizePx) / 2) || height > 255) continue;
    VisualReference ref;
    ref.valid = true;
    ref.cp = cp;
    ref.topPx = top;
    ref.bottomPx = bottom;
    return ref;
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

  // Keep lookup/LRU metadata off the scarce internal heap. Runtime reader
  // faces normally use 512 entries; a tiny embedded face can use ~96.
  entries_ = static_cast<Entry*>(ttfAlloc(sizeof(Entry) * maxSlots_));
  if (!entries_) {
    Serial.printf("[TTF] Failed to allocate glyph metadata slots=%u bytes=%u\n",
                  static_cast<unsigned>(maxSlots_),
                  static_cast<unsigned>(sizeof(Entry) * maxSlots_));
    return false;
  }
  for (uint16_t i = 0; i < maxSlots_; ++i) {
    new (&entries_[i]) Entry();
  }
  return true;
}

bool TtfEpdFont::finishInit(const char* sourceLabel) {
  if (!stream_) return false;
  if (!font_.init(*stream_)) {
    Serial.printf("[TTF] Invalid TTF %s: %s\n", sourceLabel ? sourceLabel : "?", font_.lastError());
    return false;
  }

  // Raster size still follows the requested nominal pixel size exactly. Only
  // the logical line metrics are normalized so changing TTF family changes the
  // glyph design, not the apparent top/baseline position of the whole UI/page.
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
    // The representative outline is the baseline anchor. Keep only generous
    // sanity bounds; do not force every typeface to an identical CJK drawing.
    // This removes extreme head/hhea metrics while preserving deliberate style.
    ascPx = clampMetric(ref.topPx,
                        std::max(1, static_cast<int>(std::lround(nominal * 0.55f))),
                        std::max(1, static_cast<int>(std::lround(nominal * 1.10f))));
  } else {
    // Non-CJK/pathological fallback: hhea is still preferable to head.yMax, but
    // bound it around the nominal em so an exotic font cannot move all text.
    ascPx = clampMetric(hheaAscPx,
                        std::max(1, static_cast<int>(std::lround(nominal * 0.60f))),
                        std::max(1, static_cast<int>(std::lround(nominal * 1.10f))));
  }

  // Preserve descender room for Latin punctuation/g/p/y, but cap extreme font
  // metrics. The representative CJK bottom may be zero and is not sufficient
  // on its own for mixed Chinese/Latin text.
  int descMag = std::max(0, -hheaDescPx);
  if (ref.valid) descMag = std::max(descMag, ref.bottomPx);
  descMag = clampMetric(descMag, 0,
                        std::max(1, static_cast<int>(std::lround(nominal * 0.30f))));
  const int descPx = -descMag;

  // Line spacing is a layout metric, not a license for a font to request a
  // huge box. Keep the font's positive lineGap when reasonable and otherwise
  // normalize to a compact reader-friendly band around the requested size.
  const int gapPx = std::min(hheaGapPx,
                             std::max(0, static_cast<int>(std::lround(nominal * 0.25f))));
  int linePx = ascPx + descMag + gapPx;
  linePx = std::max(linePx, nominal);
  linePx = std::min(linePx,
                    std::max(nominal, static_cast<int>(std::lround(nominal * 1.35f))));
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
  Serial.printf(
      "[TTF] Loaded %s @%upx (unitsPerEm=%u lineH=%u asc=%d desc=%d rawAsc=%d rawDesc=%d bboxTop=%d ref=U+%04X refTop=%d refBottom=%d slots=%u meta=%u budget=%u)\n",
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

  auto* s = new SdTtfStream();
  stream_ = s;
  if (!s->open(path_)) {
    Serial.printf("[TTF] Failed to open %s\n", path_.c_str());
    delete s;
    stream_ = nullptr;
    return;
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

  stream_ = new MemoryTtfStream(data, dataSize);
  finishInit("<flash>");
}

TtfEpdFont::~TtfEpdFont() {
  clearCaches();
  if (entries_) {
    for (uint16_t i = 0; i < maxSlots_; ++i) {
      entries_[i].~Entry();
    }
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
  if (entries_) {
    for (uint16_t i = 0; i < maxSlots_; ++i) {
      evictSlot(static_cast<int>(i));
    }
  }
  font_.clearScratch();
#if defined(ESP32)
  if (mutex_) xSemaphoreGive(mutex_);
#endif
}

int TtfEpdFont::ensureGlyph(uint32_t cp) const {
  if (!valid_ || !entries_) return -1;

  // Hit.
  for (uint16_t i = 0; i < maxSlots_; ++i) {
    if (entries_[i].cp == cp) {
      entries_[i].lastAccess = ++accessCounter_;
      return static_cast<int>(i);
    }
  }

  uint16_t gid = 0;
  if (!font_.findGlyph(cp, gid)) return -1;
  if (gid == 0 && cp != '?') {
    // Missing codepoint: share the '?' entry (renders '?' like the epdfont
    // backend, and keeps GfxRenderer::hasTextGlyphs' "same as '?'" detection
    // working so UI chrome can fall back to the independent emergency face).
    return ensureGlyph('?');
  }
  ttf::GlyphBitmap gb;
  if (!font_.rasterize(gid, sizePx_, gb)) {
    // Unsupported glyph (e.g. compound with point args): tofu via .notdef.
    if (gid != 0 && !font_.rasterize(0, sizePx_, gb)) return -1;
  }

  // Slot: free one or LRU.
  int slot = -1;
  uint32_t minAccess = 0xFFFFFFFF;
  for (uint16_t i = 0; i < maxSlots_; ++i) {
    if (entries_[i].cp == 0xFFFFFFFF) {
      slot = static_cast<int>(i);
      break;
    }
    if (entries_[i].lastAccess < minAccess) {
      minAccess = entries_[i].lastAccess;
      slot = static_cast<int>(i);
    }
  }
  if (slot < 0) return -1;
  evictSlot(slot);

  // Bitmap: allocate from PSRAM (preferred) / heap, bounded by budget.
  uint8_t* bmp = nullptr;
  const uint32_t packedLen = gb.packedLen;
  if (packedLen > 0 && gb.data) {
    bmp = static_cast<uint8_t*>(ttfAlloc(packedLen));
    if (!bmp) {
      Serial.printf("[TTF] glyph alloc %u failed (OOM)\n", packedLen);
      bmp = nullptr;
    } else {
      std::memcpy(bmp, gb.data, packedLen);
    }
  }

  entries_[slot].cp = cp;
  entries_[slot].lastAccess = ++accessCounter_;
  entries_[slot].glyph.width = static_cast<uint8_t>(gb.width);
  entries_[slot].glyph.height = static_cast<uint8_t>(gb.height);
  entries_[slot].glyph.advanceX = static_cast<uint8_t>(std::max(0, std::min(255, static_cast<int>(gb.advance))));
  entries_[slot].glyph.left = gb.xoff;
  entries_[slot].glyph.top = gb.yoff;
  entries_[slot].glyph.dataLength = packedLen;
  entries_[slot].glyph.dataOffset = cp;  // reuses EpdGlyph as the cache key
  entries_[slot].bitmap = bmp;
  entries_[slot].bitmapSize = packedLen;
  cacheBytes_ += packedLen;

  // Bounded budget: evict LRU (sparing the fresh slot) until under budget.
  if (cacheBytes_ > cacheBudget_) {
    for (uint16_t pass = 0; pass < maxSlots_ && cacheBytes_ > cacheBudget_; ++pass) {
      int victim = -1;
      uint32_t la = 0xFFFFFFFF;
      for (uint16_t i = 0; i < maxSlots_; ++i) {
        if (static_cast<int>(i) == slot || entries_[i].cp == 0xFFFFFFFF) continue;
        if (entries_[i].lastAccess < la) {
          la = entries_[i].lastAccess;
          victim = static_cast<int>(i);
        }
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
  if (slot < 0) return nullptr;
  return &entries_[slot].glyph;
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
