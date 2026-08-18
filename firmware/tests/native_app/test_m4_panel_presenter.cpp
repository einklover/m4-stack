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
    assert(h.session.acceptedCrc() == crc);
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

  printf("test_m4_panel_presenter: PASS\n");
  return 0;
}
