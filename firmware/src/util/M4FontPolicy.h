#pragma once

// Host-testable Murphy M4 SD font selection policy (no Arduino dependencies).
//
// Rules:
//  A) Only the verified canonical family "NotoSansCJKsc" may be auto-promoted
//     onto NOTOSANS reader/content IDs. UI / SMALL keep compact builtin metrics.
//     Never use families.front() as fallback.
//  B) SYSTEM_FONT + canonical present: promote canonical for content without
//     mutating settings to FONT_CUSTOM.
//     FONT_CUSTOM: load the explicitly selected family for reader hash IDs.
//     If that file is missing: diagnostic + fall back to canonical system CJK
//     (or offline subset if canonical absent). Never promote a non-canonical
//     auto-picked family onto UI/NOTOSANS.

#include <cstring>
#include <string>
#include <vector>

namespace M4FontPolicy {

constexpr const char* kCanonicalFamily = "NotoSansCJKsc";
constexpr const char* kCanonicalSdPath = "/fonts/NotoSansCJKsc.epdfont";

// Settings mirror (avoid pulling CrossPointSettings into host tests).
enum class FamilyMode : uint8_t { System = 0, Custom = 1 };

struct Inputs {
  bool hasCanonical = false;                 // /fonts/NotoSansCJKsc.epdfont present
  std::vector<std::string> availableFamilies;  // scanned /fonts/*.epdfont basenames
  FamilyMode mode = FamilyMode::System;
  std::string customFamily;                  // SETTINGS.customFontFamily
};

struct Decision {
  // Load this family under custom hash IDs for getReaderFontId() when mode==Custom.
  // Empty if no custom load (SYSTEM, or missing custom).
  std::string loadCustomFamily;

  // Promote this family onto NOTOSANS reader/content IDs via replaceFont.
  // UI_10/UI_12/SMALL remain compact. Empty = leave all offline subsets.
  // Only ever the canonical name, never an arbitrary Latin-only first file.
  std::string promoteSystemFamily;

  // Whether settings should be mutated (should always stay false for auto canonical).
  bool mutateSettingsToCustom = false;

  // Human-readable diagnostic (empty if none).
  std::string diagnostic;

  // True if explicit CUSTOM selection was requested but file missing.
  bool customMissing = false;
};

inline bool familyAvailable(const Inputs& in, const std::string& name) {
  if (name.empty()) return false;
  for (const auto& f : in.availableFamilies) {
    if (f == name) return true;
  }
  return false;
}

inline Decision decide(const Inputs& in) {
  Decision d;

  // Canonical content-font promotion is independent of FONT_CUSTOM / stale custom names.
  if (in.hasCanonical) {
    d.promoteSystemFamily = kCanonicalFamily;
  }

  if (in.mode == FamilyMode::System) {
    // Do not mutate to CUSTOM merely because canonical exists.
    d.mutateSettingsToCustom = false;
    d.loadCustomFamily.clear();
    if (!in.hasCanonical) {
      d.diagnostic =
          "no canonical /fonts/NotoSansCJKsc.epdfont; offline native-grid 15x16 reader "
          "+ compact UI CJK (other epdfonts are not auto-promoted)";
    }
    return d;
  }

  // FONT_CUSTOM: explicit user selection only.
  const std::string& sel = in.customFamily;
  if (sel.empty()) {
    d.customMissing = true;
    d.diagnostic = "FONT_CUSTOM but customFontFamily empty; falling back to system CJK policy";
    // promoteSystemFamily already set if canonical present
    return d;
  }

  if (familyAvailable(in, sel) || (sel == kCanonicalFamily && in.hasCanonical)) {
    d.loadCustomFamily = sel;
    // Never promote a non-canonical custom face onto standard content IDs.
    // promoteSystemFamily remains canonical-only (or empty).
    return d;
  }

  // Explicit selection missing on SD.
  d.customMissing = true;
  d.loadCustomFamily.clear();
  d.diagnostic = "selected custom family '" + sel +
                 "' missing on SD; falling back to canonical system CJK (or offline subset)";
  return d;
}

// Fixed generated pixel size of the current release artifact (fontconvert @ 16pt).
// Size enum still selects hash IDs / layout metrics intent, but the bitmap metrics
// are those of the single 16pt epdfont until multi-size artifacts ship.
constexpr int kCanonicalEpdfontPixelSize = 16;

// Actual raster pixel size of the compact built-in 2-bit CJK face. It is
// generated at 16pt (@150 DPI), but the em box rounds down to a 14px raster
// (generated header reports "raster pixel size: 14"; advanceY=20, ascender=15).
// System-reader binds must divide the requested px by this real raster, not by
// kCanonicalEpdfontPixelSize, or every compact-sourced size renders ~12% small.
constexpr int kCompactCjkSourcePx = 14;

// Native-grid builtin reader face is a true 16-row raster (15x16 1-bit cells).
constexpr int kNativeGridSourcePx = 16;

// Divisor for ScaledEpdFont::bind when sizing the system reader face: the
// bound source's actual raster pixels. Compact 2-bit chrome/legacy sources
// divide by 14; native-grid and canonical SD epdfont sources divide by 16.
inline int systemReaderSourcePx(bool compact2BitSource) {
  return compact2BitSource ? kCompactCjkSourcePx : kCanonicalEpdfontPixelSize;
}

// RC1 expected SHA-256 of the release canonical SD artifact (document only;
// runtime does not require matching hash to boot — invalid header still fails).
constexpr const char* kCanonicalArtifactSha256 =
    "44b5164bb1dd1f59e9230a5c81383a6dc7a5e8559103fbf4bbd0a69b224919f4";

// Header-only preflight (does not imply successful promotion into renderer).
enum class PreflightStatus : uint8_t { Missing = 0, Invalid, ValidHeader };

struct Preflight {
  PreflightStatus status = PreflightStatus::Missing;
  uint32_t fileSize = 0;
  int headerVersion = -1;
  bool magicOk = false;
  bool offsetsOk = false;
  std::string diagnostic;
};

// Result of actual canonical load/promotion into renderer IDs.
enum class LoadResult : uint8_t {
  NotAttempted = 0,
  Missing,       // file absent → subset mode
  InvalidHeader, // header rejected
  LoadFailed,    // header ok but FontManager/load failed
  Promoted,      // successfully replaced NOTOSANS reader/content IDs
};

inline const char* loadResultName(LoadResult r) {
  switch (r) {
    case LoadResult::NotAttempted: return "not_attempted";
    case LoadResult::Missing: return "missing";
    case LoadResult::InvalidHeader: return "invalid_header";
    case LoadResult::LoadFailed: return "load_failed";
    case LoadResult::Promoted: return "promoted";
  }
  return "unknown";
}

// BOOT_SUMMARY font=ok only when Missing (subset) or Promoted.
// NotAttempted / InvalidHeader / LoadFailed are not ok.
inline bool bootSummaryFontOk(LoadResult r) {
  return r == LoadResult::Missing || r == LoadResult::Promoted;
}

// Validate first 48 bytes of an EPDF file (mirrors FontManager heuristics lightly).
inline Preflight preflightFromHeader(const uint8_t* buf48, size_t fileSize, bool pathExists) {
  Preflight p;
  if (!pathExists) {
    p.status = PreflightStatus::Missing;
    p.diagnostic = "canonical /fonts/NotoSansCJKsc.epdfont missing";
    return p;
  }
  p.fileSize = static_cast<uint32_t>(fileSize);
  if (!buf48 || fileSize < 48) {
    p.status = PreflightStatus::Invalid;
    p.diagnostic = "canonical epdfont too small";
    return p;
  }
  p.magicOk = (buf48[0] == 'E' && buf48[1] == 'P' && buf48[2] == 'D' && buf48[3] == 'F');
  if (!p.magicOk) {
    p.status = PreflightStatus::Invalid;
    p.diagnostic = "canonical epdfont bad magic (expected EPDF)";
    return p;
  }
  uint32_t words[12];
  for (int i = 0; i < 12; ++i) {
    words[i] = static_cast<uint32_t>(buf48[i * 4]) | (static_cast<uint32_t>(buf48[i * 4 + 1]) << 8) |
               (static_cast<uint32_t>(buf48[i * 4 + 2]) << 16) | (static_cast<uint32_t>(buf48[i * 4 + 3]) << 24);
  }
  uint32_t oi = words[9], og = words[10], ob = words[11];
  p.headerVersion = 0;
  if (!(oi > 0 && og > oi && ob > og && ob < fileSize)) {
    const uint8_t* b8 = buf48;
    oi = b8[20] | (b8[21] << 8) | (b8[22] << 16) | (b8[23] << 24);
    og = b8[24] | (b8[25] << 8) | (b8[26] << 16) | (b8[27] << 24);
    ob = b8[28] | (b8[29] << 8) | (b8[30] << 16) | (b8[31] << 24);
    p.headerVersion = 1;
  }
  p.offsetsOk = (oi > 0 && og > oi && ob > og && ob < fileSize);
  if (!p.offsetsOk) {
    p.status = PreflightStatus::Invalid;
    p.diagnostic = "canonical epdfont offsets invalid";
    return p;
  }
  p.status = PreflightStatus::ValidHeader;
  p.diagnostic = "canonical header ok family=NotoSansCJKsc pt=";
  p.diagnostic += std::to_string(kCanonicalEpdfontPixelSize);
  return p;
}

}  // namespace M4FontPolicy
