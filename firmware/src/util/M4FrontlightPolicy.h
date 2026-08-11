#pragma once
// Host-testable frontlight clamp/off policy for Murphy M4 dual-channel PWM.
// Electrical pins/PWM are owned by FrontlightManager; this only normalizes values.

#include <cstdint>

namespace M4FrontlightPolicy {

constexpr uint8_t kMaxPercent = 100;
constexpr uint8_t kDefaultBrightness = 20;
constexpr uint8_t kDefaultWarmth = 50;
constexpr uint8_t kEarlyBootBrightness = 20;
constexpr uint8_t kEarlyBootWarmth = 50;

inline uint8_t clampPercent(int v) {
  if (v < 0) return 0;
  if (v > static_cast<int>(kMaxPercent)) return kMaxPercent;
  return static_cast<uint8_t>(v);
}

// brightness=0 is off (both channels driven off by the driver).
inline bool isOff(uint8_t brightness) { return brightness == 0; }

struct Levels {
  uint8_t brightness = kDefaultBrightness;
  uint8_t warmth = kDefaultWarmth;
};

inline Levels normalize(int brightness, int warmth) {
  Levels L;
  L.brightness = clampPercent(brightness);
  L.warmth = clampPercent(warmth);
  return L;
}

inline Levels earlyBootDefaults() {
  return Levels{kEarlyBootBrightness, kEarlyBootWarmth};
}

// Mix model documentation for tests: warmth 0 = full cool, 100 = full warm.
// Driver implements PWM duty split; we only assert bounds here.
inline void coolWarmDuties(uint8_t brightness, uint8_t warmth, uint8_t& coolOut, uint8_t& warmOut) {
  if (brightness == 0) {
    coolOut = 0;
    warmOut = 0;
    return;
  }
  // Linear mix of total brightness across channels (relative duties 0..brightness).
  warmOut = static_cast<uint8_t>((static_cast<unsigned>(brightness) * warmth) / 100u);
  coolOut = static_cast<uint8_t>(brightness - warmOut);
}

}  // namespace M4FrontlightPolicy
