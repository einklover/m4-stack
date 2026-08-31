#pragma once

#include <cstddef>
#include <cstdint>

#include "ui/scene/UiSceneTypes.h"

namespace UiScene {

// Immutable 1-bit asset — MSB first, 1=black ink, 0=transparent/no-op.
// Pixels lifetime = snapshot publication epoch or PROGMEM package lifetime.
// No allocation, no file handle, no SDK object.
struct UiSceneAsset {
  const uint8_t* pixels = nullptr;
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t stride = 0; // bytes per row = (width+7)/8
  bool progmem = false;

  bool valid() const { return pixels != nullptr && width > 0 && height > 0 && stride > 0; }

  inline uint8_t readByte(size_t offset) const {
    return progmem ? pgm_read_byte(pixels + offset) : pixels[offset];
  }

  // Black-ink test: true if pixel at (x,y) is black (1), false if transparent.
  inline bool isBlack(uint16_t x, uint16_t y) const {
    if (!valid() || x >= width || y >= height) return false;
    const size_t byteOff = static_cast<size_t>(y) * stride + (x >> 3);
    const uint8_t b = readByte(byteOff);
    const uint8_t mask = static_cast<uint8_t>(1u << (7 - (x & 7u)));
    return (b & mask) != 0;
  }
};

static constexpr uint8_t kInvalidAssetItemIndex = 0xFF;

struct AssetKey {
  BindingId binding = kInvalidBindingId;
  BindingId sourceBinding = kInvalidBindingId;
  uint8_t itemIndex = kInvalidAssetItemIndex;

  bool operator==(const AssetKey& other) const {
    return binding == other.binding && sourceBinding == other.sourceBinding &&
           itemIndex == other.itemIndex;
  }
};

// Fixed-capacity immutable asset store — backend populates, renderer consumes.
// No std::vector, no heap, no SD handle.
struct UiSceneAssets {
  static constexpr size_t kMaxAssets = 8;
  UiSceneAsset assets[kMaxAssets] = {};
  AssetKey keys[kMaxAssets] = {};
  uint8_t count = 0;

  bool add(const UiSceneAsset& a) {
    if (count >= kMaxAssets || !a.valid()) return false;
    assets[count++] = a;
    return true;
  }

  bool add(const AssetKey& key, const UiSceneAsset& asset) {
    if (count >= kMaxAssets || !asset.valid()) return false;
    keys[count] = key;
    assets[count++] = asset;
    return true;
  }

  const UiSceneAsset* get(uint8_t index) const {
    return index < count ? &assets[index] : nullptr;
  }

  const UiSceneAsset* get(const AssetKey& key) const {
    for (uint8_t i = 0; i < count; ++i) {
      if (keys[i] == key) return &assets[i];
    }
    return nullptr;
  }

  void clear() { count = 0; }

};

} // namespace UiScene
