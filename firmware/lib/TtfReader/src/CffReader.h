// CffReader — streamed Compact Font Format 1 parser for OpenType/CFF faces.
//
// This layer intentionally stops at validated CFF metadata/INDEX discovery.
// Type 2 CharString execution and rasterization are built on top of it so the
// container/parser can be fuzzed and host-tested independently.
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
  Slice privateDict() const { return privateDict_; }

  // Return an INDEX object by zero-based item number without loading the whole
  // INDEX into RAM. `index` must be a validated INDEX slice from this object.
  bool indexObject(Slice index, uint16_t item, Slice& object) const;

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
  bool readOffset(uint32_t absOff, uint8_t offSize, uint32_t& value) const;

  TtfStream* stream_ = nullptr;
  uint32_t fileSize_ = 0;
  bool ready_ = false;
  const char* lastError_ = "not initialized";

  Slice cff_;
  IndexInfo charStringsInfo_;
  IndexInfo globalSubrsInfo_;
  Slice charStrings_;
  Slice globalSubrs_;
  Slice privateDict_;
  uint16_t glyphCount_ = 0;
};

}  // namespace ttf
