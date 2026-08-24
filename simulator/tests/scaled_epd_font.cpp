#include "NativeGridEpdFont.h"
#include "ScaledEpdFont.h"
#include "util/M4FontPolicy.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

[[noreturn]] void fail(const char* msg) {
  std::cerr << "scaled_epd_font FAIL: " << msg << "\n";
  std::exit(1);
}

#define CHECK(cond) \
  do {              \
    if (!(cond)) fail(#cond); \
  } while (0)

bool pixelOn(const uint8_t* bmp, int w, int x, int y) {
  const uint32_t idx = static_cast<uint32_t>(y) * static_cast<uint32_t>(w) + static_cast<uint32_t>(x);
  return ((bmp[idx / 8u] >> (7u - (idx % 8u))) & 1u) != 0;
}

void copyBitmap(const uint8_t* src, uint32_t n, std::vector<uint8_t>* out) {
  if (!src) fail("null bitmap");
  out->assign(src, src + n);
}

bool bitmapHasInk(const uint8_t* bmp, uint32_t dataLength) {
  if (!bmp || dataLength == 0) return false;
  for (uint32_t i = 0; i < dataLength; ++i) {
    if (bmp[i] != 0) return true;
  }
  return false;
}

std::vector<uint8_t> readFile(const char* path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) fail("cannot open native-grid blob");
  in.seekg(0, std::ios::end);
  const std::streamoff n = in.tellg();
  if (n <= 0) fail("empty native-grid blob");
  in.seekg(0, std::ios::beg);
  std::vector<uint8_t> buf(static_cast<size_t>(n));
  in.read(reinterpret_cast<char*>(buf.data()), n);
  if (!in) fail("short native-grid blob read");
  return buf;
}

struct VisualBox {
  int width = 0;
  int height = 0;
  int left = 0;
  int top = 0;
  int advance = 0;
  int ink0 = 0;
  int ink1 = -1;
  int lsb = 0;
  int rsb = 0;
  int inkW = 0;
  bool any = false;
};

VisualBox measure(const EpdFont& font, uint32_t cp) {
  VisualBox b;
  const EpdGlyph* glyph = font.getGlyph(cp);
  if (!glyph) fail("missing glyph");
  b.width = glyph->width;
  b.height = glyph->height;
  b.left = glyph->left;
  b.top = glyph->top;
  b.advance = glyph->advanceX;
  const uint8_t* bmp = font.loadGlyphBitmap(glyph, nullptr);
  if (!bmp) fail("missing bitmap");
  int x0 = b.width;
  int x1 = -1;
  for (int y = 0; y < b.height; ++y) {
    for (int x = 0; x < b.width; ++x) {
      if (pixelOn(bmp, b.width, x, y)) {
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
      }
    }
  }
  if (x1 >= 0) {
    b.any = true;
    b.ink0 = x0;
    b.ink1 = x1;
    b.inkW = x1 - x0 + 1;
    b.lsb = b.left + x0;
    b.rsb = b.advance - (b.left + x1 + 1);
  }
  return b;
}

uint32_t nextCp(const char*& p) {
  const unsigned char c = static_cast<unsigned char>(*p);
  if (c == 0) return 0;
  if (c < 0x80) {
    ++p;
    return c;
  }
  if ((c & 0xE0) == 0xC0) {
    const uint32_t cp = (static_cast<uint32_t>(c & 0x1F) << 6) | (static_cast<unsigned char>(p[1]) & 0x3F);
    p += 2;
    return cp;
  }
  if ((c & 0xF0) == 0xE0) {
    const uint32_t cp = (static_cast<uint32_t>(c & 0x0F) << 12) |
                        ((static_cast<unsigned char>(p[1]) & 0x3F) << 6) |
                        (static_cast<unsigned char>(p[2]) & 0x3F);
    p += 3;
    return cp;
  }
  fail("invalid utf-8");
}

bool findIsolatedVerticalStroke(const uint8_t* bmp, int w, int h, int* sx, int* y0, int* y1) {
  for (int x = 0; x < w; ++x) {
    int run0 = -1;
    int run1 = -1;
    for (int y = 0; y < h; ++y) {
      const bool on = pixelOn(bmp, w, x, y);
      const bool leftOff = (x == 0) || !pixelOn(bmp, w, x - 1, y);
      const bool rightOff = (x + 1 >= w) || !pixelOn(bmp, w, x + 1, y);
      const bool isolated = on && leftOff && rightOff;
      if (isolated) {
        if (run0 < 0) run0 = y;
        run1 = y + 1;
      } else if (run0 >= 0) {
        if (run1 - run0 >= 3) {
          *sx = x;
          *y0 = run0;
          *y1 = run1;
          return true;
        }
        run0 = -1;
        run1 = -1;
      }
    }
    if (run0 >= 0 && run1 - run0 >= 3) {
      *sx = x;
      *y0 = run0;
      *y1 = run1;
      return true;
    }
  }
  return false;
}

bool findIsolatedHorizontalStroke(const uint8_t* bmp, int w, int h, int* sy, int* x0, int* x1) {
  for (int y = 0; y < h; ++y) {
    int run0 = -1;
    int run1 = -1;
    for (int x = 0; x < w; ++x) {
      const bool on = pixelOn(bmp, w, x, y);
      const bool upOff = (y == 0) || !pixelOn(bmp, w, x, y - 1);
      const bool downOff = (y + 1 >= h) || !pixelOn(bmp, w, x, y + 1);
      const bool isolated = on && upOff && downOff;
      if (isolated) {
        if (run0 < 0) run0 = x;
        run1 = x + 1;
      } else if (run0 >= 0) {
        if (run1 - run0 >= 3) {
          *sy = y;
          *x0 = run0;
          *x1 = run1;
          return true;
        }
        run0 = -1;
        run1 = -1;
      }
    }
    if (run0 >= 0 && run1 - run0 >= 3) {
      *sy = y;
      *x0 = run0;
      *x1 = run1;
      return true;
    }
  }
  return false;
}

}  // namespace

static_assert(M4FontPolicy::kLogicalCellPx == 16, "logical system cell is 16x16");
static_assert(M4FontPolicy::kNativeGridSourcePx == 16, "native-grid system face is a 16-row raster");
static_assert(M4FontPolicy::kCanonicalEpdfontPixelSize == 16, "canonical SD epdfont is a 16px artifact");
static_assert(M4FontPolicy::kChromeSmallScale == 1, "SMALL chrome is 1x");
static_assert(M4FontPolicy::kChromeUi10Scale == 2, "UI_10 chrome is 2x");
static_assert(M4FontPolicy::kChromeUi12Scale == 2, "UI_12 chrome is 2x");
static_assert(M4FontPolicy::kChromeSmallPx == 16, "SMALL chrome cell is 16px");
static_assert(M4FontPolicy::kChromeUi10Px == 32, "UI_10 chrome cell is 32px");
static_assert(M4FontPolicy::kChromeUi12Px == 32, "UI_12 chrome cell is 32px");
static_assert(M4FontPolicy::nativeGridIntegerScale(12) == 1, "12–20 → 1x");
static_assert(M4FontPolicy::nativeGridIntegerScale(18) == 1, "default 18 → 1x");
static_assert(M4FontPolicy::nativeGridIntegerScale(20) == 1, "20 → 1x");
static_assert(M4FontPolicy::nativeGridIntegerScale(21) == 2, "21–39 → 2x");
static_assert(M4FontPolicy::nativeGridIntegerScale(31) == 2, "diagnosed 31 → 2x");
static_assert(M4FontPolicy::nativeGridIntegerScale(39) == 2, "39 → 2x");
static_assert(M4FontPolicy::nativeGridIntegerScale(40) == 3, "40+ → 3x");
static_assert(M4FontPolicy::nativeGridIntegerScale(48) == 3, "48 → 3x");
static_assert(M4FontPolicy::nativeGridCellPx(18) == 16, "18 snaps to 16");
static_assert(M4FontPolicy::nativeGridCellPx(22) == 32, "22 snaps to 32");
static_assert(M4FontPolicy::nativeGridCellPx(31) == 32, "31 snaps to 32");
static_assert(M4FontPolicy::nativeGridCellPx(40) == 48, "40 snaps to 48");

int main(int argc, char** argv) {
  CHECK(M4FontPolicy::systemReaderSourcePx() == M4FontPolicy::kLogicalCellPx);
  CHECK(M4FontPolicy::kChromeSmallPx == 16);
  CHECK(M4FontPolicy::kChromeUi10Px == 32);
  CHECK(M4FontPolicy::kChromeUi12Px == 32);
  // Builtin snaps; TTF keeps the numeric size (documented, not applied here).
  CHECK(M4FontPolicy::nativeGridIntegerScale(31) == 2);
  CHECK(M4FontPolicy::nativeGridCellPx(31) == 32);
  CHECK(M4FontPolicy::nativeGridCellPx(31) != 31);

  const char* path = argc > 1 ? argv[1] : "../firmware/src/fontdata/m4_native_grid_15x16.bin";
  const std::vector<uint8_t> blob = readFile(path);
  NativeGridEpdFont source;
  CHECK(source.bind(blob.data(), blob.size()));
  CHECK(source.valid());

  static constexpr uint32_t kCjk = 0x4E2D;  // 中
  const EpdGlyph* srcGlyph = source.getGlyph(kCjk);
  CHECK(srcGlyph != nullptr);
  const uint8_t srcW = srcGlyph->width;
  const uint8_t srcH = srcGlyph->height;
  const uint8_t srcAX = srcGlyph->advanceX;
  const int16_t srcLeft = srcGlyph->left;
  const int16_t srcTop = srcGlyph->top;
  CHECK(srcW == 15);
  CHECK(srcH == 16);
  CHECK(srcAX == 16);
  CHECK(srcLeft == 0);

  std::vector<uint8_t> srcBmp;
  copyBitmap(source.loadGlyphBitmap(srcGlyph, nullptr), srcGlyph->dataLength, &srcBmp);

  const EpdFontData* srcData = source.getData();
  CHECK(srcData != nullptr);
  CHECK(srcData->is2Bit == false);
  CHECK(srcData->advanceY == 16);

  // Nominal reader sizes must snap to integer N, never 14/15/17/20/22/29/31.
  static constexpr int kNominal[] = {12, 18, 20, 21, 22, 31, 39, 40, 48};
  ScaledEpdFont scaled;
  for (int px : kNominal) {
    const int n = M4FontPolicy::nativeGridIntegerScale(px);
    scaled.bindInteger(&source, n);
    CHECK(scaled.integerScale() == n);
    CHECK(scaled.replicatesSourcePixels());
    CHECK(scaled.scale() == static_cast<float>(n));

    const EpdFontData* data = scaled.getData();
    CHECK(data != nullptr);
    CHECK(data->advanceY == static_cast<uint8_t>(16 * n));
    CHECK(data->ascender == srcData->ascender * n);

    const EpdGlyph* glyph = scaled.getGlyph(kCjk);
    CHECK(glyph != nullptr);
    CHECK(glyph->advanceX == 16 * n);
    CHECK(glyph->width == static_cast<uint8_t>(srcW * n));
    CHECK(glyph->height == static_cast<uint8_t>(srcH * n));
    CHECK(glyph->left == srcLeft * n);
    CHECK(glyph->top == srcTop * n);
    CHECK(glyph->advanceX != 29);
    CHECK(glyph->width != 29);
    CHECK(glyph->height != 31);
    CHECK(glyph->width != 20);
    CHECK(glyph->height != 22);
    CHECK(glyph->width != 24);
    CHECK(glyph->height != 26);

    if (n == 1) {
      CHECK(scaled.isUnityScale());
      CHECK(glyph->width == srcW);
      CHECK(glyph->advanceX == 16);
    } else {
      CHECK(!scaled.isUnityScale());
    }

    const uint8_t* bmp = scaled.loadGlyphBitmap(glyph, nullptr);
    CHECK(bmp != nullptr);
    CHECK(bitmapHasInk(bmp, glyph->dataLength));
    std::cout << "builtin snap nominal=" << px << " N=" << n << " cell=" << (16 * n)
              << " glyph=" << static_cast<int>(glyph->width) << "x" << static_cast<int>(glyph->height)
              << " advance=" << static_cast<int>(glyph->advanceX) << "\n";
  }

  auto checkKronecker = [&](int n) {
    scaled.bindInteger(&source, n);
    const EpdGlyph* g = scaled.getGlyph(kCjk);
    CHECK(g != nullptr);
    CHECK(g->width == srcW * n);
    CHECK(g->height == srcH * n);
    CHECK(g->advanceX == 16 * n);
    std::vector<uint8_t> dst;
    copyBitmap(scaled.loadGlyphBitmap(g, nullptr), g->dataLength, &dst);
    const int dw = g->width;
    const int dh = g->height;
    for (int sy = 0; sy < srcH; ++sy) {
      for (int sx = 0; sx < srcW; ++sx) {
        const bool on = pixelOn(srcBmp.data(), srcW, sx, sy);
        for (int iy = 0; iy < n; ++iy) {
          for (int ix = 0; ix < n; ++ix) {
            CHECK(pixelOn(dst.data(), dw, sx * n + ix, sy * n + iy) == on);
          }
        }
      }
    }

    int vsx = 0, vy0 = 0, vy1 = 0;
    if (findIsolatedVerticalStroke(srcBmp.data(), srcW, srcH, &vsx, &vy0, &vy1)) {
      for (int sy = vy0; sy < vy1; ++sy) {
        for (int iy = 0; iy < n; ++iy) {
          const int dy = sy * n + iy;
          int run = 0;
          int x = vsx * n;
          while (x < dw && pixelOn(dst.data(), dw, x, dy)) {
            ++run;
            ++x;
          }
          CHECK(run == n);
          if (vsx > 0) CHECK(!pixelOn(dst.data(), dw, vsx * n - 1, dy));
          if (vsx * n + n < dw) CHECK(!pixelOn(dst.data(), dw, vsx * n + n, dy));
        }
      }
    }

    int hsy = 0, hx0 = 0, hx1 = 0;
    if (findIsolatedHorizontalStroke(srcBmp.data(), srcW, srcH, &hsy, &hx0, &hx1)) {
      for (int sx = hx0; sx < hx1; ++sx) {
        for (int ix = 0; ix < n; ++ix) {
          const int dx = sx * n + ix;
          int run = 0;
          int y = hsy * n;
          while (y < dh && pixelOn(dst.data(), dw, dx, y)) {
            ++run;
            ++y;
          }
          CHECK(run == n);
          if (hsy > 0) CHECK(!pixelOn(dst.data(), dw, dx, hsy * n - 1));
          if (hsy * n + n < dh) CHECK(!pixelOn(dst.data(), dw, dx, hsy * n + n));
        }
      }
    }
  };
  checkKronecker(2);
  checkKronecker(3);

  // Extra 1px-stroke CJK probes (中 口 日 工 十). At least one must expose a
  // true 1px isolated vertical and horizontal stroke so N-width is enforced.
  static constexpr uint32_t kStrokeCjk[] = {kCjk, 0x53E3u, 0x65E5u, 0x5DE5u, 0x5341u};
  int foundV = 0;
  int foundH = 0;
  for (uint32_t cp : kStrokeCjk) {
    const EpdGlyph* sg = source.getGlyph(cp);
    CHECK(sg != nullptr);
    const uint8_t sw = sg->width;
    const uint8_t sh = sg->height;
    std::vector<uint8_t> sb;
    copyBitmap(source.loadGlyphBitmap(sg, nullptr), sg->dataLength, &sb);
    int vsx = 0, vy0 = 0, vy1 = 0, hsy = 0, hx0 = 0, hx1 = 0;
    const bool hasV = findIsolatedVerticalStroke(sb.data(), sw, sh, &vsx, &vy0, &vy1);
    const bool hasH = findIsolatedHorizontalStroke(sb.data(), sw, sh, &hsy, &hx0, &hx1);
    for (int n : {2, 3}) {
      scaled.bindInteger(&source, n);
      const EpdGlyph* gn = scaled.getGlyph(cp);
      CHECK(gn != nullptr);
      CHECK(gn->advanceX == 16 * n);
      std::vector<uint8_t> db;
      copyBitmap(scaled.loadGlyphBitmap(gn, nullptr), gn->dataLength, &db);
      if (hasV) {
        for (int iy = 0; iy < n; ++iy) {
          int run = 0;
          int x = vsx * n;
          const int dy = vy0 * n + iy;
          while (x < gn->width && pixelOn(db.data(), gn->width, x, dy)) {
            ++run;
            ++x;
          }
          CHECK(run == n);
        }
      }
      if (hasH) {
        for (int ix = 0; ix < n; ++ix) {
          int run = 0;
          int y = hsy * n;
          const int dx = hx0 * n + ix;
          while (y < gn->height && pixelOn(db.data(), gn->width, dx, y)) {
            ++run;
            ++y;
          }
          CHECK(run == n);
        }
      }
    }
    if (hasV) ++foundV;
    if (hasH) ++foundH;
  }
  CHECK(foundV > 0);
  CHECK(foundH > 0);

  // Latin/digits stay proportional and are not 16N.
  static constexpr uint32_t kLatin[] = {'A', 'i', 'M', '1', ',', '.'};
  for (int n : {1, 2, 3}) {
    scaled.bindInteger(&source, n);
    int seen = 0;
    int prev = -1;
    bool variable = false;
    for (uint32_t cp : kLatin) {
      const VisualBox b = measure(scaled, cp);
      CHECK(b.advance > 0);
      CHECK(b.advance < 16 * n);
      CHECK(b.lsb == NativeGridEpdFont::kLatinSideBearing * n);
      CHECK(b.rsb == NativeGridEpdFont::kLatinSideBearing * n);
      if (prev >= 0 && b.advance != prev) variable = true;
      prev = b.advance;
      ++seen;
    }
    CHECK(seen == 6);
    CHECK(variable);
  }

  // Directional punctuation: bearings scale by exact N, not centering.
  static constexpr uint32_t kPairs[][2] = {
      {'(', ')'},
      {'[', ']'},
      {'<', '>'},
      {0x2018u, 0x2019u},
      {0x201Cu, 0x201Du},
      {0x300Au, 0x300Bu},
      {0x300Cu, 0x300Du},
      {0x300Eu, 0x300Fu},
      {0x3008u, 0x3009u},
      {0x3010u, 0x3011u},
      {0xFF08u, 0xFF09u},
  };
  for (int n : {1, 2, 3}) {
    scaled.bindInteger(&source, n);
    for (const auto& pair : kPairs) {
      const VisualBox open = measure(scaled, pair[0]);
      const VisualBox close = measure(scaled, pair[1]);
      CHECK(open.lsb == NativeGridEpdFont::kPairOuterBearing * n);
      CHECK(open.rsb == NativeGridEpdFont::kPairInnerBearing * n);
      CHECK(close.lsb == NativeGridEpdFont::kPairInnerBearing * n);
      CHECK(close.rsb == NativeGridEpdFont::kPairOuterBearing * n);
      CHECK(open.lsb > open.rsb);
      CHECK(close.lsb < close.rsb);
      CHECK(open.advance < 16 * n);
      CHECK(close.advance < 16 * n);
    }
  }

  auto walk = [&](ScaledEpdFont& face, int n, const char* s, const char* tag) {
    int pen = 0;
    int latinN = 0;
    int cjkN = 0;
    const char* p = s;
    std::cout << "scaled N=" << n << " " << tag << ":";
    while (*p) {
      const uint32_t cp = nextCp(p);
      const auto kind = NativeGridEpdFont::metricKind(cp);
      const VisualBox b = measure(face, cp);
      if (kind == NativeGridEpdFont::MetricKind::Space) {
        CHECK(b.advance == NativeGridEpdFont::kSpaceAdvance * n);
      } else if (kind == NativeGridEpdFont::MetricKind::Latin) {
        CHECK(b.advance < 16 * n);
        ++latinN;
      } else if (kind == NativeGridEpdFont::MetricKind::PairOpen) {
        CHECK(b.lsb > b.rsb);
        CHECK(b.advance < 16 * n);
      } else if (kind == NativeGridEpdFont::MetricKind::PairClose) {
        CHECK(b.lsb < b.rsb);
        CHECK(b.advance < 16 * n);
      } else {
        CHECK(b.left == 0);
        CHECK(b.advance == 16 * n);
        ++cjkN;
      }
      pen += b.advance;
    }
    std::cout << " latin=" << latinN << " cjk=" << cjkN << " advance=" << pen << "\n";
    CHECK(pen > 0);
  };
  for (int n : {1, 2, 3}) {
    scaled.bindInteger(&source, n);
    walk(scaled, n, "设置 阅读 ABC abc 123,.;:!?()[]", "mixed");
    walk(scaled, n, "“测试” ‘ABC’ （测试） 《书名》 【章节】", "pairs");
  }

  // Chrome roles are integer N and independent of reader size.
  ScaledEpdFont chromeSmall;
  ScaledEpdFont chromeUi10;
  ScaledEpdFont chromeUi12;
  chromeSmall.bindInteger(&source, M4FontPolicy::kChromeSmallScale);
  chromeUi10.bindInteger(&source, M4FontPolicy::kChromeUi10Scale);
  chromeUi12.bindInteger(&source, M4FontPolicy::kChromeUi12Scale);
  CHECK(chromeSmall.getData()->advanceY == 16);
  CHECK(chromeUi10.getData()->advanceY == 32);
  CHECK(chromeUi12.getData()->advanceY == 32);
  CHECK(measure(chromeSmall, kCjk).advance == 16);
  CHECK(measure(chromeUi10, kCjk).advance == 32);
  const int chromeAdv = measure(chromeUi10, kCjk).advance;
  const int chromeLine = chromeUi10.getData()->advanceY;
  for (int px : {18, 22, 31, 40, 48}) {
    scaled.bindInteger(&source, M4FontPolicy::nativeGridIntegerScale(px));
    CHECK(measure(chromeUi10, kCjk).advance == chromeAdv);
    CHECK(chromeUi10.getData()->advanceY == chromeLine);
    CHECK(chromeSmall.getData()->advanceY == 16);
  }

  // Canonical/non-native dest-sample path still accepts an arbitrary ratio
  // without integer snap when replicate=false (TTF does not use this scaler).
  scaled.bind(&source, 31.0f / 16.0f, false);
  CHECK(scaled.integerScale() == 0);
  CHECK(!scaled.replicatesSourcePixels());
  const EpdGlyph* arbitrary = scaled.getGlyph(kCjk);
  CHECK(arbitrary != nullptr);
  // Dest-sample of 15x16 at 31/16 is the old non-native path; native must not
  // use it. Width is lround(15*31/16)=29 — native bindInteger(2) is 30x32.
  CHECK(arbitrary->width == 29);
  CHECK(arbitrary->height == 31);

  std::cout << "scaled EpdFont integer-N native-grid: PASS\n";
  return 0;
}
