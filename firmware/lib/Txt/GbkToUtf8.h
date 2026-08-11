#pragma once
// ---------------------------------------------------------------------------
// GBK → UTF-8 runtime conversion for TXT-to-EPUB on ESP32.
//
// The lookup table (~47 KB) lives in firmware flash (const DROM).
// Zero heap usage – ESP32 accesses flash through its transparent cache.
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <string>

// Number of GBK trail-byte slots (0x40-0x7E=63, 0x80-0xFE=127 → 190)
constexpr int GBK_TRAILS = 190;

// ---- Encoding detection ---------------------------------------------------

/// Sample the first `len` bytes and return true if valid UTF-8.
/// Returns true for pure-ASCII too (no conversion needed).
/// If false, the caller should assume GBK/GB2312/GB18030.
bool isValidUtf8(const uint8_t* buf, size_t len);

// ---- Table access ---------------------------------------------------------

/// Return a pointer to the GBK→Unicode table in flash (DROM).
/// Always valid – never returns nullptr. No heap allocation.
const uint16_t* getGbkTable();

// ---- Conversion -----------------------------------------------------------

/// Look up a single GBK double-byte pair → Unicode code point.
/// Returns 0 for unmapped pairs.
inline uint16_t gbkLookup(const uint16_t* table, uint8_t lead, uint8_t trail) {
  if (lead < 0x81 || lead > 0xFE) return 0;
  int tidx;
  if      (trail >= 0x40 && trail <= 0x7E) tidx = trail - 0x40;
  else if (trail >= 0x80 && trail <= 0xFE) tidx = trail - 0x80 + 63;
  else return 0;
  return table[(lead - 0x81) * GBK_TRAILS + tidx];
}

/// Convert a chunk of GBK bytes to a UTF-8 std::string.
///
/// @param buf        Input GBK bytes.
/// @param len        Number of bytes in buf.
/// @param table      Pointer from getGbkTable().
/// @param carry      Carry byte from previous chunk (GBK lead without trail).
/// @param hasCarry   true if carry is valid.
///
/// On return, carry/hasCarry are updated for the NEXT chunk.
/// Unmapped code points become U+FFFD (replacement character).
std::string gbkChunkToUtf8(const uint8_t* buf, size_t len,
                           const uint16_t* table,
                           uint8_t& carry, bool& hasCarry);
