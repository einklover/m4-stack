// Platform-neutral page-display contract.
//
// This is intentionally smaller than an SSD1677 driver. The shared reader
// logic only needs to know whether the display is busy, associate provenance
// with a rendered frame, and submit that frame asynchronously. Deterministic
// simulation, host/native rendering, QEMU and real hardware can each provide
// a backend without leaking their concrete driver types into ReaderModel.
#pragma once

#include <cstdint>
#include <functional>

namespace m4platform {

enum class RefreshMode {
  FAST_REFRESH,
  HALF_REFRESH,
  FULL_REFRESH,
  UI_FAST_REFRESH,
};

struct FrameTag {
  uint32_t generation = 0;
  int chapter = 0;
  int page = -1;
  uint64_t frameHash = 0;
};

class DisplayPort {
public:
  virtual ~DisplayPort() = default;

  virtual bool busy() const = 0;

  // Called when the renderer has completed a frame. `tag` is provenance: it
  // identifies the exact logical page represented by the frame.
  virtual void render(const FrameTag& tag) = 0;

  // Submit the most recently rendered frame. Returns false if the backend
  // cannot accept it (for example because an EPD refresh is already in flight).
  virtual bool submit(RefreshMode mode,
                      std::function<void()> onCommitted = nullptr) = 0;
};

}  // namespace m4platform
