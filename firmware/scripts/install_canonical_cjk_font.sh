#!/usr/bin/env bash
# Install the release canonical CJK epdfont onto an SD card root (or staging tree).
#
# Usage:
#   scripts/install_canonical_cjk_font.sh /path/to/sd-root
#   scripts/install_canonical_cjk_font.sh --check /path/to/sd-root
#
# Copies:
#   artifacts/fonts/release/NotoSansCJKsc.epdfont
#     -> <sd-root>/fonts/NotoSansCJKsc.epdfont
#
# Device path after mount: /fonts/NotoSansCJKsc.epdfont
# Required for full WeRead Chinese UI on OMIT_FONTS (Murphy M4) firmware.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${ROOT}/artifacts/fonts/release/NotoSansCJKsc.epdfont"
CHECK_ONLY=0

if [[ "${1:-}" == "--check" ]]; then
  CHECK_ONLY=1
  shift
fi

SD_ROOT="${1:-}"
if [[ -z "${SD_ROOT}" ]]; then
  echo "Usage: $0 [--check] <sd-root>"
  echo "  sd-root is the SD card mount (or a staging directory that will be copied to SD)."
  exit 2
fi

if [[ ! -f "${SRC}" ]]; then
  echo "ERROR: missing release artifact: ${SRC}"
  echo "Build or restore artifacts/fonts/release/NotoSansCJKsc.epdfont first."
  exit 1
fi

DEST_DIR="${SD_ROOT}/fonts"
DEST="${DEST_DIR}/NotoSansCJKsc.epdfont"

if [[ "${CHECK_ONLY}" -eq 1 ]]; then
  if [[ -f "${DEST}" ]]; then
    sz=$(wc -c < "${DEST}" | tr -d ' ')
    echo "OK: ${DEST} present (${sz} bytes)"
    # Light magic check
    magic=$(head -c 4 "${DEST}" | xxd -p)
    if [[ "${magic}" != "45504446" ]]; then
      echo "WARN: magic not EPDF (got ${magic})"
      exit 1
    fi
    exit 0
  fi
  echo "MISSING: ${DEST}"
  exit 1
fi

mkdir -p "${DEST_DIR}"
cp -f "${SRC}" "${DEST}"
sz=$(wc -c < "${DEST}" | tr -d ' ')
echo "Installed ${DEST} (${sz} bytes)"
echo "On device after remount/reboot: /fonts/NotoSansCJKsc.epdfont"
echo "Firmware promotes this file onto UI/SYSTEM font IDs (EpdFontLoader)."
