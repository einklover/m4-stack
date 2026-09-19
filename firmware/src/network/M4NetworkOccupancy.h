#pragma once

#include <cstdint>

// NeedNetwork / ReleaseNetwork lease. Radio off is legal only when no holders remain.
// Multiple owners may share. Force-deinit while another owner holds is denied.

enum class M4NetworkOwner : uint8_t { None = 0, WifiSettings, Transfer, Ntp, Cloud, Ota, Other };

enum class M4NetworkAcquireResult : uint8_t { Ok, DeniedHeldByOther };

namespace m4_network_occupancy_detail {

inline uint8_t& holders() {
  static uint8_t bits = 0;
  return bits;
}

inline uint8_t mask(M4NetworkOwner owner) {
  const uint8_t v = static_cast<uint8_t>(owner);
  if (v == 0 || v > 7) return 0;
  return static_cast<uint8_t>(1u << v);
}

}  // namespace m4_network_occupancy_detail

inline void m4NetworkOccupancyResetForTest() {
  m4_network_occupancy_detail::holders() = 0;
}

inline bool m4NetworkHeldBy(M4NetworkOwner owner) {
  return (m4_network_occupancy_detail::holders() & m4_network_occupancy_detail::mask(owner)) != 0;
}

inline bool m4NetworkRadioMayOff() {
  return m4_network_occupancy_detail::holders() == 0;
}

inline M4NetworkAcquireResult m4NeedNetwork(M4NetworkOwner owner) {
  const uint8_t b = m4_network_occupancy_detail::mask(owner);
  if (b == 0) return M4NetworkAcquireResult::Ok;
  m4_network_occupancy_detail::holders() = static_cast<uint8_t>(m4_network_occupancy_detail::holders() | b);
  return M4NetworkAcquireResult::Ok;
}

inline void m4ReleaseNetwork(M4NetworkOwner owner) {
  const uint8_t b = m4_network_occupancy_detail::mask(owner);
  m4_network_occupancy_detail::holders() = static_cast<uint8_t>(m4_network_occupancy_detail::holders() & static_cast<uint8_t>(~b));
}

inline M4NetworkAcquireResult m4NetworkTryDeinit(M4NetworkOwner /*requester*/) {
  if (m4_network_occupancy_detail::holders() != 0) return M4NetworkAcquireResult::DeniedHeldByOther;
  return M4NetworkAcquireResult::Ok;
}
