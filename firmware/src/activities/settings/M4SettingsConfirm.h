#pragma once

#include <cstdint>

// Power is never a danger primary. Explicit accept = footer primary or on-page verb row.

enum class M4ConfirmButton : uint8_t { None, Back, Confirm, Power, Other };

inline bool m4SettingsPowerIsPrimary(M4ConfirmButton) {
  return false;
}

inline bool m4SettingsDangerAccepts(M4ConfirmButton b, bool footerPrimaryOrRow) {
  if (b == M4ConfirmButton::Power || b == M4ConfirmButton::Back || b == M4ConfirmButton::None) {
    return false;
  }
  if (!footerPrimaryOrRow) return false;
  return b == M4ConfirmButton::Confirm || b == M4ConfirmButton::Other;
}
