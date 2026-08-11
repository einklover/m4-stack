#pragma once
// Portable book.bin structural validation helpers (host + firmware).
// Pure bounded arithmetic for length-prefix layout; no heap allocation from
// untrusted lengths. Production streams via FsFile (O(1) RAM); host tests use
// the same arithmetic and optional in-memory validateBookBinBuffer.

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>

namespace M4BookCachePolicy {

constexpr uint8_t kBookCacheVersion = 5;
constexpr size_t kMinHeaderBytes = 1 + 4 + 2 + 2;  // version + lutOffset + spine + toc
constexpr uint32_t kMaxStringLen = 4096;            // matches serialization::readString cap
// ESP32 book.bin layout: size_t fields written as 4-byte LE.
constexpr size_t kSerializedSizeTBytes = 4;
constexpr size_t kSerializedInt16Bytes = 2;
constexpr size_t kSerializedU8Bytes = 1;

struct HeaderView {
  uint8_t version = 0;
  uint32_t lutOffset = 0;
  uint16_t spineCount = 0;
  uint16_t tocCount = 0;
  uint32_t fileSize = 0;
};

inline bool headerVersionOk(uint8_t version) { return version == kBookCacheVersion; }
inline bool spineCountPlausible(uint16_t n) { return n <= 50000; }
inline bool tocCountPlausible(uint16_t n) { return n <= 50000; }

// --- Pure arithmetic (streaming and buffer paths share these) ---

// At absolute file offset `pos`, a u32 length-prefix string of `len` bytes is in-bounds.
inline bool stringPayloadInBounds(uint32_t pos, uint32_t fileSize, uint32_t len,
                                  uint32_t maxLen = kMaxStringLen) {
  if (len > maxLen) return false;
  if (static_cast<uint64_t>(pos) + 4u > fileSize) return false;
  if (static_cast<uint64_t>(pos) + 4u + static_cast<uint64_t>(len) > fileSize) return false;
  return true;
}

// After skipping a string that ends at afterStringPos, spine fixed tail fits.
inline bool spineTailInBounds(uint32_t afterStringPos, uint32_t fileSize) {
  return static_cast<uint64_t>(afterStringPos) + kSerializedSizeTBytes + kSerializedInt16Bytes <= fileSize;
}

// After three strings ending at afterStringsPos, toc fixed tail fits.
inline bool tocTailInBounds(uint32_t afterStringsPos, uint32_t fileSize) {
  return static_cast<uint64_t>(afterStringsPos) + kSerializedU8Bytes + kSerializedInt16Bytes <= fileSize;
}

inline uint64_t lutByteCount(uint16_t spineCount, uint16_t tocCount) {
  return static_cast<uint64_t>(spineCount) * 4u + static_cast<uint64_t>(tocCount) * 4u;
}

inline uint32_t dataRegionStart(uint32_t lutOffset, uint16_t spineCount, uint16_t tocCount) {
  return static_cast<uint32_t>(static_cast<uint64_t>(lutOffset) + lutByteCount(spineCount, tocCount));
}

inline bool lutRegionInFile(const HeaderView& h) {
  if (h.fileSize < kMinHeaderBytes) return false;
  if (!spineCountPlausible(h.spineCount) || !tocCountPlausible(h.tocCount)) return false;
  if (h.lutOffset > h.fileSize) return false;
  if (static_cast<uint64_t>(h.lutOffset) + lutByteCount(h.spineCount, h.tocCount) > h.fileSize) return false;
  return true;
}

// LUT entry target must land in data region; caller still walks length prefixes.
inline bool lutTargetPlausible(const HeaderView& h, uint32_t target) {
  const uint32_t dataStart = dataRegionStart(h.lutOffset, h.spineCount, h.tocCount);
  if (target < dataStart) return false;
  // At least the u32 length field of a string must fit.
  if (static_cast<uint64_t>(target) + 4u > h.fileSize) return false;
  return true;
}

// --- In-memory helpers for host regression tests only ---

inline bool readU8(const uint8_t* data, size_t size, size_t& off, uint8_t& out) {
  if (off + 1 > size) return false;
  out = data[off++];
  return true;
}

inline bool readU16LE(const uint8_t* data, size_t size, size_t& off, uint16_t& out) {
  if (off + 2 > size) return false;
  out = static_cast<uint16_t>(data[off] | (static_cast<uint16_t>(data[off + 1]) << 8));
  off += 2;
  return true;
}

inline bool readU32LE(const uint8_t* data, size_t size, size_t& off, uint32_t& out) {
  if (off + 4 > size) return false;
  out = static_cast<uint32_t>(data[off]) | (static_cast<uint32_t>(data[off + 1]) << 8) |
        (static_cast<uint32_t>(data[off + 2]) << 16) | (static_cast<uint32_t>(data[off + 3]) << 24);
  off += 4;
  return true;
}

inline bool skipBoundedString(const uint8_t* data, size_t size, size_t& off, uint32_t maxLen = kMaxStringLen) {
  uint32_t len = 0;
  const size_t pos = off;
  if (!readU32LE(data, size, off, len)) return false;
  if (!stringPayloadInBounds(static_cast<uint32_t>(pos), static_cast<uint32_t>(size), len, maxLen)) {
    off = pos;
    return false;
  }
  off += static_cast<size_t>(len);
  return true;
}

inline bool readBoundedString(const uint8_t* data, size_t size, size_t& off, std::string* out,
                              uint32_t maxLen = kMaxStringLen) {
  uint32_t len = 0;
  const size_t start = off;
  if (!readU32LE(data, size, off, len)) return false;
  if (!stringPayloadInBounds(static_cast<uint32_t>(start), static_cast<uint32_t>(size), len, maxLen)) {
    off = start;
    return false;
  }
  if (out) out->assign(reinterpret_cast<const char*>(data + off), static_cast<size_t>(len));
  off += static_cast<size_t>(len);
  return true;
}

inline bool validateSpineEntryAt(const uint8_t* data, size_t size, uint32_t entryOff) {
  if (static_cast<size_t>(entryOff) >= size) return false;
  size_t off = entryOff;
  if (!skipBoundedString(data, size, off)) return false;
  return spineTailInBounds(static_cast<uint32_t>(off), static_cast<uint32_t>(size));
}

inline bool validateTocEntryAt(const uint8_t* data, size_t size, uint32_t entryOff) {
  if (static_cast<size_t>(entryOff) >= size) return false;
  size_t off = entryOff;
  if (!skipBoundedString(data, size, off)) return false;
  if (!skipBoundedString(data, size, off)) return false;
  if (!skipBoundedString(data, size, off)) return false;
  return tocTailInBounds(static_cast<uint32_t>(off), static_cast<uint32_t>(size));
}

// Host-only full buffer walk (mirrors streaming order for regression tests).
inline bool validateBookBinBuffer(const uint8_t* data, size_t size, HeaderView* outHdr = nullptr) {
  if (!data || size < kMinHeaderBytes) return false;

  size_t off = 0;
  uint8_t version = 0;
  uint32_t lutOffset = 0;
  uint16_t spineCount = 0, tocCount = 0;
  if (!readU8(data, size, off, version) || !headerVersionOk(version)) return false;
  if (!readU32LE(data, size, off, lutOffset)) return false;
  if (!readU16LE(data, size, off, spineCount)) return false;
  if (!readU16LE(data, size, off, tocCount)) return false;

  HeaderView h;
  h.version = version;
  h.lutOffset = lutOffset;
  h.spineCount = spineCount;
  h.tocCount = tocCount;
  h.fileSize = static_cast<uint32_t>(size > 0xFFFFFFFFu ? 0xFFFFFFFFu : size);
  if (!lutRegionInFile(h)) return false;
  if (lutOffset < off) return false;

  size_t metaOff = off;
  for (int i = 0; i < 5; ++i) {
    if (!skipBoundedString(data, size, metaOff)) return false;
  }
  if (metaOff != static_cast<size_t>(lutOffset)) return false;

  size_t lutOff = lutOffset;
  const uint32_t dataStart = dataRegionStart(lutOffset, spineCount, tocCount);
  if (static_cast<size_t>(dataStart) > size) return false;

  for (uint16_t i = 0; i < spineCount; ++i) {
    uint32_t target = 0;
    if (!readU32LE(data, size, lutOff, target)) return false;
    if (!lutTargetPlausible(h, target)) return false;
    if (!validateSpineEntryAt(data, size, target)) return false;
  }
  for (uint16_t i = 0; i < tocCount; ++i) {
    uint32_t target = 0;
    if (!readU32LE(data, size, lutOff, target)) return false;
    if (!lutTargetPlausible(h, target)) return false;
    if (!validateTocEntryAt(data, size, target)) return false;
  }

  if (outHdr) *outHdr = h;
  return true;
}

inline bool structureComplete(const HeaderView& h) { return headerVersionOk(h.version) && lutRegionInFile(h); }

inline const char* bookBinName() { return "/book.bin"; }
inline const char* bookBinTmpName() { return "/book.bin.tmp"; }
inline const char* spineTmpName() { return "/spine.bin.tmp"; }
inline const char* tocTmpName() { return "/toc.bin.tmp"; }

}  // namespace M4BookCachePolicy
