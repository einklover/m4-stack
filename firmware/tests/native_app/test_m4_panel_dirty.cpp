// Host tests for M4PanelDirty planner + conservative partial policy.
// Build: g++-14 -std=c++14 -Wall -Wextra -Werror -I firmware/src
//        firmware/tests/native_app/test_m4_panel_dirty.cpp

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "util/M4PanelDirty.h"
#include "util/M4PanelMapper.h"
#include "util/M4PanelPresenter.h"

namespace {

using M4PanelDirty::Decision;
using M4PanelDirty::Mode;
using M4PanelDirty::Plan;
using M4PanelDirty::Reason;

std::vector<uint8_t> whitePhys() { return std::vector<uint8_t>(M4PanelDirty::kSize, 0xFF); }

void setBlack(std::vector<uint8_t>& fb, int x, int y) {
  assert(x >= 0 && x < static_cast<int>(M4PanelDirty::kWidth));
  assert(y >= 0 && y < static_cast<int>(M4PanelDirty::kHeight));
  const size_t off = static_cast<size_t>(y) * M4PanelDirty::kStride + static_cast<size_t>(x >> 3);
  fb[off] = static_cast<uint8_t>(fb[off] & ~(0x80u >> (x & 7)));
}

void fill(std::vector<uint8_t>& fb, int x, int y, int w, int h) {
  for (int yy = y; yy < y + h; ++yy) {
    for (int xx = x; xx < x + w; ++xx) setBlack(fb, xx, yy);
  }
}

bool containsPixel(const M4PanelDirty::Rect& r, int x, int y) {
  return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

void assertAligned(const Plan& p) {
  for (uint16_t i = 0; i < p.windowCount && i < M4PanelDirty::kPlanCap; ++i) {
    const M4PanelDirty::Rect& r = p.windows[i];
    assert((r.x % 8) == 0);
    assert((r.w % 8) == 0);
    assert(r.w > 0 && r.h > 0);
    assert(static_cast<uint32_t>(r.x) + r.w <= M4PanelDirty::kWidth);
    assert(static_cast<uint32_t>(r.y) + r.h <= M4PanelDirty::kHeight);
  }
}

}  // namespace

int main() {
  assert(M4PanelDirty::kMaxWindows == 4);
  assert(M4PanelDirty::kMaxPartialChangedPixels == 107520u);
  assert(M4PanelDirty::kMaxHygieneCoveragePixels == 96000u);
  assert(M4PanelDirty::kMaxHygieneChurnPixels == 192000u);
  assert(M4PanelDirty::kHardSafetyPartials == 64);
  assert(M4PanelDirty::kHardSafetyMinCoveragePixels == 16384u);
  assert(M4PanelDirty::kHardSafetyMinChurnPixels == 32768u);
  assert(M4PanelDirty::kMaxPartialsSinceFull == 64);
  assert(M4PanelDirty::kMaxCumulativePartialPixels == 192000u);
  assert(M4PanelDirty::kMergeGapX == 96);
  assert(M4PanelDirty::kMergeGapY == 96);
  assert(M4PanelDirty::kMaxMergeUnionArea == 30720u);
  assert(M4PanelDirty::kCoverageWords == 24);
  assert(M4PanelDirty::kTilePixels == 512u);

  // No-change => skip, no windows.
  {
    auto a = whitePhys();
    auto b = whitePhys();
    Plan p{};
    assert(M4PanelDirty::plan(a.data(), b.data(), a.size(), p));
    assert(p.changedPixels == 0);
    assert(p.windowCount == 0);
    Decision d = M4PanelDirty::decide(true, true, p, 0, 0);
    assert(d.mode == Mode::Skip);
    assert(d.reason == Reason::NoChange);
  }

  // One-pixel / byte-boundary sparse change.
  {
    auto prev = whitePhys();
    auto next = whitePhys();
    setBlack(next, 3, 10);
    Plan p{};
    assert(M4PanelDirty::plan(prev.data(), next.data(), prev.size(), p));
    assert(p.changedPixels == 1);
    assert(p.windowCount == 1);
    assertAligned(p);
    assert(p.windows[0].x == 0);
    assert(p.windows[0].w == 8);
    assert(p.windows[0].y == 10);
    assert(p.windows[0].h == 1);
    assert(containsPixel(p.windows[0], 3, 10));
    Decision d = M4PanelDirty::decide(true, true, p, 0, 0);
    assert(d.mode == Mode::Partial);
    assert(d.reason == Reason::SparsePartial);

    setBlack(next, 8, 10);  // next byte
    assert(M4PanelDirty::plan(prev.data(), next.data(), prev.size(), p));
    assert(p.changedPixels == 2);
    assert(p.windowCount == 1);
    assertAligned(p);
    assert(p.windows[0].x == 0);
    assert(p.windows[0].w == 16);
    assert(containsPixel(p.windows[0], 3, 10));
    assert(containsPixel(p.windows[0], 8, 10));
  }

  // Two separated sparse regions.
  {
    auto prev = whitePhys();
    auto next = whitePhys();
    fill(next, 16, 16, 16, 16);
    fill(next, 700, 400, 16, 16);
    Plan p{};
    assert(M4PanelDirty::plan(prev.data(), next.data(), prev.size(), p));
    assert(p.changedPixels == 16u * 16u * 2u);
    assert(p.windowCount == 2);
    assertAligned(p);
    bool sawA = false, sawB = false;
    for (uint16_t i = 0; i < p.windowCount; ++i) {
      if (containsPixel(p.windows[i], 20, 20)) sawA = true;
      if (containsPixel(p.windows[i], 705, 405)) sawB = true;
    }
    assert(sawA && sawB);
    Decision d = M4PanelDirty::decide(true, true, p, 0, 0);
    assert(d.mode == Mode::Partial);
    assert(d.reason == Reason::SparsePartial);
  }

  // Adjacent same-x tile runs merge into one window.
  {
    auto prev = whitePhys();
    auto next = whitePhys();
    fill(next, 0, 0, 32, 16);
    fill(next, 0, 16, 32, 16);
    Plan p{};
    assert(M4PanelDirty::plan(prev.data(), next.data(), prev.size(), p));
    assert(p.windowCount == 1);
    assertAligned(p);
    assert(p.windows[0].x == 0);
    assert(p.windows[0].y == 0);
    assert(p.windows[0].w == 32);
    assert(p.windows[0].h == 32);
  }

  // Dense area fallback.
  {
    auto prev = whitePhys();
    auto next = whitePhys();
    fill(next, 0, 0, 400, 300);  // 120000 px > 107520
    Plan p{};
    assert(M4PanelDirty::plan(prev.data(), next.data(), prev.size(), p));
    assert(p.changedPixels == 120000u);
    Decision d = M4PanelDirty::decide(true, true, p, 0, 0);
    assert(d.mode == Mode::Full);
    assert(d.reason == Reason::DenseArea);
  }

  // Fragmented fallback: five isolated windows.
  {
    auto prev = whitePhys();
    auto next = whitePhys();
    fill(next, 0, 0, 8, 8);
    fill(next, 200, 0, 8, 8);
    fill(next, 400, 0, 8, 8);
    fill(next, 600, 0, 8, 8);
    fill(next, 768, 400, 8, 8);
    Plan p{};
    assert(M4PanelDirty::plan(prev.data(), next.data(), prev.size(), p));
    assert(p.windowCount == 5);
    assertAligned(p);
    Decision d = M4PanelDirty::decide(true, true, p, 0, 0);
    assert(d.mode == Mode::Full);
    assert(d.reason == Reason::Fragmented);
  }

  // Nearby HUD/glyph gaps on one strip compact to <=4 windows (the live
  // INPUT_TEST counter update: several disconnected digits, changedPixels
  // far below the 28% dense cap, previously windowCount>4 => Fragmented).
  {
    auto prev = whitePhys();
    auto next = whitePhys();
    const int xs[] = {32, 32, 32, 32, 32, 32};
    const int ys[] = {20, 68, 116, 164, 212, 260};
    for (int i = 0; i < 6; ++i) fill(next, xs[i], ys[i], 8, 12);
    Plan p{};
    assert(M4PanelDirty::plan(prev.data(), next.data(), prev.size(), p));
    assert(p.changedPixels == 6u * 8u * 12u);
    assert(p.windowCount >= 1 && p.windowCount <= M4PanelDirty::kMaxWindows);
    assertAligned(p);
    for (int i = 0; i < 6; ++i) {
      bool covered = false;
      for (uint16_t w = 0; w < p.windowCount; ++w) {
        if (containsPixel(p.windows[w], xs[i] + 1, ys[i] + 1)) covered = true;
      }
      assert(covered);
    }
    Decision d = M4PanelDirty::decide(true, true, p, 0, 0);
    assert(d.mode == Mode::Partial);
    assert(d.reason == Reason::SparsePartial);
  }

  // Same pattern after the 90° logical->physical map: a HUD text line is a
  // physical column of glyphs. Digit-only updates must stay Partial.
  {
    auto prevLog = std::vector<uint8_t>(M4PanelMapper::kLogicalSize, 0xFF);
    auto nextLog = prevLog;
    auto setLog = [](std::vector<uint8_t>& fb, int x, int y, int w, int h) {
      for (int yy = y; yy < y + h; ++yy) {
        for (int xx = x; xx < x + w; ++xx) {
          const size_t off = static_cast<size_t>(yy) * M4PanelMapper::kLogicalStride +
                             static_cast<size_t>(xx >> 3);
          fb[off] = static_cast<uint8_t>(fb[off] & static_cast<uint8_t>(~(0x80u >> (xx & 7))));
        }
      }
    };
    // INPUT_TEST HUD is logical (8,250) 464x250; a one-line counter rewrite
    // dirties several 8x12 glyphs across that line.
    const int ly = 252;
    const int lxs[] = {16, 64, 112, 160, 208, 256};
    for (int i = 0; i < 6; ++i) setLog(nextLog, lxs[i], ly, 8, 12);
    auto prevPhy = std::vector<uint8_t>(M4PanelMapper::kPhysicalSize, 0);
    auto nextPhy = std::vector<uint8_t>(M4PanelMapper::kPhysicalSize, 0);
    assert(M4PanelMapper::mapLogicalToPhysical(prevLog.data(), prevLog.size(), prevPhy.data(),
                                               prevPhy.size()) == M4PanelMapper::Status::Ok);
    assert(M4PanelMapper::mapLogicalToPhysical(nextLog.data(), nextLog.size(), nextPhy.data(),
                                               nextPhy.size()) == M4PanelMapper::Status::Ok);
    Plan p{};
    assert(M4PanelDirty::plan(prevPhy.data(), nextPhy.data(), prevPhy.size(), p));
    assert(p.changedPixels == 6u * 8u * 12u);
    assert(p.windowCount >= 1 && p.windowCount <= M4PanelDirty::kMaxWindows);
    assertAligned(p);
    Decision d = M4PanelDirty::decide(true, true, p, 0, 0);
    assert(d.mode == Mode::Partial);
    assert(d.reason == Reason::SparsePartial);
  }

  // INPUT_TEST-like first paint vs white: many distant widgets stay Fragmented.
  // Do not collapse a scattered page into one huge Partial.
  {
    auto prevLog = std::vector<uint8_t>(M4PanelMapper::kLogicalSize, 0xFF);
    auto nextLog = prevLog;
    auto setLog = [](std::vector<uint8_t>& fb, int x, int y, int w, int h) {
      for (int yy = y; yy < y + h; ++yy) {
        for (int xx = x; xx < x + w; ++xx) {
          const size_t off = static_cast<size_t>(yy) * M4PanelMapper::kLogicalStride +
                             static_cast<size_t>(xx >> 3);
          fb[off] = static_cast<uint8_t>(fb[off] & static_cast<uint8_t>(~(0x80u >> (xx & 7))));
        }
      }
    };
    setLog(nextLog, 0, 0, 48, 48);        // TL
    setLog(nextLog, 432, 0, 48, 48);      // TR
    setLog(nextLog, 0, 752, 48, 48);      // BL
    setLog(nextLog, 432, 752, 48, 48);    // BR
    setLog(nextLog, 216, 376, 48, 48);    // CTR
    setLog(nextLog, 64, 140, 72, 36);     // A
    setLog(nextLog, 300, 420, 40, 72);    // B
    setLog(nextLog, 168, 72, 144, 48);    // BTN
    setLog(nextLog, 16, 520, 200, 200);   // SCROLL
    setLog(nextLog, 240, 560, 220, 80);   // DRAG
    setLog(nextLog, 240, 650, 220, 80);   // LP
    auto prevPhy = std::vector<uint8_t>(M4PanelMapper::kPhysicalSize, 0);
    auto nextPhy = std::vector<uint8_t>(M4PanelMapper::kPhysicalSize, 0);
    assert(M4PanelMapper::mapLogicalToPhysical(prevLog.data(), prevLog.size(), prevPhy.data(),
                                               prevPhy.size()) == M4PanelMapper::Status::Ok);
    assert(M4PanelMapper::mapLogicalToPhysical(nextLog.data(), nextLog.size(), nextPhy.data(),
                                               nextPhy.size()) == M4PanelMapper::Status::Ok);
    Plan p{};
    assert(M4PanelDirty::plan(prevPhy.data(), nextPhy.data(), prevPhy.size(), p));
    assert(p.changedPixels > 0);
    assert(p.changedPixels < M4PanelDirty::kMaxPartialChangedPixels);
    assert(p.windowCount > M4PanelDirty::kMaxWindows);
    Decision d = M4PanelDirty::decide(true, true, p, 0, 0);
    assert(d.mode == Mode::Full);
    assert(d.reason == Reason::Fragmented);
  }

  // First / untrusted baseline => FULL.
  {
    auto prev = whitePhys();
    auto next = whitePhys();
    setBlack(next, 0, 0);
    Plan p{};
    assert(M4PanelDirty::plan(prev.data(), next.data(), prev.size(), p));
    Decision first = M4PanelDirty::decide(false, false, p, 0, 0);
    assert(first.mode == Mode::Full);
    assert(first.reason == Reason::FirstBaseline);
    Decision rec = M4PanelDirty::decide(false, true, p, 0, 0);
    assert(rec.mode == Mode::Full);
    assert(rec.reason == Reason::ForcedFullRecovery);
  }

  // Count of 8 (legacy mechanical cadence) is no longer a sole Full/Hygiene trigger.
  {
    auto prev = whitePhys();
    auto next = whitePhys();
    setBlack(next, 40, 40);
    Plan p{};
    assert(M4PanelDirty::plan(prev.data(), next.data(), prev.size(), p));
    Decision d = M4PanelDirty::decide(true, true, p, 8, 8, 512);
    assert(d.mode == Mode::Partial);
    assert(d.reason == Reason::SparsePartial);
  }

  // 32 tiny same-tile sparse updates stay Partial (no count-only hygiene).
  {
    auto prev = whitePhys();
    auto next = whitePhys();
    setBlack(next, 40, 40);
    Plan p{};
    assert(M4PanelDirty::plan(prev.data(), next.data(), prev.size(), p));
    uint32_t churn = 0;
    for (uint32_t n = 0; n < 32; ++n) {
      Decision d = M4PanelDirty::decide(true, true, p, n, churn, 512);
      assert(d.mode == Mode::Partial);
      assert(d.reason == Reason::SparsePartial);
      churn = M4PanelDirty::satAdd(churn, p.changedPixels);
    }
  }

  // Unique tile coverage since last clean => Hygiene CadenceArea.
  {
    auto prev = whitePhys();
    auto next = whitePhys();
    setBlack(next, 40, 40);
    Plan p{};
    assert(M4PanelDirty::plan(prev.data(), next.data(), prev.size(), p));
    Decision d = M4PanelDirty::decide(true, true, p, 2, 100,
                                      M4PanelDirty::kMaxHygieneCoveragePixels);
    assert(d.mode == Mode::Hygiene);
    assert(d.reason == Reason::CadenceArea);
    d = M4PanelDirty::decide(true, true, p, 2, 100, M4PanelDirty::kMaxHygieneCoveragePixels - 1);
    assert(d.mode == Mode::Partial);
  }

  // Transition churn => Hygiene CadenceCount. Toggle of one tile is bounded
  // until accumulated changed pixels cross the churn ceiling.
  {
    auto prev = whitePhys();
    auto next = whitePhys();
    fill(next, 0, 0, 16, 16);  // 256 px
    Plan p{};
    assert(M4PanelDirty::plan(prev.data(), next.data(), prev.size(), p));
    Decision d = M4PanelDirty::decide(true, true, p, 5,
                                      M4PanelDirty::kMaxHygieneChurnPixels - p.changedPixels, 512);
    assert(d.mode == Mode::Hygiene);
    assert(d.reason == Reason::CadenceCount);
    d = M4PanelDirty::decide(true, true, p, 5, 0, 512);
    assert(d.mode == Mode::Partial);
  }

  // Hard safety ceiling requires evidence. 64 tiny updates with no coverage
  // or churn stay Partial; 64 + min coverage/churn is Hygiene.
  {
    auto prev = whitePhys();
    auto next = whitePhys();
    setBlack(next, 8, 8);
    Plan p{};
    assert(M4PanelDirty::plan(prev.data(), next.data(), prev.size(), p));
    Decision d = M4PanelDirty::decide(true, true, p, M4PanelDirty::kHardSafetyPartials, 100, 100);
    assert(d.mode == Mode::Partial);
    d = M4PanelDirty::decide(true, true, p, M4PanelDirty::kHardSafetyPartials, 100,
                             M4PanelDirty::kHardSafetyMinCoveragePixels);
    assert(d.mode == Mode::Hygiene);
    assert(d.reason == Reason::CadenceCount);
    d = M4PanelDirty::decide(true, true, p, M4PanelDirty::kHardSafetyPartials,
                             M4PanelDirty::kHardSafetyMinChurnPixels, 100);
    assert(d.mode == Mode::Hygiene);
    assert(d.reason == Reason::CadenceCount);
    d = M4PanelDirty::decide(true, true, p, M4PanelDirty::kHardSafetyPartials - 1, 100,
                             M4PanelDirty::kHardSafetyMinCoveragePixels);
    assert(d.mode == Mode::Partial);
  }

  // Coverage bitmap: marking one 32x16 window is exactly one tile.
  {
    uint32_t bits[M4PanelDirty::kCoverageWords];
    M4PanelDirty::clearCoverage(bits);
    assert(M4PanelDirty::coveragePixels(bits) == 0);
    Plan p{};
    p.windowCount = 1;
    p.windows[0] = M4PanelDirty::Rect{32, 16, 32, 16};
    M4PanelDirty::markPlanCoverage(bits, p);
    assert(M4PanelDirty::coverageTileCount(bits) == 1);
    assert(M4PanelDirty::coveragePixels(bits) == 512);
    M4PanelDirty::markPlanCoverage(bits, p);  // idempotent
    assert(M4PanelDirty::coverageTileCount(bits) == 1);
  }

  // Coalesced newest-frame dirty is vs last successful physical, not skipped B.
  {
    auto presented = whitePhys();
    auto skippedB = whitePhys();
    auto newestC = whitePhys();
    fill(skippedB, 0, 0, 64, 64);
    fill(newestC, 400, 200, 16, 16);
    Plan vsB{};
    Plan vsC{};
    assert(M4PanelDirty::plan(presented.data(), skippedB.data(), presented.size(), vsB));
    assert(M4PanelDirty::plan(presented.data(), newestC.data(), presented.size(), vsC));
    assert(vsB.changedPixels == 64u * 64u);
    assert(vsC.changedPixels == 16u * 16u);
    assert(vsC.windowCount == 1);
    assert(containsPixel(vsC.windows[0], 400, 200));
    assert(!containsPixel(vsC.windows[0], 8, 8));
  }

  // Bad size fails closed.
  {
    auto a = whitePhys();
    Plan p{};
    assert(!M4PanelDirty::plan(a.data(), a.data(), 16, p));
    assert(!M4PanelDirty::plan(nullptr, a.data(), a.size(), p));
  }

  // #34 boundary: nearby-window merge must not bypass CadenceCount / FirstBaseline /
  // ForcedFullRecovery. A HUD-like coalesced plan is still SparsePartial only while
  // the hygiene counters allow it.
  {
    auto prev = whitePhys();
    auto next = whitePhys();
    const int xs[] = {32, 32, 32, 32, 32, 32};
    const int ys[] = {20, 68, 116, 164, 212, 260};
    for (int i = 0; i < 6; ++i) fill(next, xs[i], ys[i], 8, 12);
    Plan p{};
    assert(M4PanelDirty::plan(prev.data(), next.data(), prev.size(), p));
    assert(p.windowCount >= 1 && p.windowCount <= M4PanelDirty::kMaxWindows);
    Decision sparse = M4PanelDirty::decide(true, true, p, 0, 0);
    assert(sparse.mode == Mode::Partial);
    assert(sparse.reason == Reason::SparsePartial);
    Decision cadN = M4PanelDirty::decide(true, true, p, M4PanelDirty::kMaxPartialsSinceFull, 0);
    assert(cadN.mode == Mode::Full);
    assert(cadN.reason == Reason::CadenceCount);
    Decision cadNMinus = M4PanelDirty::decide(true, true, p, M4PanelDirty::kMaxPartialsSinceFull - 1, 0);
    assert(cadNMinus.mode == Mode::Partial);
    assert(cadNMinus.reason == Reason::SparsePartial);
    Decision cadA = M4PanelDirty::decide(true, true, p, 0,
                                         M4PanelDirty::kMaxCumulativePartialPixels - p.changedPixels);
    assert(cadA.mode == Mode::Full);
    assert(cadA.reason == Reason::CadenceArea);
    Decision first = M4PanelDirty::decide(false, false, p, M4PanelDirty::kMaxPartialsSinceFull, 0);
    assert(first.mode == Mode::Full);
    assert(first.reason == Reason::FirstBaseline);
    Decision rec = M4PanelDirty::decide(false, true, p, 0, 0);
    assert(rec.mode == Mode::Full);
    assert(rec.reason == Reason::ForcedFullRecovery);
  }

  // Merge gap is inclusive at 96px and exclusive at 97px when compaction is needed.
  {
    auto prev = whitePhys();
    auto next = whitePhys();
    const int ys96[] = {0, 104, 208, 312, 416};  // 8px tall, gap 96
    for (int i = 0; i < 5; ++i) fill(next, 0, ys96[i], 8, 8);
    Plan p{};
    assert(M4PanelDirty::plan(prev.data(), next.data(), prev.size(), p));
    assert(p.windowCount >= 1 && p.windowCount <= M4PanelDirty::kMaxWindows);
    assertAligned(p);
    Decision d = M4PanelDirty::decide(true, true, p, 0, 0);
    assert(d.mode == Mode::Partial);
    assert(d.reason == Reason::SparsePartial);

    next = whitePhys();
    const int ys97[] = {0, 105, 210, 315, 420};  // 8px tall, gap 97
    for (int i = 0; i < 5; ++i) fill(next, 0, ys97[i], 8, 8);
    assert(M4PanelDirty::plan(prev.data(), next.data(), prev.size(), p));
    assert(p.windowCount == 5);
    assertAligned(p);
    d = M4PanelDirty::decide(true, true, p, 0, 0);
    assert(d.mode == Mode::Full);
    assert(d.reason == Reason::Fragmented);
  }

  // Union-area cap: nearby medium widgets must not chain-swallow into a Partial.
  {
    auto prev = whitePhys();
    auto next = whitePhys();
    // 34px gaps sit in a clean 32px tile so tile-runs stay split, but the
    // 96px nearby-merge would still consider them — union-area must refuse.
    fill(next, 0, 0, 190, 80);
    fill(next, 224, 0, 190, 80);
    fill(next, 448, 0, 190, 80);
    fill(next, 0, 96, 190, 80);
    fill(next, 224, 96, 190, 80);
    Plan p{};
    assert(M4PanelDirty::plan(prev.data(), next.data(), prev.size(), p));
    assert(p.changedPixels == 5u * 190u * 80u);
    assert(p.changedPixels < M4PanelDirty::kMaxPartialChangedPixels);
    assert(p.windowCount > M4PanelDirty::kMaxWindows);
    assertAligned(p);
    Decision d = M4PanelDirty::decide(true, true, p, 0, 0);
    assert(d.mode == Mode::Full);
    assert(d.reason == Reason::Fragmented);
  }

  // Overflowed plans stay Fragmented: merge must not run on a truncated cover.
  {
    auto prev = whitePhys();
    auto next = whitePhys();
    int n = 0;
    for (int y = 0; y < 480 && n < 18; y += 32) {
      for (int x = 0; x < 800 && n < 18; x += 160) {
        setBlack(next, x, y);
        ++n;
      }
    }
    assert(n == 18);
    Plan p{};
    assert(M4PanelDirty::plan(prev.data(), next.data(), prev.size(), p));
    assert(p.changedPixels == 18u);
    assert(p.windowCount == M4PanelDirty::kPlanCap + 1);
    Decision d = M4PanelDirty::decide(true, true, p, 0, 0);
    assert(d.mode == Mode::Full);
    assert(d.reason == Reason::Fragmented);
  }

  // Determinism: the same HUD strip yields a stable Partial plan.
  {
    auto prev = whitePhys();
    auto next = whitePhys();
    const int xs[] = {32, 32, 32, 32, 32, 32};
    const int ys[] = {20, 68, 116, 164, 212, 260};
    for (int i = 0; i < 6; ++i) fill(next, xs[i], ys[i], 8, 12);
    Plan first{};
    assert(M4PanelDirty::plan(prev.data(), next.data(), prev.size(), first));
    for (int i = 0; i < 64; ++i) {
      Plan p{};
      assert(M4PanelDirty::plan(prev.data(), next.data(), prev.size(), p));
      assert(p.changedPixels == first.changedPixels);
      assert(p.windowCount == first.windowCount);
      assert(p.windowArea == first.windowArea);
      Decision d = M4PanelDirty::decide(true, true, p, 0, 0);
      assert(d.mode == Mode::Partial);
      assert(d.reason == Reason::SparsePartial);
    }
  }

  // Presenter: first / release-reacquire / cadence / failure does not advance policy.
  {
    M4PanelPresenter::Scheduler s;
    s.acquire(M4PanelPresenter::Owner::BrowserBridge);
    assert(!s.state().baselineTrusted);
    assert(!s.state().everPresented);
    auto prev = whitePhys();
    auto next = whitePhys();
    setBlack(next, 8, 8);
    Plan p{};
    assert(M4PanelDirty::plan(prev.data(), next.data(), prev.size(), p));
    Decision d = s.decide(p);
    assert(d.mode == Mode::Full);
    assert(d.reason == Reason::FirstBaseline);
    s.offer(1, 0x11, 0);
    assert(s.take(0) == M4PanelPresenter::TakeStatus::Ready);
    s.complete(true, 0xAA, 10, 0, Mode::Full, 1800, p.changedPixels, p.windowArea, p.windowCount, d.reason);
    assert(s.state().fullOk == 1);
    assert(s.state().baselineTrusted);
    assert(s.state().partialsSinceFull == 0);

    s.offer(2, 0x22, 2010);
    assert(s.take(2010) == M4PanelPresenter::TakeStatus::Ready);
    d = s.decide(p);
    assert(d.mode == Mode::Partial);
    s.complete(true, 0xBB, 2200, 0, Mode::Partial, 220, p.changedPixels, p.windowArea, p.windowCount, d.reason);
    assert(s.state().partialOk == 1);
    assert(s.state().partialsSinceFull == 1);
    assert(s.state().cumulativePartialPixels == p.changedPixels);

    // Injected partial failure: no completed advance, baseline untrusted.
    s.injectNextFailure();
    assert(s.consumeInjectedFailure());
    s.offer(3, 0x33, 4300);
    assert(s.take(4300) == M4PanelPresenter::TakeStatus::Ready);
    const uint32_t okBefore = s.state().completed;
    const uint32_t panelCrcBefore = s.state().lastPanelCrc;
    s.complete(false, 0, 4310, static_cast<uint32_t>(M4PanelPresenter::Error::DisplayFailed), Mode::Partial,
               0, p.changedPixels, p.windowArea, p.windowCount, Reason::SparsePartial);
    assert(s.state().completed == okBefore);
    assert(s.state().lastPanelCrc == panelCrcBefore);
    assert(s.state().partialErr == 1);
    assert(!s.state().baselineTrusted);
    assert(s.state().pending);
    d = s.decide(p);
    assert(d.mode == Mode::Full);
    assert(d.reason == Reason::ForcedFullRecovery);

    s.release();
    assert(!s.browserOwns());
    assert(!s.state().baselineTrusted);
    assert(!s.state().everPresented);
    assert(s.acquire(M4PanelPresenter::Owner::BrowserBridge));
    d = s.decide(p);
    assert(d.mode == Mode::Full);
    assert(d.reason == Reason::FirstBaseline);
  }

  // Scheduler: 32 tiny same-tile Partials never count-only Hygiene/Full.
  {
    M4PanelPresenter::Scheduler s;
    s.setMinIntervalMs(0);
    s.acquire(M4PanelPresenter::Owner::BrowserBridge);
    auto prev = whitePhys();
    auto next = whitePhys();
    setBlack(next, 24, 24);
    Plan p{};
    assert(M4PanelDirty::plan(prev.data(), next.data(), prev.size(), p));
    s.offer(1, 1, 0);
    s.take(0);
    s.complete(true, 1, 1, 0, Mode::Full, 10, 0, 0, 0, Reason::FirstBaseline);
    for (uint32_t i = 0; i < 32; ++i) {
      s.offer(static_cast<int32_t>(i + 2), i + 2, 10 + i);
      s.take(10 + i);
      Decision d = s.decide(p);
      assert(d.mode == Mode::Partial);
      assert(d.reason == Reason::SparsePartial);
      s.notePolicy(d.mode, d.reason, p.changedPixels, p.windowArea, p.windowCount, &p);
      s.complete(true, i + 2, 11 + i, 0, Mode::Partial, 5, p.changedPixels, p.windowArea, p.windowCount,
                 d.reason);
    }
    assert(s.state().partialOk == 32);
    assert(s.state().hygieneReq == 0);
    assert(s.state().fullOk == 1);
    assert(s.state().uniqueCoveragePixels == 512);
    Decision d = s.decide(p);
    assert(d.mode == Mode::Partial);
  }

  // Broad unique coverage eventually Hygienes and resets counters.
  {
    M4PanelPresenter::Scheduler s;
    s.setMinIntervalMs(0);
    s.acquire(M4PanelPresenter::Owner::BrowserBridge);
    s.offer(1, 1, 0);
    s.take(0);
    s.complete(true, 1, 1, 0, Mode::Full, 10, 0, 0, 0, Reason::FirstBaseline);

    auto presented = whitePhys();
    auto candidate = whitePhys();
    // Add 256x16 strips (8 tiles) so unique coverage crosses 25% before the
    // 64-Partial safety ceiling can fire.
    uint32_t steps = 0;
    while (s.state().uniqueCoveragePixels < M4PanelDirty::kMaxHygieneCoveragePixels) {
      const int y = static_cast<int>(steps * M4PanelDirty::kTileH);
      assert(y + M4PanelDirty::kTileH <= static_cast<int>(M4PanelDirty::kHeight));
      fill(candidate, 0, y, 256, M4PanelDirty::kTileH);
      Plan p{};
      assert(M4PanelDirty::plan(presented.data(), candidate.data(), presented.size(), p));
      assert(p.windowCount >= 1);
      s.offer(static_cast<int32_t>(steps + 2), steps + 2, 20 + steps);
      s.take(20 + steps);
      Decision d = s.decide(p);
      if (d.mode == Mode::Hygiene) {
        assert(d.reason == Reason::CadenceArea);
        s.notePolicy(d.mode, d.reason, p.changedPixels, p.windowArea, p.windowCount, &p);
        s.complete(true, steps + 2, 21 + steps, 0, Mode::Hygiene, 1700, p.changedPixels, p.windowArea,
                   p.windowCount, d.reason);
        assert(s.state().hygieneOk == 1);
        assert(s.state().partialsSinceFull == 0);
        assert(s.state().cumulativePartialPixels == 0);
        assert(s.state().uniqueCoveragePixels == 0);
        presented.swap(candidate);
        break;
      }
      assert(d.mode == Mode::Partial);
      s.notePolicy(d.mode, d.reason, p.changedPixels, p.windowArea, p.windowCount, &p);
      s.complete(true, steps + 2, 21 + steps, 0, Mode::Partial, 5, p.changedPixels, p.windowArea,
                 p.windowCount, d.reason);
      presented.swap(candidate);
      candidate = presented;
      ++steps;
      assert(steps < 40);
    }
    assert(s.state().hygieneOk == 1);
    auto tiny = presented;
    setBlack(tiny, 400, 8);
    Plan p{};
    assert(M4PanelDirty::plan(presented.data(), tiny.data(), presented.size(), p));
    s.offer(400, 400, 400);
    s.take(400);
    Decision d = s.decide(p);
    assert(d.mode == Mode::Partial);
  }

  // Hygiene failure does not reset counters and retries as ForcedFullRecovery.
  {
    M4PanelPresenter::Scheduler s;
    s.setMinIntervalMs(0);
    s.acquire(M4PanelPresenter::Owner::BrowserBridge);
    auto prev = whitePhys();
    auto next = whitePhys();
    setBlack(next, 24, 24);
    Plan p{};
    assert(M4PanelDirty::plan(prev.data(), next.data(), prev.size(), p));
    s.offer(1, 1, 0);
    s.take(0);
    s.complete(true, 1, 1, 0, Mode::Full, 10, 0, 0, 0, Reason::FirstBaseline);
    Plan wide{};
    wide.windowCount = 1;
    wide.windows[0] = M4PanelDirty::Rect{0, 0, M4PanelDirty::kWidth, 240};  // half panel of tiles
    s.notePolicy(Mode::Partial, Reason::SparsePartial, 1, 1, 1, &wide);
    assert(s.state().uniqueCoveragePixels >= M4PanelDirty::kMaxHygieneCoveragePixels);
    s.offer(2, 2, 20);
    s.take(20);
    Decision d = s.decide(p);
    assert(d.mode == Mode::Hygiene);
    assert(d.reason == Reason::CadenceArea);
    const uint32_t covBefore = s.state().uniqueCoveragePixels;
    s.notePolicy(d.mode, d.reason, p.changedPixels, p.windowArea, p.windowCount, &p);
    s.complete(false, 0, 21, static_cast<uint32_t>(M4PanelPresenter::Error::DisplayFailed), Mode::Hygiene,
               10, p.changedPixels, p.windowArea, p.windowCount, d.reason);
    assert(s.state().hygieneErr == 1);
    assert(s.state().hygieneOk == 0);
    assert(!s.state().baselineTrusted);
    assert(s.state().uniqueCoveragePixels == covBefore);
    assert(s.state().pending);
    d = s.decide(p);
    assert(d.mode == Mode::Full);
    assert(d.reason == Reason::ForcedFullRecovery);
  }

  printf("test_m4_panel_dirty: PASS\n");
  return 0;
}
