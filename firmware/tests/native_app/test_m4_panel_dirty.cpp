// Host tests for M4PanelDirty planner + conservative partial policy.
// Build: g++-14 -std=c++14 -Wall -Wextra -Werror -I firmware/src
//        firmware/tests/native_app/test_m4_panel_dirty.cpp

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "util/M4PanelDirty.h"
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
  assert(M4PanelDirty::kMaxPartialsSinceFull == 8);
  assert(M4PanelDirty::kMaxCumulativePartialPixels == 384000u);

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

  // Repeated partials => forced full cadence.
  {
    auto prev = whitePhys();
    auto next = whitePhys();
    setBlack(next, 40, 40);
    Plan p{};
    assert(M4PanelDirty::plan(prev.data(), next.data(), prev.size(), p));
    Decision d = M4PanelDirty::decide(true, true, p, M4PanelDirty::kMaxPartialsSinceFull, 0);
    assert(d.mode == Mode::Full);
    assert(d.reason == Reason::CadenceCount);
    d = M4PanelDirty::decide(true, true, p, M4PanelDirty::kMaxPartialsSinceFull - 1, 0);
    assert(d.mode == Mode::Partial);
  }

  // Cumulative-area forced full.
  {
    auto prev = whitePhys();
    auto next = whitePhys();
    fill(next, 0, 0, 16, 16);  // 256 px
    Plan p{};
    assert(M4PanelDirty::plan(prev.data(), next.data(), prev.size(), p));
    Decision d = M4PanelDirty::decide(true, true, p, 0, M4PanelDirty::kMaxCumulativePartialPixels - 256);
    assert(d.mode == Mode::Full);
    assert(d.reason == Reason::CadenceArea);
    d = M4PanelDirty::decide(true, true, p, 0, 0);
    assert(d.mode == Mode::Partial);
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

  // Cadence counters on scheduler: 8 partials then forced full, then reset.
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
    for (uint32_t i = 0; i < M4PanelDirty::kMaxPartialsSinceFull; ++i) {
      s.offer(static_cast<int32_t>(i + 2), i + 2, 10 + i);
      s.take(10 + i);
      Decision d = s.decide(p);
      assert(d.mode == Mode::Partial);
      s.complete(true, i + 2, 11 + i, 0, Mode::Partial, 5, p.changedPixels, p.windowArea, p.windowCount,
                 d.reason);
    }
    assert(s.state().partialOk == M4PanelDirty::kMaxPartialsSinceFull);
    Decision d = s.decide(p);
    assert(d.mode == Mode::Full);
    assert(d.reason == Reason::CadenceCount);
    s.offer(20, 20, 100);
    s.take(100);
    s.complete(true, 20, 101, 0, Mode::Full, 12, p.changedPixels, p.windowArea, p.windowCount, d.reason);
    assert(s.state().partialsSinceFull == 0);
    assert(s.state().cumulativePartialPixels == 0);
    d = s.decide(p);
    assert(d.mode == Mode::Partial);
  }

  printf("test_m4_panel_dirty: PASS\n");
  return 0;
}
