#pragma once

#include <cstdint>

// Phase 1 (INV-S1): dependency-free consume-once suppression truth.
// Physical-only state that survives beginFrame() untouched; synthetic edges
// neither set, clear, nor observe suppression. Consumed by both production
// (MappedInputManager physical paths + main.cpp frame-top check) and the
// Task 9 host behavioral test with bare g++.
struct M4SuppressState {
  uint16_t mask = 0;
  uint16_t firedLatch = 0;
};

// 11 enumerators fit uint16_t, so indices 0-10 are usable.
[[nodiscard]] constexpr bool m4SuppressButtonIndex(uint8_t buttonIndex) { return buttonIndex < 11; }

constexpr void m4SuppressNextRelease(M4SuppressState& state, uint8_t buttonIndex) {
  if (!m4SuppressButtonIndex(buttonIndex)) return;
  state.mask |= static_cast<uint16_t>(static_cast<uint16_t>(1u) << buttonIndex);
}

// Frame-top fold over already-resolved physical release edges of this frame.
constexpr bool m4ConsumeSuppressedRelease(M4SuppressState& state, uint16_t physicalReleasedMask) {
  const uint16_t hit = static_cast<uint16_t>(state.mask & physicalReleasedMask);
  if (hit == 0) return false;
  state.mask = static_cast<uint16_t>(state.mask & static_cast<uint16_t>(~hit));
  return true;
}

// One-shot threshold event while held (replaces the ad-hoc already-fired
// latch; sets latch + suppression once when isPressed && heldMs >=
// thresholdMs, clears when released).
constexpr bool m4LongPressFired(M4SuppressState& state, uint8_t buttonIndex, bool isPressed,
                                unsigned long heldMs, unsigned long thresholdMs) {
  if (!m4SuppressButtonIndex(buttonIndex)) return false;
  const uint16_t bit = static_cast<uint16_t>(static_cast<uint16_t>(1u) << buttonIndex);
  if (!isPressed) {
    state.firedLatch = static_cast<uint16_t>(state.firedLatch & static_cast<uint16_t>(~bit));
    return false;
  }
  if (heldMs < thresholdMs) return false;
  if ((state.firedLatch & bit) != 0) return false;
  state.firedLatch |= bit;
  state.mask |= bit;
  return true;
}
