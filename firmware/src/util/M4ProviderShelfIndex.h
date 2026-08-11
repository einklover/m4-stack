#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace M4ProviderShelfIndex {

constexpr size_t kStride = 16;

struct Builder {
  size_t rows = 0;
  uint32_t offset = 0;
  bool any = false;
  uint8_t last = '\n';
  std::vector<uint32_t> anchors{0};

  void feed(const uint8_t* data, size_t len) {
    if (!data || len == 0) return;
    for (size_t i = 0; i < len; ++i) {
      any = true;
      last = data[i];
      ++offset;
      if (data[i] == '\n') {
        ++rows;
        if (rows % kStride == 0) anchors.push_back(offset);
      }
    }
  }

  size_t finish() const { return rows + ((any && last != '\n') ? 1u : 0u); }
};

inline size_t anchorSlot(size_t row) { return row / kStride; }
inline size_t anchorRow(size_t row) { return anchorSlot(row) * kStride; }

}  // namespace M4ProviderShelfIndex
