#include "CffReader.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace ttf {
namespace {

uint16_t rd16be(const uint8_t* p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}
uint32_t rd32be(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) |
         static_cast<uint32_t>(p[3]);
}
uint32_t tag(const char s[5]) {
  return (static_cast<uint32_t>(static_cast<uint8_t>(s[0])) << 24) |
         (static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 8) |
         static_cast<uint32_t>(static_cast<uint8_t>(s[3]));
}

bool decodeDictNumber(const uint8_t* data, size_t len, size_t& pos, int32_t& out) {
  if (pos >= len) return false;
  const uint8_t b0 = data[pos++];
  if (b0 >= 32 && b0 <= 246) {
    out = static_cast<int32_t>(b0) - 139;
    return true;
  }
  if (b0 >= 247 && b0 <= 250) {
    if (pos >= len) return false;
    out = (static_cast<int32_t>(b0) - 247) * 256 + data[pos++] + 108;
    return true;
  }
  if (b0 >= 251 && b0 <= 254) {
    if (pos >= len) return false;
    out = -(static_cast<int32_t>(b0) - 251) * 256 - data[pos++] - 108;
    return true;
  }
  if (b0 == 28) {
    if (len - pos < 2) return false;
    out = static_cast<int16_t>(rd16be(data + pos));
    pos += 2;
    return true;
  }
  if (b0 == 29) {
    if (len - pos < 4) return false;
    out = static_cast<int32_t>(rd32be(data + pos));
    pos += 4;
    return true;
  }
  if (b0 == 30) {
    // Real numbers are legal in Top DICT but irrelevant to the offsets needed
    // by the runtime. Consume nibbles through the 0xF terminator.
    while (pos < len) {
      const uint8_t b = data[pos++];
      if ((b >> 4) == 0xF || (b & 0x0F) == 0xF) {
        out = 0;
        return true;
      }
    }
    return false;
  }
  // 255 is reserved in CFF DICT data; 0..27/31 are operators.
  return false;
}

}  // namespace

bool CffFont::readAt(uint32_t off, void* dst, uint32_t n) const {
  if (!stream_ || off > fileSize_ || n > fileSize_ - off) return false;
  if (!stream_->seek(off)) return false;
  return stream_->read(dst, n) == n;
}

bool CffFont::readOffset(uint32_t absOff, uint8_t offSize, uint32_t& value) const {
  value = 0;
  if (offSize < 1 || offSize > 4) return false;
  uint8_t buf[4] = {};
  if (!readAt(absOff, buf, offSize)) return false;
  for (uint8_t i = 0; i < offSize; ++i) value = (value << 8) | buf[i];
  return true;
}

bool CffFont::parseIndex(uint32_t relOff, IndexInfo& out, uint32_t* nextRel) const {
  out = IndexInfo{};
  if (!cff_.valid() || relOff > cff_.len || cff_.len - relOff < 2) return false;
  const uint32_t abs = cff_.off + relOff;
  uint8_t countBuf[2];
  if (!readAt(abs, countBuf, 2)) return false;
  const uint16_t count = rd16be(countBuf);
  out.count = count;
  out.whole.off = abs;
  if (count == 0) {
    out.whole.len = 2;
    if (nextRel) *nextRel = relOff + 2;
    return true;
  }

  uint8_t offSize = 0;
  if (!readAt(abs + 2, &offSize, 1) || offSize < 1 || offSize > 4) return false;
  const uint64_t offsetsBytes = static_cast<uint64_t>(count + 1u) * offSize;
  const uint64_t dataRel64 = static_cast<uint64_t>(relOff) + 3u + offsetsBytes;
  if (dataRel64 > cff_.len) return false;

  uint32_t first = 0, last = 0;
  if (!readOffset(abs + 3, offSize, first) ||
      !readOffset(abs + 3 + static_cast<uint32_t>(count) * offSize, offSize, last)) {
    return false;
  }
  if (first != 1 || last < first) return false;
  const uint64_t endRel64 = dataRel64 + static_cast<uint64_t>(last - 1u);
  if (endRel64 > cff_.len || endRel64 > 0xffffffffu) return false;

  out.offSize = offSize;
  out.offsetsOff = abs + 3;
  out.dataOff = cff_.off + static_cast<uint32_t>(dataRel64);
  out.whole.len = static_cast<uint32_t>(endRel64 - relOff);
  if (nextRel) *nextRel = static_cast<uint32_t>(endRel64);
  return true;
}

bool CffFont::indexObject(const IndexInfo& index, uint16_t item, Slice& object) const {
  object = Slice{};
  if (item >= index.count || index.count == 0 || index.offSize == 0) return false;
  uint32_t a = 0, b = 0;
  const uint32_t entry = index.offsetsOff + static_cast<uint32_t>(item) * index.offSize;
  if (!readOffset(entry, index.offSize, a) || !readOffset(entry + index.offSize, index.offSize, b)) return false;
  if (a == 0 || b < a) return false;
  const uint64_t objOff = static_cast<uint64_t>(index.dataOff) + a - 1u;
  const uint64_t objLen = static_cast<uint64_t>(b) - a;
  const uint64_t indexEnd = static_cast<uint64_t>(index.whole.off) + index.whole.len;
  if (objOff + objLen > indexEnd || objOff > 0xffffffffu || objLen > 0xffffffffu) return false;
  object.off = static_cast<uint32_t>(objOff);
  object.len = static_cast<uint32_t>(objLen);
  return true;
}

bool CffFont::indexObject(Slice index, uint16_t item, Slice& object) const {
  object = Slice{};
  if (!index.valid() || index.off < cff_.off || index.off - cff_.off >= cff_.len) return false;
  IndexInfo parsed;
  if (!parseIndex(index.off - cff_.off, parsed, nullptr)) return false;
  if (parsed.whole.len != index.len) return false;
  return indexObject(parsed, item, object);
}

bool CffFont::parseTopDict(Slice dict) {
  if (!dict.valid() || dict.len > 64u * 1024u) {
    lastError_ = "CFF Top DICT invalid or too large";
    return false;
  }
  std::vector<uint8_t> bytes(dict.len);
  if (!readAt(dict.off, bytes.data(), dict.len)) {
    lastError_ = "failed to read CFF Top DICT";
    return false;
  }

  std::vector<int32_t> operands;
  operands.reserve(8);
  uint32_t charStringsRel = 0;
  bool haveCharStrings = false;
  uint32_t privateSize = 0, privateRel = 0;

  size_t pos = 0;
  while (pos < bytes.size()) {
    const uint8_t b0 = bytes[pos];
    if (b0 >= 28 || b0 >= 32) {
      int32_t value = 0;
      if (!decodeDictNumber(bytes.data(), bytes.size(), pos, value)) {
        lastError_ = "malformed CFF Top DICT number";
        return false;
      }
      if (operands.size() >= 48) {
        lastError_ = "CFF Top DICT operand stack overflow";
        return false;
      }
      operands.push_back(value);
      continue;
    }

    ++pos;
    uint16_t op = b0;
    if (b0 == 12) {
      if (pos >= bytes.size()) {
        lastError_ = "truncated escaped CFF Top DICT operator";
        return false;
      }
      op = static_cast<uint16_t>(0x0c00u | bytes[pos++]);
    }

    if (op == 17) {  // CharStrings
      if (operands.size() != 1 || operands[0] < 0) {
        lastError_ = "invalid CFF CharStrings offset";
        return false;
      }
      charStringsRel = static_cast<uint32_t>(operands[0]);
      haveCharStrings = true;
    } else if (op == 18) {  // Private: size, offset
      if (operands.size() != 2 || operands[0] < 0 || operands[1] < 0) {
        lastError_ = "invalid CFF Private DICT range";
        return false;
      }
      privateSize = static_cast<uint32_t>(operands[0]);
      privateRel = static_cast<uint32_t>(operands[1]);
    }
    operands.clear();
  }

  if (!haveCharStrings || charStringsRel >= cff_.len) {
    lastError_ = "CFF Top DICT missing CharStrings";
    return false;
  }
  if (privateSize != 0) {
    if (privateRel > cff_.len || privateSize > cff_.len - privateRel) {
      lastError_ = "CFF Private DICT outside table";
      return false;
    }
    privateDict_ = {cff_.off + privateRel, privateSize};
  }

  if (!parseIndex(charStringsRel, charStringsInfo_, nullptr) || charStringsInfo_.count == 0) {
    lastError_ = "invalid or empty CFF CharStrings INDEX";
    return false;
  }
  charStrings_ = charStringsInfo_.whole;
  glyphCount_ = charStringsInfo_.count;
  return true;
}

bool CffFont::init(TtfStream& stream, uint32_t faceOffset) {
  ready_ = false;
  lastError_ = "not initialized";
  stream_ = &stream;
  fileSize_ = stream.size();
  cff_ = Slice{};
  charStringsInfo_ = IndexInfo{};
  globalSubrsInfo_ = IndexInfo{};
  charStrings_ = Slice{};
  globalSubrs_ = Slice{};
  privateDict_ = Slice{};
  glyphCount_ = 0;

  if (faceOffset > fileSize_ || fileSize_ - faceOffset < 12) {
    lastError_ = "OpenType face header out of range";
    return false;
  }
  uint8_t sfnt[12];
  if (!readAt(faceOffset, sfnt, sizeof(sfnt))) {
    lastError_ = "failed to read OpenType face header";
    return false;
  }
  if (rd32be(sfnt) != 0x4f54544fu) {  // 'OTTO'
    lastError_ = "not an OTTO OpenType face";
    return false;
  }
  const uint16_t numTables = rd16be(sfnt + 4);
  if (numTables == 0 || numTables > 128 ||
      static_cast<uint64_t>(faceOffset) + 12u + static_cast<uint64_t>(numTables) * 16u > fileSize_) {
    lastError_ = "invalid OpenType table directory";
    return false;
  }

  const uint32_t cffTag = tag("CFF ");
  const uint32_t cff2Tag = tag("CFF2");
  bool sawCff2 = false;
  for (uint16_t i = 0; i < numTables; ++i) {
    uint8_t rec[16];
    if (!readAt(faceOffset + 12u + static_cast<uint32_t>(i) * 16u, rec, sizeof(rec))) {
      lastError_ = "failed to read OpenType table record";
      return false;
    }
    const uint32_t tableTag = rd32be(rec);
    const uint32_t off = rd32be(rec + 8);
    const uint32_t len = rd32be(rec + 12);
    if (tableTag == cff2Tag) sawCff2 = true;
    if (tableTag == cffTag) {
      if (off > fileSize_ || len > fileSize_ - off || len < 4) {
        lastError_ = "CFF table outside font file";
        return false;
      }
      cff_ = {off, len};
    }
  }
  if (!cff_.valid()) {
    lastError_ = sawCff2 ? "CFF2 face is not supported yet" : "OTTO face has no CFF table";
    return false;
  }

  uint8_t hdr[4];
  if (!readAt(cff_.off, hdr, sizeof(hdr)) || hdr[0] != 1 || hdr[2] < 4 || hdr[2] > cff_.len ||
      hdr[3] < 1 || hdr[3] > 4) {
    lastError_ = "invalid CFF1 header";
    return false;
  }

  uint32_t rel = hdr[2];
  IndexInfo names, topDicts, strings;
  if (!parseIndex(rel, names, &rel) || names.count == 0) {
    lastError_ = "invalid CFF Name INDEX";
    return false;
  }
  if (!parseIndex(rel, topDicts, &rel) || topDicts.count != 1) {
    lastError_ = "CFF Top DICT INDEX must contain one font";
    return false;
  }
  if (!parseIndex(rel, strings, &rel)) {
    lastError_ = "invalid CFF String INDEX";
    return false;
  }
  if (!parseIndex(rel, globalSubrsInfo_, nullptr)) {
    lastError_ = "invalid CFF Global Subr INDEX";
    return false;
  }
  globalSubrs_ = globalSubrsInfo_.whole;

  Slice topDict;
  if (!indexObject(topDicts, 0, topDict)) {
    lastError_ = "failed to locate CFF Top DICT";
    return false;
  }
  if (!parseTopDict(topDict)) return false;

  ready_ = true;
  lastError_ = "ok";
  return true;
}

}  // namespace ttf
