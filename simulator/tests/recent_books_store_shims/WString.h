#pragma once

#include <string>

class String {
 public:
  String() = default;
  String(const char* value) : value_(value ? value : "") {}
  String(const std::string& value) : value_(value) {}
  size_t length() const { return value_.size(); }
  void toLowerCase() {}
  bool endsWith(const String& suffix) const {
    return value_.size() >= suffix.value_.size() &&
           value_.compare(value_.size() - suffix.value_.size(), suffix.value_.size(), suffix.value_) == 0;
  }

 private:
  std::string value_;
};
