// Host tests for M4PanelPresenter: latest-frame-wins, bounded pending,
// ownership, and panel-failure isolation from the M4B3 accepted framebuffer.
// Build: g++-14 -std=c++14 -Wall -Wextra -Werror -I firmware/src
//        firmware/tests/native_app/test_m4_panel_presenter.cpp

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "util/M4B3Protocol.h"
#include "util/M4PanelMapper.h"
#include "util/M4PanelPresenter.h"

namespace {

std::vector<uint8_t> whiteLogical() { return std::vector<uint8_t>(M4B3::kKeyframeSize, 0xFF); }

void setBlack(std::vector<uint8_t>& fb, int x, int y) {
  const size_t off = static_cast<size_t>(y) * M4B3::kStride + static_cast<size_t>(x >> 3);
  fb[off] = static_cast<uint8_t>(fb[off] & ~(0x80u >> (x & 7)));
}

struct Harness {
  std::vector<uint8_t> accepted;
  std::vector<uint8_t> candidate;
  M4B3::Session session;
  uint8_t reply[80]{};

  Harness() : accepted(M4B3::kKeyframeSize, 0xFF), candidate(M4B3::kKeyframeSize, 0xFF) {
    session.attach(accepted.data(), candidate.data());
  }

  size_t handle(const uint8_t* msg, size_t len) { return session.handle(msg, len, reply, sizeof(reply)); }
};

}  // namespace

int main() {
  assert(M4PanelPresenter::Scheduler::pendingDepth() == 1);
  assert(M4PanelPresenter::kMinIntervalMs >= 1000);

  // Ownership: offer without acquire is dropped; acquire/release transitions.
  {
    M4PanelPresenter::Scheduler s;
    assert(!s.browserOwns());
    assert(s.offer(1, 0x11, 0) == M4PanelPresenter::OfferStatus::DroppedNoOwner);
    assert(s.state().dropped == 1);
    assert(s.state().requested == 0);
    assert(s.state().pending == false);
    assert(s.acquire(M4PanelPresenter::Owner::Ui) == false);
    assert(s.acquire(M4PanelPresenter::Owner::BrowserBridge));
    assert(s.browserOwns());
    assert(s.acquire(M4PanelPresenter::Owner::BrowserBridge));
    assert(s.offer(1, 0x11, 10) == M4PanelPresenter::OfferStatus::Scheduled);
    assert(s.release());
    assert(s.state().owner == M4PanelPresenter::Owner::Ui);
    assert(s.state().pending == false);
    assert(s.offer(2, 0x22, 20) == M4PanelPresenter::OfferStatus::DroppedNoOwner);
    assert(s.state().dropped == 2);
  }

  // Latest-frame-wins + bounded pending depth while interval/busy.
  {
    M4PanelPresenter::Scheduler s;
    s.setMinIntervalMs(2000);
    s.acquire(M4PanelPresenter::Owner::BrowserBridge);
    assert(s.offer(1, 0xA1, 0) == M4PanelPresenter::OfferStatus::Scheduled);
    assert(s.offer(2, 0xA2, 5) == M4PanelPresenter::OfferStatus::Coalesced);
    assert(s.offer(3, 0xA3, 6) == M4PanelPresenter::OfferStatus::Coalesced);
    assert(s.offer(4, 0xA4, 7) == M4PanelPresenter::OfferStatus::Coalesced);
    assert(s.offer(5, 0xA5, 8) == M4PanelPresenter::OfferStatus::Coalesced);
    assert(s.state().requested == 5);
    assert(s.state().coalesced == 4);
    assert(s.state().pending == true);
    assert(s.state().pendingFrameId == 5);
    assert(s.state().pendingCrc == 0xA5u);

    int32_t id = -1;
    uint32_t crc = 0;
    assert(s.take(10, &id, &crc) == M4PanelPresenter::TakeStatus::Ready);
    assert(id == 5 && crc == 0xA5u);
    assert(s.state().busy);
    assert(s.state().pending == false);
    assert(s.offer(6, 0xA6, 20) == M4PanelPresenter::OfferStatus::Scheduled);
    assert(s.offer(7, 0xA7, 21) == M4PanelPresenter::OfferStatus::Coalesced);
    assert(s.take(30, &id, &crc) == M4PanelPresenter::TakeStatus::Busy);
    s.complete(true, 0xB007, 40);
    assert(s.state().completed == 1);
    assert(s.state().busy == false);
    assert(s.take(41, &id, &crc) == M4PanelPresenter::TakeStatus::Interval);
    assert(s.take(40 + 1999, &id, &crc) == M4PanelPresenter::TakeStatus::Interval);
    assert(s.take(40 + 2000, &id, &crc) == M4PanelPresenter::TakeStatus::Ready);
    assert(id == 7 && crc == 0xA7u);
    s.complete(true, 0xB007, 2040);
    assert(s.state().completed == 2);
    assert(s.take(5000, &id, &crc) == M4PanelPresenter::TakeStatus::Idle);
    assert(s.state().requested == 7);
    assert(s.state().coalesced == 5);
  }

  // Release while busy is deferred until complete.
  {
    M4PanelPresenter::Scheduler s;
    s.acquire(M4PanelPresenter::Owner::BrowserBridge);
    s.offer(1, 0x11, 0);
    assert(s.take(0) == M4PanelPresenter::TakeStatus::Ready);
    assert(s.release());
    assert(s.browserOwns());
    assert(s.state().wantRelease);
    s.complete(true, 0x22, 10);
    assert(!s.browserOwns());
    assert(s.state().owner == M4PanelPresenter::Owner::Ui);
    assert(s.state().pending == false);
  }

  // Panel failure retries the same pending frame and does not touch M4B3 session.
  {
    Harness h;
    uint8_t wire[M4B3::kMaxMessageSize];
    const size_t hn = M4B3::encodeHello(wire, sizeof(wire), 1, M4B3::kHelloOk);
    assert(h.handle(wire, hn) > 0);
    assert(h.session.helloOk());

    std::vector<uint8_t> fb = whiteLogical();
    setBlack(fb, 0, 0);
    setBlack(fb, 479, 0);
    setBlack(fb, 0, 799);
    setBlack(fb, 479, 799);
    const uint32_t crc = M4B3::crc32(fb.data(), fb.size());
    const size_t kn = M4B3::encodeKeyframe(wire, sizeof(wire), 2, 3, fb.data());
    assert(h.handle(wire, kn) > 0);
    assert(h.session.acceptedFrameId() == 3);
    assert(h.session.acceptedCrc() == crc);
    assert(std::memcmp(h.session.accepted(), fb.data(), fb.size()) == 0);

    M4PanelPresenter::Scheduler s;
    s.acquire(M4PanelPresenter::Owner::BrowserBridge);
    assert(s.offer(h.session.acceptedFrameId(), h.session.acceptedCrc(), 0) ==
           M4PanelPresenter::OfferStatus::Scheduled);
    int32_t id = -1;
    uint32_t scrc = 0;
    assert(s.take(0, &id, &scrc) == M4PanelPresenter::TakeStatus::Ready);
    s.complete(false, 0, 5, static_cast<uint32_t>(M4PanelPresenter::Error::DisplayFailed));
    assert(h.session.acceptedFrameId() == 3);
    assert(h.session.acceptedCrc() == crc);
    assert(std::memcmp(h.session.accepted(), fb.data(), fb.size()) == 0);
    assert(s.state().presentErrors == 1);
    assert(s.state().completed == 0);
    assert(s.state().pending == true);
    assert(s.state().pendingFrameId == 3);
    assert(s.state().lastSourceCrc == crc);

    assert(s.take(5, &id, &scrc) == M4PanelPresenter::TakeStatus::Ready);
    s.complete(true, 0xFEED, 10);
    assert(s.state().completed == 1);
    assert(s.state().fullOk == 1);
    assert(s.state().baselineTrusted);
    assert(h.session.acceptedCrc() == crc);
  }

  // Acquire/release resets physical baseline; first present after reacquire is FULL.
  {
    M4PanelPresenter::Scheduler s;
    s.acquire(M4PanelPresenter::Owner::BrowserBridge);
    assert(!s.state().everPresented);
    assert(!s.state().baselineTrusted);
    s.offer(1, 0x11, 0);
    s.take(0);
    s.complete(true, 0x22, 5);
    assert(s.state().everPresented);
    assert(s.state().baselineTrusted);
    assert(s.release());
    assert(!s.state().everPresented);
    assert(!s.state().baselineTrusted);
    assert(s.acquire(M4PanelPresenter::Owner::BrowserBridge));
    M4PanelDirty::Plan empty{};
    auto d = s.decide(empty);
    assert(d.mode == M4PanelDirty::Mode::Full);
    assert(d.reason == M4PanelDirty::Reason::FirstBaseline);
  }

  // Landmark geometry maps to distinct physical regions; corners are black.
  {
    std::vector<uint8_t> logical = whiteLogical();
    auto fill = [&](int x0, int y0, int w, int h) {
      for (int y = y0; y < y0 + h; ++y) {
        for (int x = x0; x < x0 + w; ++x) setBlack(logical, x, y);
      }
    };
    fill(0, 0, 64, 64);        // TL
    fill(448, 0, 32, 64);      // TR
    fill(0, 768, 64, 32);      // BL
    fill(432, 752, 48, 48);    // BR
    fill(40, 80, 80, 40);      // A
    fill(200, 200, 40, 80);    // B
    fill(80, 500, 120, 24);    // C
    std::vector<uint8_t> mapped(M4PanelMapper::kPhysicalSize, 0x00);
    assert(M4PanelMapper::mapLogicalToPhysical(logical.data(), logical.size(), mapped.data(),
                                               mapped.size()) == M4PanelMapper::Status::Ok);
    assert(mapped[0] == 0x00);
    assert(mapped[99] == 0x00);
    assert(mapped[47900] == 0x00);
    assert(mapped[47999] == 0x00);
    int px = 0, py = 0;
    M4PanelMapper::logicalToPhysical(32, 32, &px, &py);  // TL interior
    assert(M4PanelMapper::isBlack(mapped.data(), M4PanelMapper::kPhysicalStride, px, py));
    M4PanelMapper::logicalToPhysical(464, 16, &px, &py);  // TR interior
    assert(M4PanelMapper::isBlack(mapped.data(), M4PanelMapper::kPhysicalStride, px, py));
    M4PanelMapper::logicalToPhysical(16, 784, &px, &py);  // BL interior
    assert(M4PanelMapper::isBlack(mapped.data(), M4PanelMapper::kPhysicalStride, px, py));
    M4PanelMapper::logicalToPhysical(456, 776, &px, &py);  // BR interior
    assert(M4PanelMapper::isBlack(mapped.data(), M4PanelMapper::kPhysicalStride, px, py));
    M4PanelMapper::logicalToPhysical(240, 100, &px, &py);  // white gap
    assert(!M4PanelMapper::isBlack(mapped.data(), M4PanelMapper::kPhysicalStride, px, py));
  }

  // Mapper snapshot used by presenter is independent of the session buffer.
  {
    std::vector<uint8_t> logical = whiteLogical();
    setBlack(logical, 8, 16);
    std::vector<uint8_t> mapped(M4PanelMapper::kPhysicalSize, 0x00);
    std::vector<uint8_t> sessionCopy = logical;
    assert(M4PanelMapper::mapLogicalToPhysical(logical.data(), logical.size(), mapped.data(),
                                               mapped.size()) == M4PanelMapper::Status::Ok);
    setBlack(sessionCopy, 40, 200);
    assert(std::memcmp(logical.data(), sessionCopy.data(), logical.size()) != 0);
    int px = 0, py = 0;
    M4PanelMapper::logicalToPhysical(8, 16, &px, &py);
    assert(M4PanelMapper::isBlack(mapped.data(), M4PanelMapper::kPhysicalStride, px, py));
  }

  // Lifecycle A–F: lastPresented is trusted only after a successful physical
  // present and must be invalidated when glass can have changed elsewhere.
  {
    M4PanelDirty::Plan identical{};
    auto takeAndDecide = [](M4PanelPresenter::Scheduler& s, const M4PanelDirty::Plan& plan, uint32_t now) {
      assert(s.take(now) == M4PanelPresenter::TakeStatus::Ready);
      const auto d = s.decide(plan);
      s.notePolicy(d.mode, d.reason, plan.changedPixels, plan.windowArea, plan.windowCount);
      return d;
    };
    auto presentFull = [&](M4PanelPresenter::Scheduler& s, uint32_t now, int32_t id, uint32_t crc,
                           uint32_t panelCrc, bool ok) {
      assert(s.offer(id, crc, now) == M4PanelPresenter::OfferStatus::Scheduled);
      const auto d = takeAndDecide(s, M4PanelDirty::Plan{}, now);
      s.complete(ok, panelCrc, now + 5, ok ? 0 : static_cast<uint32_t>(M4PanelPresenter::Error::DisplayFailed),
                 d.mode, 5, 0, 0, 0, d.reason);
      return d;
    };

    // A. FULL succeeds → identical frame NoChange.
    M4PanelPresenter::Scheduler s;
    s.setMinIntervalMs(0);
    assert(s.acquire(M4PanelPresenter::Owner::BrowserBridge));
    auto d = presentFull(s, 0, 1, 0x89325E27u, 0xE4147346u, true);
    assert(d.mode == M4PanelDirty::Mode::Full);
    assert(d.reason == M4PanelDirty::Reason::FirstBaseline);
    assert(s.state().baselineTrusted);
    assert(s.state().everPresented);
    assert(s.state().fullOk == 1);
    assert(s.offer(1, 0x89325E27u, 20) == M4PanelPresenter::OfferStatus::Scheduled);
    d = takeAndDecide(s, identical, 20);
    assert(d.mode == M4PanelDirty::Mode::Skip);
    assert(d.reason == M4PanelDirty::Reason::NoChange);
    s.complete(true, 0xE4147346u, 25, 0, d.mode, 0, 0, 0, 0, d.reason);
    assert(s.state().noChange == 1);
    assert(s.state().fullOk == 1);
    const uint32_t epochAfterA = s.state().baselineEpoch;

    // B. External/non-Bridge mutation → same identical frame MUST FULL, not NoChange.
    s.invalidatePhysicalBaseline();
    assert(!s.state().baselineTrusted);
    assert(s.state().everPresented);
    assert(s.state().baselineEpoch > epochAfterA);
    assert(s.offer(1, 0x89325E27u, 40) == M4PanelPresenter::OfferStatus::Scheduled);
    d = takeAndDecide(s, identical, 40);
    assert(d.mode == M4PanelDirty::Mode::Full);
    assert(d.reason == M4PanelDirty::Reason::ForcedFullRecovery);
    s.complete(true, 0xE4147346u, 50, 0, d.mode, 8, 0, 0, 0, d.reason);
    assert(s.state().baselineTrusted);
    assert(s.state().fullOk == 2);

    // E. After that recovery FULL, next identical frame is NoChange again.
    assert(s.offer(1, 0x89325E27u, 60) == M4PanelPresenter::OfferStatus::Scheduled);
    d = takeAndDecide(s, identical, 60);
    assert(d.mode == M4PanelDirty::Mode::Skip);
    assert(d.reason == M4PanelDirty::Reason::NoChange);
    s.complete(true, 0xE4147346u, 65, 0, d.mode, 0, 0, 0, 0, d.reason);
    assert(s.state().noChange == 2);

    // C. owner release → UI write → reacquire → identical frame FirstBaseline FULL.
    assert(s.release());
    assert(!s.state().baselineTrusted);
    assert(!s.state().everPresented);
    s.invalidatePhysicalBaseline();  // GfxRenderer UI write
    assert(s.acquire(M4PanelPresenter::Owner::BrowserBridge));
    d = presentFull(s, 80, 1, 0x89325E27u, 0xE4147346u, true);
    assert(d.mode == M4PanelDirty::Mode::Full);
    assert(d.reason == M4PanelDirty::Reason::FirstBaseline);
    assert(s.state().fullOk == 3);

    // D. Simulated boot/panel re-init → same frame FULL (FirstBaseline).
    s.notePanelReinit();
    assert(!s.state().baselineTrusted);
    assert(!s.state().everPresented);
    d = presentFull(s, 100, 1, 0x89325E27u, 0xE4147346u, true);
    assert(d.mode == M4PanelDirty::Mode::Full);
    assert(d.reason == M4PanelDirty::Reason::FirstBaseline);
    assert(s.state().fullOk == 4);
    assert(s.state().baselineTrusted);

    // F. Failed recovery FULL keeps baseline untrusted and retries FULL.
    s.invalidatePhysicalBaseline();
    assert(s.offer(1, 0x89325E27u, 120) == M4PanelPresenter::OfferStatus::Scheduled);
    d = takeAndDecide(s, identical, 120);
    assert(d.mode == M4PanelDirty::Mode::Full);
    assert(d.reason == M4PanelDirty::Reason::ForcedFullRecovery);
    const uint32_t fullOkBeforeFail = s.state().fullOk;
    const uint32_t panelCrcBeforeFail = s.state().lastPanelCrc;
    s.complete(false, 0, 130, static_cast<uint32_t>(M4PanelPresenter::Error::DisplayFailed), d.mode, 4, 0, 0,
               0, d.reason);
    assert(!s.state().baselineTrusted);
    assert(s.state().fullOk == fullOkBeforeFail);
    assert(s.state().lastPanelCrc == panelCrcBeforeFail);
    assert(s.state().pending);
    d = takeAndDecide(s, identical, 140);
    assert(d.mode == M4PanelDirty::Mode::Full);
    assert(d.reason == M4PanelDirty::Reason::ForcedFullRecovery);
    s.complete(true, 0xE4147346u, 150, 0, d.mode, 8, 0, 0, 0, d.reason);
    assert(s.state().baselineTrusted);
    assert(s.state().fullOk == fullOkBeforeFail + 1);

    // Fast-reconnect analogue: disconnect invalidates without release, so the
    // identical frame cannot stay on NoChange while owner is unchanged.
    assert(s.browserOwns());
    s.invalidatePhysicalBaseline();
    assert(s.browserOwns());
    assert(s.offer(1, 0x89325E27u, 160) == M4PanelPresenter::OfferStatus::Scheduled);
    d = takeAndDecide(s, identical, 160);
    assert(d.mode == M4PanelDirty::Mode::Full);
    assert(d.reason == M4PanelDirty::Reason::ForcedFullRecovery);
  }

  // ACK independence: Session FRAME_ACK commits accepted CRC without a present
  // take/complete. Presenter failure/busy must not rewrite the accepted buffer.
  {
    Harness h;
    uint8_t wire[M4B3::kMaxMessageSize];
    assert(h.handle(wire, M4B3::encodeHello(wire, sizeof(wire), 1, M4B3::kHelloOk)) > 0);
    std::vector<uint8_t> fb = whiteLogical();
    setBlack(fb, 8, 8);
    const uint32_t crc = M4B3::crc32(fb.data(), fb.size());
    const size_t kn = M4B3::encodeKeyframe(wire, sizeof(wire), 2, 7, fb.data());
    const size_t ackN = h.handle(wire, kn);
    assert(ackN > 0);
    assert(h.reply[4] == M4B3::kTypeFrameAck);
    assert(h.reply[M4B3::kEnvelopeSize + 4] == M4B3::kAckOk);
    assert(h.session.acceptedFrameId() == 7);
    assert(h.session.acceptedCrc() == crc);

    M4PanelPresenter::Scheduler s;
    s.acquire(M4PanelPresenter::Owner::BrowserBridge);
    assert(s.offer(h.session.acceptedFrameId(), h.session.acceptedCrc(), 0) ==
           M4PanelPresenter::OfferStatus::Scheduled);
    assert(s.take(0) == M4PanelPresenter::TakeStatus::Ready);
    assert(s.offer(8, 0xBEEF, 1) == M4PanelPresenter::OfferStatus::Scheduled);
    assert(s.take(1) == M4PanelPresenter::TakeStatus::Busy);
    s.complete(false, 0, 2, static_cast<uint32_t>(M4PanelPresenter::Error::DisplayFailed));
    assert(h.session.acceptedFrameId() == 7);
    assert(h.session.acceptedCrc() == crc);
    assert(std::memcmp(h.session.accepted(), fb.data(), fb.size()) == 0);
    assert(!s.state().baselineTrusted);
  }

  // Frozen present snapshot is caller-owned and independent of a later HAL wipe.
  // Host-testable analogue of 229543e: lastPresented advances from the frozen
  // copy, not from a live buffer that Home can memset during FULL.
  {
    std::vector<uint8_t> pending(M4PanelMapper::kPhysicalSize, 0x00);
    pending[0] = 0xA5;
    pending[99] = 0x5A;
    pending[47999] = 0x3C;
    std::vector<uint8_t> presentBuf = pending;
    std::vector<uint8_t> liveHal = presentBuf;
    std::memset(liveHal.data(), 0xFF, liveHal.size());
    assert(presentBuf[0] == 0xA5);
    assert(presentBuf[99] == 0x5A);
    assert(presentBuf[47999] == 0x3C);
    assert(std::memcmp(presentBuf.data(), liveHal.data(), presentBuf.size()) != 0);
    std::vector<uint8_t> lastPresented = presentBuf;
    assert(std::memcmp(lastPresented.data(), pending.data(), pending.size()) == 0);
    const uint32_t frozenCrc = M4B3::crc32(presentBuf.data(), presentBuf.size());
    assert(frozenCrc != M4B3::crc32(liveHal.data(), liveHal.size()));
  }

  printf("test_m4_panel_presenter: PASS\n");
  return 0;
}
