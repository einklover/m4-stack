#pragma once

class HardwareSerial {
 public:
  template <typename... Args>
  void printf(const char*, Args...) {}
};

inline HardwareSerial Serial;
inline unsigned long millis() { return 0; }
