// CffReader — streamed Compact Font Format 1 parser for OpenType/CFF faces.
//
// Pure C++ with no Arduino dependency. CFF INDEXes and CharStrings stay in the
// seekable font stream; only the active DICT/CharString is read while executing.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "TtfReader.h"

namespace ttf {

class CffFont {
 public:
  struct Slice {
    uint32_t off = 0;  // absolute file offset
    uint32_t len = 0;
    bool valid() const { return len != 0; }
  };

  CffFont() = default;
  CffFont(const CffFont&) = delete;
  CffFont& operator=(const CffFont&) = delete;

  // Parse an OpenType/SFNT face containing a CFF (not CFF2) table.
  // `faceOffset` permits TTC/OTC callers to select an already-resolved face.
  bool init(TtfStream& stream, uint32_t faceOffset = 0);

  bool ready() const { return ready_; }
  const char* lastError() const { return lastError_; }
  uint16_t glyphCount() const { return glyphCount_; }
  Slice cffTable() const { return cff_; }
  Slice charStringsIndex() const { return charStrings_; }
  Slice globalSubrsIndex() const { return globalSubrs_; }
  Slice localSubrsIndex() const { return localSubrs_; }
  Slice privateDict() const { return privateDict_; }

  // Return an INDEX object by zero-based item number without loading the whole
  // INDEX into RAM. `index` must be a validated INDEX slice from this object.
  bool indexObject(Slice index, uint16_t item, Slice& object) const;

  // Execute a CFF1 Type 2 CharString and return flattened outline contours in
  // font units. Cubic Beziers are adaptively subdivided into on-curve points so
  // the existing renderer can reuse the same contour/raster path as TrueType.
  bool collectGlyph(uint16_t gid, std::vector<Contour>& out) const;

 private:
  struct IndexInfo {
    Slice whole;
    uint16_t count = 0;
    uint8_t offSize = 0;
    uint32_t offsetsOff = 0;
    uint32_t dataOff = 0;
  };

  bool readAt(uint32_t off, void* dst, uint32_t n) const;
  bool parseIndex(uint32_t relOff, IndexInfo& out, uint32_t* nextRel = nullptr) const;
  bool indexObject(const IndexInfo& index, uint16_t item, Slice& object) const;
  bool parseTopDict(Slice dict);
  bool parsePrivateDict();
  bool readOffset(uint32_t absOff, uint8_t offSize, uint32_t& value) const;
  bool executeType2(Slice code, std::vector<Contour>& out, int depth,
                    float& x, float& y, uint32_t& stemCount) const;

  TtfStream* stream_ = nullptr;
  uint32_t fileSize_ = 0;
  bool ready_ = false;
  mutable const char* lastError_ = "not initialized";

  Slice cff_;
  IndexInfo charStringsInfo_;
  IndexInfo globalSubrsInfo_;
  IndexInfo localSubrsInfo_;
  Slice charStrings_;
  Slice globalSubrs_;
  Slice localSubrs_;
  Slice privateDict_;
  uint16_t glyphCount_ = 0;
};

}  // namespace ttf
