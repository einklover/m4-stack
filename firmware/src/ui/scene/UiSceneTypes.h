#pragma once

#include <cstddef>
#include <cstdint>

#ifdef __AVR__
#include <avr/pgmspace.h>
#else
#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef pgm_read_byte
#define pgm_read_byte(addr) (*(const uint8_t*)(addr))
#endif
#endif

namespace UiScene {

enum class DataState : uint8_t { Loading, Ready, Stale, Empty, Error };

static constexpr uint16_t kMaxSceneNodes = 128;
static constexpr uint8_t kMaxBindings = 64;
static constexpr uint8_t kMaxActions = 32;
static constexpr uint8_t kMaxRepeatItems = 8;

using BindingId = uint8_t;
using ActionId = uint8_t;
static constexpr BindingId kInvalidBindingId = 0xFF;
static constexpr ActionId kInvalidActionId = 0xFF;

struct Rect {
  int16_t x = 0;
  int16_t y = 0;
  uint16_t width = 0;
  uint16_t height = 0;

  constexpr bool contains(int16_t px, int16_t py) const {
    return px >= x && py >= y &&
           static_cast<int32_t>(px) < static_cast<int32_t>(x) + width &&
           static_cast<int32_t>(py) < static_cast<int32_t>(y) + height;
  }
};

enum class TextStorage : uint8_t { Ram, Progmem };

struct TextView {
  const char* data = nullptr;
  uint16_t size = 0;
  TextStorage storage = TextStorage::Ram;

  static constexpr TextView fromRam(const char* data, uint16_t size) {
    return {data, size, TextStorage::Ram};
  }
  static constexpr TextView fromProgmem(const char* data, uint16_t size) {
    return {data, size, TextStorage::Progmem};
  }
  uint8_t readByte(uint16_t index) const {
    if (!data || index >= size) return 0;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(data + index);
    return storage == TextStorage::Progmem ? pgm_read_byte(p) : *p;
  }
};

struct ItemContext {
  uint8_t index = 0;
  uint8_t count = 0;
  const BindingId* bindings = nullptr;
  uint8_t bindingCount = 0;
};

struct NumericBinding {
  BindingId id = kInvalidBindingId;
  int32_t value = 0;
};

struct ActionTarget {
  ActionId id = kInvalidActionId;
  Rect bounds{};
  BindingId argument = kInvalidBindingId;
};

} // namespace UiScene
