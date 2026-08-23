#!/usr/bin/env python3
"""Reproducibly generate the tracked compact M4 CJK fallback header.

The source font is intentionally an input, not a repository dependency. The
checked-in charset and this script are the reproducibility boundary; the
generated header is a cropped, packed 2-bit EpdFont bitmap and uses no runtime
decoder beyond the existing EpdFont path.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CHARSET = ROOT / "firmware/src/fontdata/m4_compact_cjk_charset.txt"
DEFAULT_OUTPUT = ROOT / "firmware/src/fontdata/m4_compact_cjk_16.h"
FONTCONVERT = ROOT / "firmware/lib/EpdFont/scripts/fontconvert.py"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", required=True, type=Path, help="Noto Sans SC static TTF used as the build-time source")
    parser.add_argument("--charset", type=Path, default=DEFAULT_CHARSET)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--pixel-size", type=int, default=14,
                        help="compact bitmap height; 14px preserves the measured 220-270KB budget")
    parser.add_argument("--name", default="m4_compact_cjk_16")
    args = parser.parse_args()

    font = args.font.expanduser().resolve()
    charset = args.charset.expanduser().resolve()
    output = args.output.expanduser()
    if not font.is_file():
        parser.error(f"font not found: {font}")
    if not charset.is_file():
        parser.error(f"charset not found: {charset}")
    if args.pixel_size <= 0 or args.pixel_size > 255:
        parser.error("--pixel-size must be between 1 and 255")

    codepoints = {char for char in charset.read_text(encoding="utf-8") if ord(char) >= 0x20}
    if len(codepoints) < 3500:
        parser.error(f"charset has only {len(codepoints)} printable code points; expected at least 3500")
    required_ascii = {chr(cp) for cp in range(0x20, 0x7F)}
    missing_ascii = required_ascii - codepoints
    if missing_ascii:
        parser.error("charset must list all printable ASCII code points")

    output.parent.mkdir(parents=True, exist_ok=True)
    command = [
        sys.executable,
        str(FONTCONVERT),
        args.name,
        "16",  # stable logical M4 fallback name; --pixel-size controls the compact raster
        str(font),
        "--2bit",
        "--pixel-size",
        str(args.pixel_size),
        "--interval-merge-gap",
        "0",
        "--codepoints-file",
        str(charset),
    ]
    print("$ " + " ".join(command), file=sys.stderr)
    rendered = subprocess.run(command, check=True, stdout=subprocess.PIPE, text=True).stdout
    # Keep checked-in generated source deterministic across worktrees and
    # machines. The source font path is deliberately not embedded in the
    # header; NOTICE.txt and this script document the external input.
    rendered = re.sub(
        r" \* Command used:.*\n",
        " * raster pixel size: " + str(args.pixel_size) + "\n"
        " * charset: firmware/src/fontdata/m4_compact_cjk_charset.txt\n"
        " * regenerate: python3 firmware/scripts/generate_m4_compact_cjk.py --font <path-to-NotoSansSC-Medium.ttf>\n",
        rendered,
        count=1,
    )
    output.write_text(rendered, encoding="utf-8", newline="\n")
    print(f"generated {output} from {len(codepoints)} charset code points", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
