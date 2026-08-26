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
  READER_CLEANUP_REFRESH,
};

enum class RefreshContext {
  UI_CONTEXT,
  READER_BODY_CONTEXT,
};

// Legacy FULL/HALF names remain source-compatible with older simulator
// callers, but they are never a physical waveform. Only the explicit reader
// cleanup can request the one-inversion mode.
constexpr RefreshMode normalizeRefreshMode(RefreshMode mode,
                                            RefreshContext context = RefreshContext::UI_CONTEXT) {
  return mode == RefreshMode::READER_CLEANUP_REFRESH &&
                 context == RefreshContext::READER_BODY_CONTEXT
             ? RefreshMode::READER_CLEANUP_REFRESH
             : RefreshMode::FAST_REFRESH;
}

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
                      std::function<void()> onCommitted = nullptr,
                      RefreshContext context = RefreshContext::UI_CONTEXT) = 0;
};

}  // namespace m4platform
