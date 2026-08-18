#pragma once

// Deterministic physical-panel dirty planner + conservative partial policy.
// Host-testable. Operates on two 800x480 MONO1 48,000-byte buffers (the last
// successfully presented panel framebuffer vs the newest mapped candidate).
// Never inspects the M4B3 accepted/ACK logical framebuffer.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "util/M4PanelMapper.h"

namespace M4PanelDirty {

constexpr uint16_t kWidth = M4PanelMapper::kPhysicalWidth;    // 800
constexpr uint16_t kHeight = M4PanelMapper::kPhysicalHeight;  // 480
constexpr uint16_t kStride = M4PanelMapper::kPhysicalStride;  // 100
constexpr uint32_t kSize = M4PanelMapper::kPhysicalSize;      // 48000
constexpr uint32_t kPanelPixels = static_cast<uint32_t>(kWidth) * kHeight;  // 384000

// Tile grid: 32x16 keeps X byte-aligned and avoids a swarm of 8x1 activations.
constexpr uint16_t kTileW = 32;
constexpr uint16_t kTileH = 16;
constexpr uint16_t kTilesX = kWidth / kTileW;   // 25
constexpr uint16_t kTilesY = kHeight / kTileH;  // 30

// Conservative production defaults (Phase D). Tune only with hardware evidence.
constexpr uint16_t kMaxWindows = 4;
constexpr uint32_t kMaxPartialChangedPixels = (kPanelPixels * 28u) / 100u;  // 107520
constexpr uint32_t kMaxPartialsSinceFull = 8;
constexpr uint32_t kMaxCumulativePartialPixels = kPanelPixels;  // one panel-equivalent
constexpr uint16_t kPlanCap = 16;  // merge output cap before declaring fragmented
// Nearby-window compaction only. 96px (~3 tiles X / 6 tiles Y) joins glyph/HUD
// gaps on one strip. 200px-separated corners stay distinct so Fragmented still
// covers a scattered multi-widget page. Union-area cap blocks greedy
// chain-swallow of medium widgets into a half-panel Partial.
constexpr uint16_t kMergeGapX = 96;
constexpr uint16_t kMergeGapY = 96;
constexpr uint32_t kMaxMergeUnionArea = (kPanelPixels * 8u) / 100u;  // 30720

static_assert((kWidth % kTileW) == 0, "tile width divides panel");
static_assert((kHeight % kTileH) == 0, "tile height divides panel");
static_assert((kTileW % 8) == 0, "tiles are byte-aligned in X");
static_assert(kMaxWindows >= 1 && kMaxWindows <= kPlanCap, "window bound");

enum class Reason : uint8_t {
  None = 0,
  FirstBaseline = 1,
  UntrustedBaseline = 2,
  NoChange = 3,
  SparsePartial = 4,
  DenseArea = 5,
  Fragmented = 6,
  CadenceCount = 7,
  CadenceArea = 8,
  ForcedFullRecovery = 9,
};

enum class Mode : uint8_t { Full = 0, Partial = 1, Skip = 2 };

inline const char* reasonName(Reason r) {
  switch (r) {
    case Reason::FirstBaseline:
      return "first";
    case Reason::UntrustedBaseline:
      return "untrusted";
    case Reason::NoChange:
      return "no_change";
    case Reason::SparsePartial:
      return "sparse";
    case Reason::DenseArea:
      return "dense";
    case Reason::Fragmented:
      return "fragmented";
    case Reason::CadenceCount:
      return "cadence_n";
    case Reason::CadenceArea:
      return "cadence_area";
    case Reason::ForcedFullRecovery:
      return "recover";
    case Reason::None:
    default:
      return "none";
  }
}

struct Rect {
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t w = 0;
  uint16_t h = 0;
};

struct Plan {
  uint32_t changedPixels = 0;
  uint32_t windowArea = 0;
  uint16_t windowCount = 0;
  Rect windows[kPlanCap]{};
};

struct Decision {
  Mode mode = Mode::Full;
  Reason reason = Reason::UntrustedBaseline;
};

inline uint32_t popcount8(uint8_t v) {
  return static_cast<uint32_t>(__builtin_popcount(static_cast<unsigned>(v)));
}

inline bool rowBytesDiffer(const uint8_t* a, const uint8_t* b, int y, int byte0, int byte1Incl) {
  const size_t row = static_cast<size_t>(y) * kStride;
  for (int bx = byte0; bx <= byte1Incl; ++bx) {
    if (a[row + static_cast<size_t>(bx)] != b[row + static_cast<size_t>(bx)]) return true;
  }
  return false;
}

inline uint32_t rectArea(const Rect& r) {
  return static_cast<uint32_t>(r.w) * r.h;
}

inline uint16_t axisGap(int a0, int a1, int b0, int b1) {
  const int left = a0 < b0 ? a1 : b1;
  const int right = a0 < b0 ? b0 : a0;
  const int g = right - left;
  return g > 0 ? static_cast<uint16_t>(g) : 0;
}

inline uint16_t gapX(const Rect& a, const Rect& b) {
  return axisGap(static_cast<int>(a.x), static_cast<int>(a.x) + a.w, static_cast<int>(b.x),
                 static_cast<int>(b.x) + b.w);
}

inline uint16_t gapY(const Rect& a, const Rect& b) {
  return axisGap(static_cast<int>(a.y), static_cast<int>(a.y) + a.h, static_cast<int>(b.y),
                 static_cast<int>(b.y) + b.h);
}

inline Rect unionRect(const Rect& a, const Rect& b) {
  const int x0 = a.x < b.x ? static_cast<int>(a.x) : static_cast<int>(b.x);
  const int y0 = a.y < b.y ? static_cast<int>(a.y) : static_cast<int>(b.y);
  const int x1 = (static_cast<int>(a.x) + a.w) > (static_cast<int>(b.x) + b.w)
                     ? (static_cast<int>(a.x) + a.w)
                     : (static_cast<int>(b.x) + b.w);
  const int y1 = (static_cast<int>(a.y) + a.h) > (static_cast<int>(b.y) + b.h)
                     ? (static_cast<int>(a.y) + a.h)
                     : (static_cast<int>(b.y) + b.h);
  Rect r;
  r.x = static_cast<uint16_t>(x0);
  r.y = static_cast<uint16_t>(y0);
  r.w = static_cast<uint16_t>(x1 - x0);
  r.h = static_cast<uint16_t>(y1 - y0);
  return r;
}

inline uint32_t mergeWaste(const Rect& a, const Rect& b) {
  const uint32_t ua = rectArea(unionRect(a, b));
  const uint32_t sa = rectArea(a) + rectArea(b);
  return ua > sa ? ua - sa : 0;
}

inline bool shrinkToDirty(const uint8_t* prev, const uint8_t* next, Rect& r) {
  if (!prev || !next || r.w == 0 || r.h == 0) return false;
  int x0 = static_cast<int>(r.x);
  int y0 = static_cast<int>(r.y);
  int x1 = x0 + static_cast<int>(r.w) - 1;
  int y1 = y0 + static_cast<int>(r.h) - 1;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 >= static_cast<int>(kWidth)) x1 = static_cast<int>(kWidth) - 1;
  if (y1 >= static_cast<int>(kHeight)) y1 = static_cast<int>(kHeight) - 1;
  if (x0 > x1 || y0 > y1) return false;

  int byte0 = x0 >> 3;
  int byte1 = x1 >> 3;
  while (y0 <= y1 && !rowBytesDiffer(prev, next, y0, byte0, byte1)) ++y0;
  while (y1 >= y0 && !rowBytesDiffer(prev, next, y1, byte0, byte1)) --y1;
  if (y0 > y1) return false;

  int minB = byte1;
  int maxB = byte0;
  for (int y = y0; y <= y1; ++y) {
    const size_t row = static_cast<size_t>(y) * kStride;
    for (int bx = byte0; bx <= byte1; ++bx) {
      if (prev[row + static_cast<size_t>(bx)] != next[row + static_cast<size_t>(bx)]) {
        if (bx < minB) minB = bx;
        if (bx > maxB) maxB = bx;
      }
    }
  }
  if (minB > maxB) return false;
  r.x = static_cast<uint16_t>(minB * 8);
  r.y = static_cast<uint16_t>(y0);
  r.w = static_cast<uint16_t>((maxB - minB + 1) * 8);
  r.h = static_cast<uint16_t>(y1 - y0 + 1);
  return r.w > 0 && r.h > 0 && (r.x % 8) == 0 && (r.w % 8) == 0 &&
         (static_cast<uint32_t>(r.x) + r.w) <= kWidth && (static_cast<uint32_t>(r.y) + r.h) <= kHeight;
}

// Diff prev (last successful physical present) vs next (newest mapped).
inline bool plan(const uint8_t* prev, const uint8_t* next, size_t len, Plan& out) {
  out = Plan{};
  if (!prev || !next || len != kSize) return false;

  // Static: plan() runs on the Arduino loop task (~8 KB stack). A 750-tile
  // scratch would overflow if kept automatic.
  static uint8_t dirty[kTilesX * kTilesY];
  std::memset(dirty, 0, sizeof(dirty));

  for (int y = 0; y < static_cast<int>(kHeight); ++y) {
    const size_t row = static_cast<size_t>(y) * kStride;
    const int ty = y / static_cast<int>(kTileH);
    for (int bx = 0; bx < static_cast<int>(kStride); ++bx) {
      const uint8_t d = static_cast<uint8_t>(prev[row + static_cast<size_t>(bx)] ^
                                            next[row + static_cast<size_t>(bx)]);
      if (d == 0) continue;
      out.changedPixels += popcount8(d);
      dirty[ty * kTilesX + (bx * 8) / kTileW] = 1;
    }
  }
  if (out.changedPixels == 0) return true;

  struct Run {
    uint16_t ty;
    uint16_t tx0;
    uint16_t tx1;  // exclusive
  };
  static Run runs[kTilesY * kTilesX];
  uint16_t nRuns = 0;
  for (uint16_t ty = 0; ty < kTilesY; ++ty) {
    uint16_t tx = 0;
    while (tx < kTilesX) {
      if (!dirty[ty * kTilesX + tx]) {
        ++tx;
        continue;
      }
      uint16_t tx0 = tx;
      while (tx < kTilesX && dirty[ty * kTilesX + tx]) ++tx;
      if (nRuns < static_cast<uint16_t>(kTilesY * kTilesX)) {
        runs[nRuns++] = Run{ty, tx0, tx};
      }
    }
  }

  static uint8_t used[kTilesY * kTilesX];
  std::memset(used, 0, sizeof(used));
  uint16_t produced = 0;
  bool overflow = false;
  for (uint16_t i = 0; i < nRuns; ++i) {
    if (used[i]) continue;
    used[i] = 1;
    uint16_t ty0 = runs[i].ty;
    uint16_t ty1 = runs[i].ty;
    const uint16_t tx0 = runs[i].tx0;
    const uint16_t tx1 = runs[i].tx1;
    bool grew = true;
    while (grew) {
      grew = false;
      for (uint16_t j = 0; j < nRuns; ++j) {
        if (used[j]) continue;
        if (runs[j].tx0 != tx0 || runs[j].tx1 != tx1) continue;
        if (runs[j].ty == static_cast<uint16_t>(ty1 + 1)) {
          used[j] = 1;
          ty1 = runs[j].ty;
          grew = true;
        } else if (runs[j].ty + 1 == ty0) {
          used[j] = 1;
          ty0 = runs[j].ty;
          grew = true;
        }
      }
    }
    Rect r;
    r.x = static_cast<uint16_t>(tx0 * kTileW);
    r.y = static_cast<uint16_t>(ty0 * kTileH);
    r.w = static_cast<uint16_t>((tx1 - tx0) * kTileW);
    r.h = static_cast<uint16_t>((ty1 - ty0 + 1) * kTileH);
    if (!shrinkToDirty(prev, next, r)) continue;
    if (produced < kPlanCap) {
      out.windows[produced++] = r;
    } else {
      overflow = true;
    }
  }

  // Compact nearby windows so a sparse HUD/glyph update (many small gaps on
  // one strip) does not trip Fragmented. Overflowed plans stay Fragmented so
  // we never Partial-refresh a truncated cover. Distant widgets stay split.
  if (!overflow) {
    while (produced > kMaxWindows) {
      int bestI = -1;
      int bestJ = -1;
      uint32_t bestWaste = 0xFFFFFFFFu;
      for (uint16_t i = 0; i < produced; ++i) {
        for (uint16_t j = static_cast<uint16_t>(i + 1); j < produced; ++j) {
          if (gapX(out.windows[i], out.windows[j]) > kMergeGapX) continue;
          if (gapY(out.windows[i], out.windows[j]) > kMergeGapY) continue;
          if (rectArea(unionRect(out.windows[i], out.windows[j])) > kMaxMergeUnionArea) continue;
          const uint32_t waste = mergeWaste(out.windows[i], out.windows[j]);
          if (waste < bestWaste) {
            bestWaste = waste;
            bestI = static_cast<int>(i);
            bestJ = static_cast<int>(j);
          }
        }
      }
      if (bestI < 0) break;
      Rect u = unionRect(out.windows[bestI], out.windows[bestJ]);
      if (!shrinkToDirty(prev, next, u)) break;
      out.windows[bestI] = u;
      for (uint16_t k = static_cast<uint16_t>(bestJ + 1); k < produced; ++k) {
        out.windows[k - 1] = out.windows[k];
      }
      --produced;
    }
  }

  out.windowArea = 0;
  for (uint16_t i = 0; i < produced; ++i) {
    out.windowArea += rectArea(out.windows[i]);
  }
  out.windowCount = produced;
  if (overflow) out.windowCount = static_cast<uint16_t>(kPlanCap + 1);
  return true;
}

inline Decision decide(bool baselineTrusted, bool everPresented, const Plan& plan, uint32_t partialsSinceFull,
                       uint32_t cumulativePartialPixels) {
  Decision d;
  if (!everPresented) {
    d.mode = Mode::Full;
    d.reason = Reason::FirstBaseline;
    return d;
  }
  if (!baselineTrusted) {
    d.mode = Mode::Full;
    d.reason = Reason::ForcedFullRecovery;
    return d;
  }
  if (plan.changedPixels == 0) {
    d.mode = Mode::Skip;
    d.reason = Reason::NoChange;
    return d;
  }
  if (partialsSinceFull >= kMaxPartialsSinceFull) {
    d.mode = Mode::Full;
    d.reason = Reason::CadenceCount;
    return d;
  }
  if (cumulativePartialPixels + plan.changedPixels >= kMaxCumulativePartialPixels) {
    d.mode = Mode::Full;
    d.reason = Reason::CadenceArea;
    return d;
  }
  if (plan.changedPixels > kMaxPartialChangedPixels) {
    d.mode = Mode::Full;
    d.reason = Reason::DenseArea;
    return d;
  }
  if (plan.windowCount == 0 || plan.windowCount > kMaxWindows) {
    d.mode = Mode::Full;
    d.reason = Reason::Fragmented;
    return d;
  }
  d.mode = Mode::Partial;
  d.reason = Reason::SparsePartial;
  return d;
}

}  // namespace M4PanelDirty
