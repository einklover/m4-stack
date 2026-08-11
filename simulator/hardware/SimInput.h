#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>

namespace m4sim {

enum class MurphyButton : uint8_t { Up = 0, Down = 1, Power = 2 };

struct TouchSample {
  uint16_t x = 0;
  uint16_t y = 0;
  uint32_t atMs = 0;
};

class SimInput {
 public:
  explicit SimInput(uint32_t debounceMs = 30, uint16_t tapSlopPx = 24,
                    uint32_t swipeMaxMs = 500, uint16_t swipeMinPx = 48)
      : debounceMs_(debounceMs), tapSlopPx_(tapSlopPx),
        swipeMaxMs_(swipeMaxMs), swipeMinPx_(swipeMinPx) {}

  void sampleButtons(uint32_t nowMs, bool upPinHigh, bool downPinHigh, bool powerPinHigh) {
    const uint8_t raw = (!upPinHigh ? kUp : 0) |
                        (!downPinHigh ? kDown : 0) |
                        (!powerPinHigh ? kPower : 0);
    if (raw != sampled_) {
      sampled_ = raw;
      sampledSince_ = nowMs;
    }
    pressedEdges_ = 0;
    releasedEdges_ = 0;
    if (nowMs - sampledSince_ > debounceMs_ && sampled_ != committed_) {
      pressedEdges_ = sampled_ & ~committed_;
      releasedEdges_ = committed_ & ~sampled_;
      committed_ = sampled_;
    }
  }

  bool pressed(MurphyButton b) const { return committed_ & bit(b); }
  bool wasPressed(MurphyButton b) const { return pressedEdges_ & bit(b); }
  bool wasReleased(MurphyButton b) const { return releasedEdges_ & bit(b); }

  void touchDown(TouchSample p) {
    touchActive_ = true;
    touchDown_ = p;
    touchLatest_ = p;
    movedBeyondTapSlop_ = false;
    releasedTap_.reset();
    releasedSwipe_.reset();
  }

  void touchMove(TouchSample p) {
    if (!touchActive_) return;
    touchLatest_ = p;
    const int dx = static_cast<int>(p.x) - static_cast<int>(touchDown_.x);
    const int dy = static_cast<int>(p.y) - static_cast<int>(touchDown_.y);
    if (absInt(dx) > tapSlopPx_ || absInt(dy) > tapSlopPx_) movedBeyondTapSlop_ = true;
  }

  void touchUp(TouchSample p) {
    if (!touchActive_) return;
    touchMove(p);
    touchActive_ = false;
    const uint32_t held = p.atMs - touchDown_.atMs;
    if (!movedBeyondTapSlop_) releasedTap_ = normalize(touchDown_);
    const int dx = static_cast<int>(p.x) - static_cast<int>(touchDown_.x);
    const int dy = static_cast<int>(p.y) - static_cast<int>(touchDown_.y);
    if (held <= swipeMaxMs_ &&
        (absInt(dx) >= swipeMinPx_ || absInt(dy) >= swipeMinPx_)) {
      releasedSwipe_ = Swipe{normalize(touchDown_), normalize(p), held};
    }
  }

  struct Point { float x = 0; float y = 0; };
  struct Swipe { Point start; Point end; uint32_t heldMs = 0; };

  std::optional<Point> consumeTap() {
    auto out = releasedTap_;
    releasedTap_.reset();
    return out;
  }
  std::optional<Swipe> consumeSwipe() {
    auto out = releasedSwipe_;
    releasedSwipe_.reset();
    return out;
  }

  // Exact production M4 FT6x36 transform from InputManager::pollFt6x36:
  // silicon portrait rawX=0..479/rawY=0..799, BoardConfig swapXY=true,
  // flipY=true, output panel frame 800x480. Invalid silicon coordinates are
  // rejected rather than clamped so bus/frame corruption cannot become a tap.
  static std::optional<TouchSample> ft6336RawToPanel(uint16_t rawX, uint16_t rawY,
                                                     uint32_t atMs) {
    if (rawX >= 480 || rawY >= 800) return std::nullopt;
    const uint16_t sx = rawY;            // swapXY
    const uint16_t sy = rawX;
    const uint16_t panelX = sx;          // 0..799
    const uint16_t panelY = 479u - sy;   // flipY
    return TouchSample{panelX, panelY, atMs};
  }

  static Point normalize(const TouchSample& p) {
    Point out;
    out.x = std::clamp(static_cast<float>(p.x) / 799.0f, 0.0f, 1.0f);
    out.y = std::clamp(static_cast<float>(p.y) / 479.0f, 0.0f, 1.0f);
    return out;
  }

 private:
  static constexpr uint8_t kUp = 1u << 0;
  static constexpr uint8_t kDown = 1u << 1;
  static constexpr uint8_t kPower = 1u << 2;
  static uint8_t bit(MurphyButton b) { return 1u << static_cast<uint8_t>(b); }
  static int absInt(int v) { return v < 0 ? -v : v; }

  uint32_t debounceMs_ = 30;
  uint16_t tapSlopPx_ = 24;
  uint32_t swipeMaxMs_ = 500;
  uint16_t swipeMinPx_ = 48;
  uint8_t sampled_ = 0;
  uint8_t committed_ = 0;
  uint8_t pressedEdges_ = 0;
  uint8_t releasedEdges_ = 0;
  uint32_t sampledSince_ = 0;
  bool touchActive_ = false;
  bool movedBeyondTapSlop_ = false;
  TouchSample touchDown_{};
  TouchSample touchLatest_{};
  std::optional<Point> releasedTap_;
  std::optional<Swipe> releasedSwipe_;
};

}  // namespace m4sim
