#pragma once

// Host-testable multi-encoding TXT detection + streaming decode to UTF-8.
//
// Supported: UTF-8 (+BOM), UTF-16LE/BE (+BOM), GBK/GB2312 dual-byte (table).
// NOT supported: GB18030 four-byte — detect returns Unknown + "gb18030_4byte_unsupported".
//
// StreamDecoder contract:
//   decode() returns input bytes consumed. Every consumed byte is either fully
//   emitted as complete code point(s) into out[] or retained in decoder state.
//   Caller must never re-feed consumed bytes.
//   Insufficient outCap: stop without consuming further input; completed CPs that
//   cannot fit are held in pendingCp state.
//   flush() at EOF emits U+FFFD for residual incomplete sequences.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace M4TxtEncoding {

enum class TxtEnc : uint8_t {
  Unknown = 0,
  Utf8 = 1,
  Utf8Bom = 2,
  Gbk = 3,  // dual-byte GBK/GB2312 only
  Utf16Le = 4,
  Utf16Be = 5,
};

// Legacy alias name used in older comments/code: GbkFamily == Gbk
constexpr TxtEnc GbkFamily = TxtEnc::Gbk;

struct DetectResult {
  TxtEnc enc = TxtEnc::Unknown;
  size_t bomBytes = 0;
  const char* name = "unknown";
  const char* diagnostic = "";
};

using GbkLookupFn = uint16_t (*)(uint8_t lead, uint8_t trail);

inline const char* encName(TxtEnc e) {
  switch (e) {
    case TxtEnc::Utf8: return "utf-8";
    case TxtEnc::Utf8Bom: return "utf-8-bom";
    case TxtEnc::Gbk: return "gbk";
    case TxtEnc::Utf16Le: return "utf-16le";
    case TxtEnc::Utf16Be: return "utf-16be";
    default: return "unknown";
  }
}

inline bool isUtf8Family(TxtEnc e) { return e == TxtEnc::Utf8 || e == TxtEnc::Utf8Bom; }
inline bool needsDecodeToUtf8(TxtEnc e) {
  return e == TxtEnc::Gbk || e == TxtEnc::Utf16Le || e == TxtEnc::Utf16Be;
}

inline bool isGb18030FourByteAt(const uint8_t* p, size_t len, size_t i) {
  if (i + 3 >= len) return false;
  const uint8_t a = p[i], b = p[i + 1], c = p[i + 2], d = p[i + 3];
  return a >= 0x81 && a <= 0xFE && b >= 0x30 && b <= 0x39 && c >= 0x81 && c <= 0xFE && d >= 0x30 &&
         d <= 0x39;
}

inline int countGb18030FourByte(const uint8_t* buf, size_t len) {
  int n = 0;
  for (size_t i = 0; i + 3 < len;) {
    if (isGb18030FourByteAt(buf, len, i)) {
      ++n;
      i += 4;
    } else {
      ++i;
    }
  }
  return n;
}

inline bool isStrictUtf8(const uint8_t* buf, size_t len) {
  if (!buf && len) return false;
  for (size_t i = 0; i < len;) {
    const uint8_t b = buf[i];
    if (b < 0x80) {
      ++i;
      continue;
    }
    int seqLen = 0;
    if (b >= 0xC2 && b <= 0xDF) seqLen = 2;
    else if (b >= 0xE0 && b <= 0xEF) seqLen = 3;
    else if (b >= 0xF0 && b <= 0xF4) seqLen = 4;
    else return false;
    if (i + static_cast<size_t>(seqLen) > len) break;
    for (int j = 1; j < seqLen; ++j)
      if ((buf[i + j] & 0xC0) != 0x80) return false;
    if (seqLen == 3) {
      const uint32_t cp = (static_cast<uint32_t>(b & 0x0F) << 12) |
                          (static_cast<uint32_t>(buf[i + 1] & 0x3F) << 6) |
                          static_cast<uint32_t>(buf[i + 2] & 0x3F);
      if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    }
    i += static_cast<size_t>(seqLen);
  }
  return true;
}

struct GbkScore {
  int pairs = 0;
  int bad = 0;
  int fourByte = 0;
};

// Skip up to 3 bytes that look like mid-sequence (UTF-8 continuation) so mid/tail
// samples do not false-negative on GBK/UTF-8 structure.
inline size_t alignSampleStart(const uint8_t* buf, size_t len) {
  if (!buf || len == 0) return 0;
  size_t i = 0;
  while (i < len && i < 3) {
    const uint8_t b = buf[i];
    if ((b & 0xC0) == 0x80) {
      ++i;
      continue;  // UTF-8 continuation
    }
    break;
  }
  return i;
}

inline GbkScore scoreGbkDetail(const uint8_t* buf, size_t len, GbkLookupFn lookup) {
  GbkScore s;
  size_t i = 0;
  while (i < len) {
    if (isGb18030FourByteAt(buf, len, i)) {
      ++s.fourByte;
      i += 4;
      continue;
    }
    const uint8_t b = buf[i];
    if (b < 0x80) {
      ++i;
      continue;
    }
    if (b >= 0x81 && b <= 0xFE && i + 1 < len) {
      const uint8_t t = buf[i + 1];
      const bool trailOk = (t >= 0x40 && t <= 0x7E) || (t >= 0x80 && t <= 0xFE);
      if (trailOk) {
        if (lookup) {
          if (lookup(b, t) != 0)
            ++s.pairs;
          else
            ++s.bad;
        } else {
          ++s.pairs;
        }
        i += 2;
        continue;
      }
    }
    ++s.bad;
    ++i;
  }
  return s;
}

inline int scoreGbkDualByte(const uint8_t* buf, size_t len, GbkLookupFn lookup) {
  const GbkScore s = scoreGbkDetail(buf, len, lookup);
  if (s.pairs == 0) return 0;
  if (s.bad > s.pairs) return s.pairs - s.bad;
  return s.pairs;
}

// Single contiguous sample. isHead: BOM only valid here.
// isCompleteFile: sample spans entire file (allows 1 valid GBK pair if no conflicts).
struct DetectSampleOpts {
  bool isHead = true;
  bool isCompleteFile = false;
  size_t fileSize = 0;
  bool alignStart = false;  // mid/tail samples
};

inline DetectResult detectOne(const uint8_t* sample, size_t len, GbkLookupFn gbkLookup,
                              DetectSampleOpts opt) {
  DetectResult r;
  if (!sample || len == 0) {
    r.enc = TxtEnc::Utf8;
    r.name = encName(r.enc);
    r.diagnostic = "empty_sample_default_utf8";
    return r;
  }

  size_t off = 0;
  if (opt.alignStart) {
    off = alignSampleStart(sample, len);
    if (off >= len) {
      r.enc = TxtEnc::Utf8;
      r.name = encName(r.enc);
      r.diagnostic = "align_consumed_sample";
      return r;
    }
  }
  const uint8_t* p = sample + off;
  const size_t n = len - off;

  if (opt.isHead) {
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
      r.enc = TxtEnc::Utf8Bom;
      r.bomBytes = 3;
      r.name = encName(r.enc);
      return r;
    }
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
      r.enc = TxtEnc::Utf16Le;
      r.bomBytes = 2;
      r.name = encName(r.enc);
      return r;
    }
    if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) {
      r.enc = TxtEnc::Utf16Be;
      r.bomBytes = 2;
      r.name = encName(r.enc);
      return r;
    }
  }

  // GB18030 4-byte evidence anywhere in this sample → unsupported
  if (countGb18030FourByte(p, n) >= 1) {
    r.enc = TxtEnc::Unknown;
    r.name = encName(r.enc);
    r.diagnostic = "gb18030_4byte_unsupported";
    return r;
  }

  if (isStrictUtf8(p, n)) {
    r.enc = TxtEnc::Utf8;
    r.name = encName(r.enc);
    return r;
  }

  const GbkScore gs = scoreGbkDetail(p, n, gbkLookup);
  // Complete small files: 1 table-valid pair and zero conflicts is enough.
  // Larger / truncated samples stay conservative (need ≥2 pairs).
  const bool smallComplete =
      opt.isCompleteFile || (opt.fileSize > 0 && opt.fileSize <= 2048 && opt.isHead && n >= opt.fileSize);
  const int needPairs = smallComplete ? 1 : 2;
  if (gs.pairs >= needPairs && (gs.pairs >= 2 || gs.bad == 0)) {
    r.enc = TxtEnc::Gbk;
    r.name = encName(r.enc);
    r.diagnostic = smallComplete && gs.pairs == 1 ? "heuristic_gbk_short" : "heuristic_gbk";
    return r;
  }

  r.enc = TxtEnc::Unknown;
  r.name = encName(r.enc);
  r.diagnostic = "undetected_or_unsupported";
  return r;
}

// Legacy single-buffer detect (treat as head; complete if small).
inline DetectResult detect(const uint8_t* sample, size_t len, GbkLookupFn gbkLookup = nullptr) {
  DetectSampleOpts opt;
  opt.isHead = true;
  opt.isCompleteFile = true;  // caller passed one buffer; unknown total size → allow short-file path
  opt.fileSize = len;
  opt.alignStart = false;
  return detectOne(sample, len, gbkLookup, opt);
}

// Merge multipoint evidence. BOM-only from head result.
inline DetectResult mergeDetect(const DetectResult* parts, size_t count, size_t fileSize,
                                int totalGbkPairs, int totalGbkBad, bool anyNotUtf8,
                                bool anyGb18030) {
  DetectResult r;
  if (!parts || count == 0) {
    r.enc = TxtEnc::Unknown;
    r.name = encName(r.enc);
    r.diagnostic = "no_samples";
    return r;
  }
  // Head BOM / UTF-16 win immediately (should already be short-circuited by caller)
  if (parts[0].enc == TxtEnc::Utf8Bom || parts[0].enc == TxtEnc::Utf16Le ||
      parts[0].enc == TxtEnc::Utf16Be) {
    return parts[0];
  }
  if (anyGb18030) {
    r.enc = TxtEnc::Unknown;
    r.name = encName(r.enc);
    r.diagnostic = "gb18030_4byte_unsupported";
    return r;
  }
  if (!anyNotUtf8) {
    r.enc = TxtEnc::Utf8;
    r.name = encName(r.enc);
    r.diagnostic = "multipoint_utf8";
    return r;
  }
  const bool smallFile = fileSize > 0 && fileSize <= 2048;
  const int needPairs = smallFile ? 1 : 2;
  if (totalGbkPairs >= needPairs && (totalGbkPairs >= 2 || totalGbkBad == 0)) {
    r.enc = TxtEnc::Gbk;
    r.name = encName(r.enc);
    r.diagnostic = "multipoint_gbk";
    return r;
  }
  r.enc = TxtEnc::Unknown;
  r.name = encName(r.enc);
  r.diagnostic = "undetected_or_unsupported";
  return r;
}

// Pure multipoint detect over a full in-memory file image (host tests + production
// when samples are assembled). Fixed sampling budget:
//   head ≤ 1024, mid ≤ 256, tail ≤ 256 (with 3-byte align overlap).
inline DetectResult detectMulti(const uint8_t* file, size_t fileSize, GbkLookupFn gbkLookup = nullptr) {
  DetectResult r;
  if (!file || fileSize == 0) {
    r.enc = TxtEnc::Utf8;
    r.name = encName(r.enc);
    r.diagnostic = "empty_sample_default_utf8";
    return r;
  }

  constexpr size_t kHeadMax = 1024;
  constexpr size_t kBlock = 256;
  constexpr size_t kAlignPad = 3;

  const size_t headLen = fileSize < kHeadMax ? fileSize : kHeadMax;
  DetectSampleOpts headOpt;
  headOpt.isHead = true;
  headOpt.isCompleteFile = (headLen == fileSize);
  headOpt.fileSize = fileSize;
  DetectResult head = detectOne(file, headLen, gbkLookup, headOpt);
  if (head.enc == TxtEnc::Utf8Bom || head.enc == TxtEnc::Utf16Le || head.enc == TxtEnc::Utf16Be) {
    return head;
  }
  if (headLen == fileSize) {
    return head;  // whole file already analyzed
  }

  int totalPairs = 0, totalBad = 0;
  bool anyNotUtf8 = false;
  bool anyGb18030 = false;

  auto accumulate = [&](const uint8_t* p, size_t n, bool align) {
    DetectSampleOpts o;
    o.isHead = false;
    o.isCompleteFile = false;
    o.fileSize = fileSize;
    o.alignStart = align;
    size_t off = align ? alignSampleStart(p, n) : 0;
    if (off >= n) return;
    const uint8_t* q = p + off;
    const size_t m = n - off;
    if (countGb18030FourByte(q, m) >= 1) anyGb18030 = true;
    if (!isStrictUtf8(q, m)) anyNotUtf8 = true;
    const GbkScore gs = scoreGbkDetail(q, m, gbkLookup);
    totalPairs += gs.pairs;
    totalBad += gs.bad;
    if (gs.fourByte > 0) anyGb18030 = true;
  };

  // Head contribution
  if (countGb18030FourByte(file, headLen) >= 1) anyGb18030 = true;
  if (!isStrictUtf8(file, headLen)) anyNotUtf8 = true;
  {
    const GbkScore gs = scoreGbkDetail(file, headLen, gbkLookup);
    totalPairs += gs.pairs;
    totalBad += gs.bad;
  }

  // Mid block with align pad
  if (fileSize > headLen) {
    size_t midCenter = fileSize / 2;
    size_t midStart = midCenter > kAlignPad ? midCenter - kAlignPad : 0;
    if (midStart + kBlock + kAlignPad > fileSize) {
      midStart = fileSize > (kBlock + kAlignPad) ? fileSize - (kBlock + kAlignPad) : 0;
    }
    size_t midLen = fileSize - midStart;
    if (midLen > kBlock + kAlignPad) midLen = kBlock + kAlignPad;
    accumulate(file + midStart, midLen, true);
  }

  // Tail block
  if (fileSize > headLen) {
    size_t tailStart = fileSize > (kBlock + kAlignPad) ? fileSize - (kBlock + kAlignPad) : 0;
    if (tailStart < headLen && fileSize > headLen) {
      // avoid pure re-read of head on small files; still ok if overlap
    }
    size_t tailLen = fileSize - tailStart;
    accumulate(file + tailStart, tailLen, true);
  }

  DetectResult parts[1] = {head};
  return mergeDetect(parts, 1, fileSize, totalPairs, totalBad, anyNotUtf8, anyGb18030);
}

inline size_t utf8LenForCp(uint32_t cp) {
  if (cp < 0x80) return 1;
  if (cp < 0x800) return 2;
  if (cp < 0x10000) return 3;
  return 4;
}

inline bool tryAppendUtf8Cp(char* out, size_t cap, size_t& n, uint32_t cp) {
  const size_t need = utf8LenForCp(cp);
  if (n + need > cap) return false;
  if (cp < 0x80) {
    out[n++] = static_cast<char>(cp);
  } else if (cp < 0x800) {
    out[n++] = static_cast<char>(0xC0 | (cp >> 6));
    out[n++] = static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out[n++] = static_cast<char>(0xE0 | (cp >> 12));
    out[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[n++] = static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out[n++] = static_cast<char>(0xF0 | (cp >> 18));
    out[n++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[n++] = static_cast<char>(0x80 | (cp & 0x3F));
  }
  return true;
}

// Per code-point boundary: utf8End exclusive in this call's out[]; rawEnd exclusive
// relative to this call's in[0].
struct CpBoundary {
  uint32_t utf8End = 0;
  uint32_t rawEnd = 0;
};

struct StreamDecoder {
  TxtEnc enc = TxtEnc::Utf8;

  uint8_t u8buf[4]{};
  uint8_t u8len = 0;
  int u8need = 0;

  bool gbkHasLead = false;
  uint8_t gbkLead = 0;

  bool u16HasByte = false;
  uint8_t u16Byte = 0;
  bool u16HasHi = false;
  uint16_t u16Hi = 0;

  // Held unit after emitting FFFD for lone high surrogate (not yet reprocessed).
  bool u16HasHeld = false;
  uint16_t u16Held = 0;
  uint32_t u16HeldRawEnd = 0;

  bool hasPendingCp = false;
  uint32_t pendingCp = 0;
  uint32_t pendingRawEnd = 0;

  void reset(TxtEnc e) {
    enc = e;
    u8len = 0;
    u8need = 0;
    gbkHasLead = false;
    gbkLead = 0;
    u16HasByte = false;
    u16Byte = 0;
    u16HasHi = false;
    u16Hi = 0;
    u16HasHeld = false;
    u16Held = 0;
    u16HeldRawEnd = 0;
    hasPendingCp = false;
    pendingCp = 0;
    pendingRawEnd = 0;
  }

  bool hasResidual() const {
    return hasPendingCp || u8len || gbkHasLead || u16HasByte || u16HasHi || u16HasHeld;
  }

  bool tryDrainPending(char* out, size_t outCap, size_t& written, CpBoundary* map, size_t mapCap,
                       size_t& mapLen) {
    if (!hasPendingCp) return true;
    if (!tryAppendUtf8Cp(out, outCap, written, pendingCp)) return false;
    if (map && mapLen < mapCap) map[mapLen++] = {static_cast<uint32_t>(written), pendingRawEnd};
    hasPendingCp = false;
    return true;
  }

  bool emit(char* out, size_t outCap, size_t& written, uint32_t cp, uint32_t rawEnd, CpBoundary* map,
            size_t mapCap, size_t& mapLen) {
    if (!tryAppendUtf8Cp(out, outCap, written, cp)) {
      hasPendingCp = true;
      pendingCp = cp;
      pendingRawEnd = rawEnd;
      return false;
    }
    if (map && mapLen < mapCap) map[mapLen++] = {static_cast<uint32_t>(written), rawEnd};
    return true;
  }

  // UTF-16: assemble one unit from state+input; returns false if need more input.
  // On success, *rawEnd is exclusive index into current in[] (bytes from this call only
  // plus conceptually prior half-byte ends at same index).
  bool takeUtf16Unit(const uint8_t* in, size_t inLen, size_t& i, uint16_t& unit, uint32_t& rawEnd) {
    if (u16HasByte) {
      if (i >= inLen) return false;
      const uint8_t b1 = in[i++];
      if (enc == TxtEnc::Utf16Le)
        unit = static_cast<uint16_t>(u16Byte) | (static_cast<uint16_t>(b1) << 8);
      else
        unit = static_cast<uint16_t>(b1) | (static_cast<uint16_t>(u16Byte) << 8);
      u16HasByte = false;
      rawEnd = static_cast<uint32_t>(i);
      return true;
    }
    if (i + 1 >= inLen) {
      if (i < inLen) {
        u16Byte = in[i++];
        u16HasByte = true;
      }
      return false;
    }
    const uint8_t b0 = in[i], b1 = in[i + 1];
    i += 2;
    if (enc == TxtEnc::Utf16Le)
      unit = static_cast<uint16_t>(b0) | (static_cast<uint16_t>(b1) << 8);
    else
      unit = static_cast<uint16_t>(b1) | (static_cast<uint16_t>(b0) << 8);
    rawEnd = static_cast<uint32_t>(i);
    return true;
  }

  // Process one UTF-16 unit already obtained (may set hi / held).
  // Returns false if emit failed (pending set); unit is considered consumed.
  bool processUtf16Unit(uint16_t unit, uint32_t rawEnd, char* out, size_t outCap, size_t& written,
                        CpBoundary* map, size_t mapCap, size_t& mapLen) {
    if (u16HasHi) {
      if (unit >= 0xDC00 && unit <= 0xDFFF) {
        const uint32_t cp =
            0x10000 + ((static_cast<uint32_t>(u16Hi - 0xD800) << 10) | (unit - 0xDC00));
        u16HasHi = false;
        return emit(out, outCap, written, cp, rawEnd, map, mapCap, mapLen);
      }
      // Lone high surrogate
      if (!emit(out, outCap, written, 0xFFFD, rawEnd, map, mapCap, mapLen)) {
        // Keep hi; also need to reprocess unit — stash held
        u16HasHeld = true;
        u16Held = unit;
        u16HeldRawEnd = rawEnd;
        // hi still set — but we also need FFFD for hi. pending has FFFD.
        // Clear hi only after FFFD emits. pending is FFFD for hi; after drain re-set held.
        // Simpler: pending FFFD replaces hi; clear hi now; held has unit.
        u16HasHi = false;
        return false;
      }
      u16HasHi = false;
      // fallthrough to process unit
    }
    if (unit >= 0xD800 && unit <= 0xDBFF) {
      u16HasHi = true;
      u16Hi = unit;
      return true;
    }
    if (unit >= 0xDC00 && unit <= 0xDFFF) {
      return emit(out, outCap, written, 0xFFFD, rawEnd, map, mapCap, mapLen);
    }
    return emit(out, outCap, written, unit, rawEnd, map, mapCap, mapLen);
  }

  size_t decode(const uint8_t* in, size_t inLen, char* out, size_t outCap, size_t* outLen,
                GbkLookupFn gbkLookup, CpBoundary* map = nullptr, size_t mapCap = 0,
                size_t* mapLenOut = nullptr) {
    size_t written = 0;
    size_t mapLen = 0;
    size_t i = 0;

    if (!tryDrainPending(out, outCap, written, map, mapCap, mapLen)) {
      if (outLen) *outLen = written;
      if (mapLenOut) *mapLenOut = mapLen;
      return 0;
    }

    // Drain held UTF-16 unit from previous insufficient-out after lone-hi FFFD
    if ((enc == TxtEnc::Utf16Le || enc == TxtEnc::Utf16Be) && u16HasHeld) {
      u16HasHeld = false;
      if (!processUtf16Unit(u16Held, u16HeldRawEnd, out, outCap, written, map, mapCap, mapLen)) {
        if (outLen) *outLen = written;
        if (mapLenOut) *mapLenOut = mapLen;
        return 0;
      }
    }

    if (isUtf8Family(enc) || enc == TxtEnc::Unknown) {
      while (true) {
        if (u8len > 0) {
          while (u8len < static_cast<uint8_t>(u8need) && i < inLen) u8buf[u8len++] = in[i++];
          if (u8len < static_cast<uint8_t>(u8need)) break;
          bool ok = true;
          for (int j = 1; j < u8need; ++j)
            if ((u8buf[j] & 0xC0) != 0x80) {
              ok = false;
              break;
            }
          uint32_t cp = 0xFFFD;
          if (ok) {
            if (u8need == 2)
              cp = ((u8buf[0] & 0x1F) << 6) | (u8buf[1] & 0x3F);
            else if (u8need == 3)
              cp = ((u8buf[0] & 0x0F) << 12) | ((u8buf[1] & 0x3F) << 6) | (u8buf[2] & 0x3F);
            else
              cp = ((u8buf[0] & 0x07) << 18) | ((u8buf[1] & 0x3F) << 12) | ((u8buf[2] & 0x3F) << 6) |
                   (u8buf[3] & 0x3F);
          }
          u8len = 0;
          u8need = 0;
          if (!emit(out, outCap, written, cp, static_cast<uint32_t>(i), map, mapCap, mapLen)) {
            if (outLen) *outLen = written;
            if (mapLenOut) *mapLenOut = mapLen;
            return i;
          }
          continue;
        }
        if (i >= inLen) break;
        const uint8_t b = in[i];
        if (b < 0x80) {
          if (!emit(out, outCap, written, b, static_cast<uint32_t>(i + 1), map, mapCap, mapLen)) {
            if (outLen) *outLen = written;
            if (mapLenOut) *mapLenOut = mapLen;
            return i;
          }
          ++i;
          continue;
        }
        int seqLen = 0;
        if (b >= 0xC2 && b <= 0xDF) seqLen = 2;
        else if (b >= 0xE0 && b <= 0xEF) seqLen = 3;
        else if (b >= 0xF0 && b <= 0xF4) seqLen = 4;
        else {
          if (!emit(out, outCap, written, 0xFFFD, static_cast<uint32_t>(i + 1), map, mapCap, mapLen)) {
            if (outLen) *outLen = written;
            if (mapLenOut) *mapLenOut = mapLen;
            return i;
          }
          ++i;
          continue;
        }
        u8buf[0] = b;
        u8len = 1;
        u8need = seqLen;
        ++i;
      }
    } else if (enc == TxtEnc::Gbk) {
      while (true) {
        if (gbkHasLead) {
          if (i >= inLen) break;
          const uint8_t trail = in[i];
          const uint16_t cp = gbkLookup ? gbkLookup(gbkLead, trail) : 0;
          if (!emit(out, outCap, written, cp ? cp : 0xFFFD, static_cast<uint32_t>(i + 1), map, mapCap,
                    mapLen)) {
            if (outLen) *outLen = written;
            if (mapLenOut) *mapLenOut = mapLen;
            return i;  // lead kept, trail not consumed
          }
          gbkHasLead = false;
          ++i;
          continue;
        }
        if (i >= inLen) break;
        const uint8_t b = in[i];
        if (b < 0x80) {
          if (!emit(out, outCap, written, b, static_cast<uint32_t>(i + 1), map, mapCap, mapLen)) {
            if (outLen) *outLen = written;
            if (mapLenOut) *mapLenOut = mapLen;
            return i;
          }
          ++i;
          continue;
        }
        if (b >= 0x81 && b <= 0xFE) {
          gbkLead = b;
          gbkHasLead = true;
          ++i;
          continue;
        }
        ++i;  // stray
      }
    } else {
      // UTF-16
      while (true) {
        // Ensure room for worst-case 4-byte CP before taking more input
        if (written + 4 > outCap && !u16HasHi && !u16HasHeld) {
          // Might still fit smaller CPs; check minimum 1
          if (written + 1 > outCap) break;
        }
        uint16_t unit = 0;
        uint32_t rawEnd = 0;
        if (!takeUtf16Unit(in, inLen, i, unit, rawEnd)) break;
        // Before processing hi-pair, ensure room for 4 bytes if hi pending + low
        if (u16HasHi && written + 4 > outCap) {
          // Put unit back: we cannot un-take easily if from u16HasByte.
          // Stash as held and stop — but hi still set. On next call with larger out, process.
          u16HasHeld = true;
          u16Held = unit;
          u16HeldRawEnd = rawEnd;
          break;
        }
        if (!processUtf16Unit(unit, rawEnd, out, outCap, written, map, mapCap, mapLen)) {
          if (outLen) *outLen = written;
          if (mapLenOut) *mapLenOut = mapLen;
          return i;
        }
      }
    }

    if (outLen) *outLen = written;
    if (mapLenOut) *mapLenOut = mapLen;
    return i;
  }

  size_t flush(char* out, size_t outCap, size_t* outLen, CpBoundary* map = nullptr, size_t mapCap = 0,
               size_t* mapLenOut = nullptr) {
    size_t written = 0;
    size_t mapLen = 0;
    if (!tryDrainPending(out, outCap, written, map, mapCap, mapLen)) {
      if (outLen) *outLen = written;
      if (mapLenOut) *mapLenOut = mapLen;
      return 0;
    }
    auto one = [&](uint32_t cp) {
      return emit(out, outCap, written, cp, 0, map, mapCap, mapLen);
    };
    if (u8len > 0) {
      if (!one(0xFFFD)) goto done;
      u8len = 0;
      u8need = 0;
    }
    if (gbkHasLead) {
      if (!one(0xFFFD)) goto done;
      gbkHasLead = false;
    }
    if (u16HasByte) {
      if (!one(0xFFFD)) goto done;
      u16HasByte = false;
    }
    if (u16HasHi) {
      if (!one(0xFFFD)) goto done;
      u16HasHi = false;
    }
    if (u16HasHeld) {
      // process held as unit at EOF
      if (u16Held >= 0xD800 && u16Held <= 0xDBFF) {
        if (!one(0xFFFD)) goto done;
      } else if (u16Held >= 0xDC00 && u16Held <= 0xDFFF) {
        if (!one(0xFFFD)) goto done;
      } else {
        if (!one(u16Held)) goto done;
      }
      u16HasHeld = false;
    }
  done:
    if (outLen) *outLen = written;
    if (mapLenOut) *mapLenOut = mapLen;
    return written;
  }
};

// Exact window decode for direct-read paging.
struct WindowDecodeResult {
  size_t nextRaw = 0;
  size_t utf8Len = 0;
  size_t mapLen = 0;
};

inline WindowDecodeResult decodeWindow(const uint8_t* file, size_t fileSize, TxtEnc enc, size_t bomSkip,
                                       size_t rawStart, char* out, size_t outCap, GbkLookupFn gbkLookup,
                                       CpBoundary* map, size_t mapCap) {
  WindowDecodeResult r;
  r.nextRaw = rawStart;
  if (!file || rawStart >= fileSize || !out || outCap == 0) return r;

  size_t pos = rawStart;
  if (rawStart == 0 && bomSkip > 0 && bomSkip <= fileSize) pos = bomSkip;
  if (pos >= fileSize) {
    r.nextRaw = pos;
    return r;
  }

  StreamDecoder dec;
  dec.reset(enc);
  size_t outLen = 0, mapLen = 0;
  const size_t avail = fileSize - pos;
  dec.decode(file + pos, avail, out, outCap, &outLen, gbkLookup, map, mapCap, &mapLen);

  r.utf8Len = outLen;
  r.mapLen = mapLen;
  if (mapLen > 0) {
    r.nextRaw = pos + map[mapLen - 1].rawEnd;
  } else {
    r.nextRaw = pos;  // nothing emitted; do not skip residual mid-sequence
  }

  // Only flush residual at true EOF when nothing more can be read into decoder
  // and we have residual that was held without more input available.
  if (r.nextRaw >= fileSize || (pos + avail <= fileSize && r.nextRaw == pos && dec.hasResidual() && outLen == 0)) {
    // If at end of file and residual, flush into remaining out space
    if (pos + avail >= fileSize && dec.hasResidual() && outLen < outCap) {
      size_t fl = 0, m2 = 0;
      dec.flush(out + outLen, outCap - outLen, &fl, map ? map + mapLen : nullptr, mapCap > mapLen ? mapCap - mapLen : 0,
                &m2);
      r.utf8Len = outLen + fl;
      r.mapLen = mapLen + m2;
      r.nextRaw = fileSize;
    }
  }
  return r;
}

// Given CP map from a window starting at `pos`, map utf8 prefix length → absolute raw end.
inline size_t absoluteRawEndForUtf8Prefix(size_t windowPos, const CpBoundary* map, size_t mapLen,
                                          size_t utf8Prefix, size_t fallbackAbs) {
  if (!map || mapLen == 0) return fallbackAbs;
  uint32_t bestRel = 0;
  bool any = false;
  for (size_t i = 0; i < mapLen; ++i) {
    if (map[i].utf8End <= utf8Prefix) {
      bestRel = map[i].rawEnd;
      any = true;
    } else
      break;
  }
  if (!any) return windowPos;
  return windowPos + bestRel;
}

// Page by maximum UTF-8 output bytes (simulates mid-paragraph page splits without
// font metrics). nextRaw is exact via CpBoundary; never proportional.
inline size_t pageNextRawByMaxUtf8(const uint8_t* file, size_t fileSize, TxtEnc enc, size_t bomSkip,
                                   size_t rawStart, size_t maxUtf8, GbkLookupFn gbkLookup, char* scratch,
                                   size_t scratchCap, CpBoundary* map, size_t mapCap, size_t* outUtf8Len) {
  if (outUtf8Len) *outUtf8Len = 0;
  if (maxUtf8 == 0 || rawStart >= fileSize || scratchCap == 0) return rawStart;
  const size_t cap = maxUtf8 < scratchCap ? maxUtf8 : scratchCap;
  auto wr = decodeWindow(file, fileSize, enc, bomSkip, rawStart, scratch, cap, gbkLookup, map, mapCap);
  if (outUtf8Len) *outUtf8Len = wr.utf8Len;
  if (wr.utf8Len == 0) {
    if (rawStart < fileSize) {
      // Force align progress
      if (enc == TxtEnc::Utf16Le || enc == TxtEnc::Utf16Be) {
        size_t a = rawStart + 2 - (rawStart % 2);
        return a > fileSize ? fileSize : a;
      }
      return rawStart + 1;
    }
    return rawStart;
  }
  return wr.nextRaw;
}

// Host/firmware pure sequential line paging (exact nextRaw).
inline size_t pageNextRawByLines(const uint8_t* file, size_t fileSize, TxtEnc enc, size_t bomSkip,
                                 size_t rawStart, int maxLines, GbkLookupFn gbkLookup, char* scratch,
                                 size_t scratchCap, CpBoundary* map, size_t mapCap) {
  if (maxLines <= 0 || rawStart >= fileSize) return rawStart;
  auto wr = decodeWindow(file, fileSize, enc, bomSkip, rawStart, scratch, scratchCap, gbkLookup, map, mapCap);
  if (wr.utf8Len == 0) {
    // Avoid infinite loop: if residual incomplete at EOF, advance to end
    if (rawStart < fileSize && wr.nextRaw == rawStart) {
      // try larger? stuck — advance by aligned unit
      if (enc == TxtEnc::Utf16Le || enc == TxtEnc::Utf16Be) {
        size_t a = rawStart + (rawStart % 2);
        if (a == rawStart) a += 2;
        return a > fileSize ? fileSize : a;
      }
      return rawStart + 1 > fileSize ? fileSize : rawStart + 1;
    }
    return wr.nextRaw;
  }
  size_t pos = rawStart;
  if (rawStart == 0 && bomSkip > 0 && bomSkip <= fileSize) pos = bomSkip;

  int lines = 0;
  size_t utf8Cut = wr.utf8Len;
  for (size_t i = 0; i < wr.utf8Len; ++i) {
    if (scratch[i] == '\n') {
      ++lines;
      if (lines >= maxLines) {
        utf8Cut = i + 1;
        break;
      }
    }
  }
  if (lines < maxLines) return wr.nextRaw;
  return absoluteRawEndForUtf8Prefix(pos, map, wr.mapLen, utf8Cut, wr.nextRaw);
}

}  // namespace M4TxtEncoding
