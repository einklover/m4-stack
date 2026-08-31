#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEFAULT_OUTPUT_DIR="${SCRIPT_DIR}/../builtinFonts"
FONT=""
OUTPUT_DIR="$DEFAULT_OUTPUT_DIR"
CHARSET="gb2312-plus"
CODEPOINTS_FILE=""

usage() {
  cat >&2 <<'EOF'
Usage: convert-builtin-fonts.sh --font PATH [--output-dir DIR] [--charset NAME]
                               [--codepoints-file PATH]

PATH must be a locally supplied, legally usable TTF/OTF. Generated headers are
written to firmware/lib/EpdFont/builtinFonts by default; that directory is
generated and intentionally ignored by Git.
EOF
}

while (($#)); do
  case "$1" in
    --font)
      (($# >= 2)) || { usage; exit 2; }
      FONT="$2"
      shift 2
      ;;
    --output-dir)
      (($# >= 2)) || { usage; exit 2; }
      OUTPUT_DIR="$2"
      shift 2
      ;;
    --charset)
      (($# >= 2)) || { usage; exit 2; }
      CHARSET="$2"
      shift 2
      ;;
    --codepoints-file)
      (($# >= 2)) || { usage; exit 2; }
      CODEPOINTS_FILE="$2"
      shift 2
      ;;
    -h|--help)
      usage >&1
      exit 0
      ;;
    *)
      echo "ERROR: unknown argument: $1" >&2
      usage
      exit 2
      ;;
  esac
done

if [[ -z "$FONT" ]]; then
  echo "ERROR: no font supplied. Provide a compatible TTF/OTF with --font PATH." >&2
  echo "       Prefer a pixel-style, CJK-capable font for Chinese UI coverage." >&2
  echo "       Example: $0 --font /path/to/your-font.ttf" >&2
  exit 2
fi
if [[ ! -f "$FONT" ]]; then
  echo "ERROR: font file does not exist: $FONT" >&2
  exit 2
fi
command -v python3 >/dev/null 2>&1 || {
  echo "ERROR: python3 is required; install Python 3 and freetype-py==2.5.1." >&2
  exit 2
}

mkdir -p "$OUTPUT_DIR"

common_args=(--charset "$CHARSET")
if [[ -n "$CODEPOINTS_FILE" ]]; then
  [[ -f "$CODEPOINTS_FILE" ]] || {
    echo "ERROR: codepoint file does not exist: $CODEPOINTS_FILE" >&2
    exit 2
  }
  common_args+=(--codepoints-file "$CODEPOINTS_FILE")
fi

generate() {
  local name="$1"
  local size="$2"
  local mode="$3"
  local output="${OUTPUT_DIR}/${name}.h"
  if [[ "$mode" == "2bit" ]]; then
    python3 "${SCRIPT_DIR}/fontconvert.py" "$name" "$size" "$FONT" "${common_args[@]}" --2bit > "$output"
  else
    python3 "${SCRIPT_DIR}/fontconvert.py" "$name" "$size" "$FONT" "${common_args[@]}" > "$output"
  fi
  echo "Generated $output"
}

# Preserve the existing output names used by legacy profiles and fontIds.h.
# One local face is deliberately used for every role: no proprietary source
# font is assumed or copied into this repository.
for family in bookerly notosans; do
  for size in 12 14 16 18; do
    for style in regular italic bold bolditalic; do
      generate "${family}_${size}_${style}" "$size" 2bit
    done
  done
done
for size in 8 10 12 14; do
  for style in regular italic bold bolditalic; do
    generate "opendyslexic_${size}_${style}" "$size" 2bit
  done
done
for size in 10 12; do
  for style in regular bold; do
    generate "ubuntu_${size}_${style}" "$size" 1bit
  done
done
generate notosans_8_regular 8 1bit

# The M4 application includes these seven faces when OMIT_FONTS is not set.
for size in 13 20 22; do
  generate "notosans_${size}_bold" "$size" 2bit
done

cat > "${OUTPUT_DIR}/all.h" <<'EOF'
#pragma once

#include <builtinFonts/notosans_12_bold.h>
#include <builtinFonts/notosans_13_bold.h>
#include <builtinFonts/notosans_14_bold.h>
#include <builtinFonts/notosans_16_bold.h>
#include <builtinFonts/notosans_18_bold.h>
#include <builtinFonts/notosans_20_bold.h>
#include <builtinFonts/notosans_22_bold.h>
EOF
echo "Generated ${OUTPUT_DIR}/all.h"
