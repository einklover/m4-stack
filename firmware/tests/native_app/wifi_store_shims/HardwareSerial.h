#pragma once

#include <cstdint>

inline uint32_t millis() { return 0; }

struct TestSerial {
  template <typename... Args>
  void printf(const char*, Args...) {}
};

inline TestSerial Serial;
