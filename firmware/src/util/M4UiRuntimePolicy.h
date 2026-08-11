#pragma once

#include <atomic>

// Process-wide UI runtime policy for the currently active Activity.
//
// Murphy M4 renders one foreground Activity at a time. Runtime TTF typography
// itself is now quantized to shared fixed raster faces (18/22/26px), so Native
// plugin pages deliberately use 100% text scale and reuse the exact same system
// chrome fonts. The percentage remains for legacy epdfont/package compatibility,
// but built-in Native providers no longer depend on arbitrary bitmap scaling.
namespace M4UiRuntimePolicy {

inline constexpr int kNativePluginTextScalePercent = 100;
inline std::atomic<int> gTextScalePercent{100};

inline int clampTextScalePercent(int percent) {
  if (percent < 60) return 60;
  if (percent > 125) return 125;
  return percent;
}

inline void setTextScalePercent(int percent) {
  gTextScalePercent.store(clampTextScalePercent(percent), std::memory_order_release);
}

inline int textScalePercent() {
  return gTextScalePercent.load(std::memory_order_acquire);
}

inline float textScale() {
  return static_cast<float>(textScalePercent()) / 100.0f;
}

}  // namespace M4UiRuntimePolicy
