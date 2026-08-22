#pragma once

#include "apps/M4xJsonStream.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>

namespace M4NativeProviderText {

// Legado and WeRead both return chapter bodies with lightweight XHTML. Keep
// the cache readable on the native reader without buffering the whole body.
class XhtmlStripSink final : public M4xJsonStream::Sink {
 public:
  explicit XhtmlStripSink(M4xJsonStream::Sink& out) : out_(out) {}

  bool write(const uint8_t* data, size_t len) override {
    if (!data && len != 0) return false;
    for (size_t i = 0; i < len; ++i) {
      const uint8_t b = data[i];
      if (inTag_) {
        if (b == '>') {
          std::string low = tag_;
          std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
          });
          if (low.find("br") == 0 || low.find("/p") == 0 ||
              low.find("/div") == 0 || low.find("/li") == 0) {
            if (!emit('\n')) return false;
          }
          inTag_ = false;
          tag_.clear();
        } else if (b < 0x80 && tag_.size() < 32) {
          tag_.push_back(static_cast<char>(b));
        }
        continue;
      }

      if (inEntity_) {
        if (b == ';') {
          if (entity_ == "nbsp" || entity_ == "#160") {
            if (!emit(' ')) return false;
          } else if (entity_ == "amp") {
            if (!emit('&')) return false;
          } else if (entity_ == "lt") {
            if (!emit('<')) return false;
          } else if (entity_ == "gt") {
            if (!emit('>')) return false;
          } else {
            if (!emit(' ')) return false;
          }
          inEntity_ = false;
          entity_.clear();
          continue;
        }
        if (b < 0x80 && entity_.size() < 14) {
          entity_.push_back(static_cast<char>(b));
          continue;
        }
        if (!emit('&') ||
            !out_.write(reinterpret_cast<const uint8_t*>(entity_.data()), entity_.size())) {
          return false;
        }
        inEntity_ = false;
        entity_.clear();
      }

      if (b == '<') {
        inTag_ = true;
        tag_.clear();
      } else if (b == '&') {
        inEntity_ = true;
        entity_.clear();
      } else if (b != '\r') {
        if (!out_.write(&b, 1)) return false;
      }
    }
    return true;
  }

 private:
  bool emit(char c) {
    const uint8_t b = static_cast<uint8_t>(c);
    return out_.write(&b, 1);
  }

  M4xJsonStream::Sink& out_;
  bool inTag_ = false;
  bool inEntity_ = false;
  std::string tag_;
  std::string entity_;
};

}  // namespace M4NativeProviderText
