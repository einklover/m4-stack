#pragma once

struct M4PsramTestSerial {
  template <typename... Args>
  void printf(const char*, Args...) {}
};

inline M4PsramTestSerial Serial;
