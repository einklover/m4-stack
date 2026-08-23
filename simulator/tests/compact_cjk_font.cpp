#include "fontdata/m4_compact_cjk_16.h"

#include <cassert>
#include <cstdint>
#include <iostream>

namespace {

const EpdGlyph* findGlyph(uint32_t cp) {
  int left = 0;
  int right = static_cast<int>(sizeof(m4_compact_cjk_16Intervals) /
                               sizeof(m4_compact_cjk_16Intervals[0])) - 1;
  while (left <= right) {
    const int mid = left + (right - left) / 2;
    const EpdUnicodeInterval& interval = m4_compact_cjk_16Intervals[mid];
    if (cp < interval.first) {
      right = mid - 1;
    } else if (cp > interval.last) {
      left = mid + 1;
    } else {
      return &m4_compact_cjk_16Glyphs[interval.offset + cp - interval.first];
    }
  }
  return nullptr;
}

}  // namespace

int main() {
  static constexpr uint32_t kCommon[] = {
      0x4E2D, 0x56FD, 0x7684, 0x4F60, 0x6211, 0x8BFB, 0x4E66,
      0x7AE0, 0x8282, 0x7F51, 0x7EDC, 0x8BBE, 0x7F6E, 0x53E3,
      'A', '?', 0x3001, 0x3002, 0xFF01, 0xFF1F,
  };
  for (uint32_t cp : kCommon) {
    const EpdGlyph* glyph = findGlyph(cp);
    assert(glyph != nullptr);
    assert(glyph->advanceX > 0);
    assert(glyph->dataLength > 0);
    assert(glyph->dataOffset + glyph->dataLength <= sizeof(m4_compact_cjk_16Bitmaps));
  }

  // The compact face intentionally has finite coverage. The existing loader
  // promotes SD EPDF/runtime sfnt faces for missing reader glyphs; this check
  // ensures the compact face does not silently turn a missing codepoint into a
  // bogus bitmap before that external fallback can run.
  assert(findGlyph(0x9FFF) == nullptr);
  assert(findGlyph('?') != nullptr);

  const size_t staticBytes = sizeof(m4_compact_cjk_16Bitmaps) +
                             sizeof(m4_compact_cjk_16Glyphs) +
                             sizeof(m4_compact_cjk_16Intervals) +
                             sizeof(m4_compact_cjk_16);
  assert(m4_compact_cjk_16.is2Bit);
  assert(staticBytes >= 220u * 1024u);
  assert(staticBytes <= 270u * 1024u);
  std::cout << "compact CJK coverage/fallback/budget: PASS (" << staticBytes << " bytes)\n";
  return 0;
}
