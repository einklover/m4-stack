#pragma once

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <thread>

class String {
 public:
  String() = default;
  String(const char* value) : value_(value ? value : "") {}
  String(const std::string& value) : value_(value) {}

  String& operator=(const char* value) {
    value_ = value ? value : "";
    return *this;
  }

  const char* c_str() const { return value_.c_str(); }
  size_t length() const { return value_.length(); }

 private:
  std::string value_;
};

class HardwareSerial {
 public:
  template <typename... Args>
  void printf(const char*, Args...) {}
  void print(const char*) {}
  void println(const char*) {}
  void println() {}
};

inline HardwareSerial Serial;
inline void delay(unsigned long ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
