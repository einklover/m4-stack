#!/usr/bin/env python3
"""Extract user-visible UTF-8 characters from shipped source for M4 UI fonts.

Scans:
  - src/I18n.h STR_TABLE strings
  - Hardcoded UTF-8 string literals in src/**/*.cpp, src/**/*.h (excluding generated)
  - Plus required ASCII printable

Outputs a sorted UTF-8 charset file and exits non-zero if optional coverage
font headers are given and miss any code point.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"

# C-string literals: "..." with escapes (skip raw u8 if rare)
STR_LIT = re.compile(r'(?<!R)"((?:\\.|[^"\\])*)"')
STR_MACRO = re.compile(
    r'STR\(\s*\w+\s*,\s*"((?:\\.|[^"\\])*)"\s*,\s*"((?:\\.|[^"\\])*)"'
)

SKIP_PARTS = {
    "network/html",
    ".generated.h",
    "fontIds.h",
}


def decode_c_string(s: str) -> str:
    try:
        return bytes(s, "utf-8").decode("unicode_escape").encode("latin1").decode("utf-8")
    except Exception:
        try:
            return s.encode("utf-8").decode("unicode_escape")
        except Exception:
            return s


def should_scan(path: Path) -> bool:
    rel = path.relative_to(ROOT).as_posix()
    if any(p in rel for p in SKIP_PARTS):
        return False
    if path.suffix not in {".cpp", ".h", ".hpp"}:
        return False
    return True


def collect_chars() -> set[str]:
    chars: set[str] = set()
    for i in range(0x20, 0x7F):
        chars.add(chr(i))

    i18n = SRC / "I18n.h"
    if i18n.exists():
        text = i18n.read_text(encoding="utf-8", errors="replace")
        for a, b in STR_MACRO.findall(text):
            for s in (a, b):
                for ch in s:
                    if ord(ch) >= 0x20:
                        chars.add(ch)

    for path in SRC.rglob("*"):
        if not path.is_file() or not should_scan(path):
            continue
        if path.name == "I18n.h":
            continue  # already handled
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for m in STR_LIT.finditer(text):
            raw = m.group(1)
            # Skip pure ASCII paths / formats that are not UI copy when no non-ASCII
            if not any(ord(c) >= 0x80 for c in raw):
                continue
            # Skip Serial-only format strings (not on-screen UI).
            preamble = text[max(0, m.start() - 100) : m.start()]
            if "Serial." in preamble or "printf(" in preamble:
                continue
            for ch in raw:
                o = ord(ch)
                # Latin/punct through CJK — skip emoji/dingbats not present in UI fonts.
                # Bidirectional formatting controls can appear in source
                # comments/strings but are invisible and trigger compiler
                # warnings when emitted as glyph comments. They are never
                # meaningful UI characters.
                if o in {0x200B, 0x200C, 0x200D, 0x200E, 0x200F,
                         0x202A, 0x202B, 0x202C, 0x202D, 0x202E,
                         0x2066, 0x2067, 0x2068, 0x2069}:
                    continue
                if o >= 0x20 and o < 0x2600:
                    chars.add(ch)
                elif 0x3000 <= o <= 0x30FF or 0x4E00 <= o <= 0x9FFF or 0xFF00 <= o <= 0xFFEF:
                    chars.add(ch)
    # Known UI extras that appear as concatenated Chinese fragments
    # Host-rendered plugin chrome is not part of the C++ source tree. Keep the
    # built-in fallback useful before an external CJK face is loaded; dynamic
    # book titles still prefer the reader font through hasTextGlyphs().
    extras = (
        "主页库风眼含设为未找到"
        "微信读书番茄小说章节目录书架点击打开分类查找书籍"
        "上一页下一页暂无缓存加载中返回登录失败网络"
        "微信讀書番茄小說章節目錄書架點擊打開分類查找書籍"
        "上一頁下一頁暫無緩存載入中返回登入失敗網絡"
    )
    chars.update(extras)
    return chars


def parse_font_codepoints(header: Path) -> set[int]:
    """Parse EpdUnicodeInterval entries from a generated font header."""
    text = header.read_text(encoding="utf-8", errors="replace")
    cps: set[int] = set()
    # { 0xXXXX, 0xYYYY, ... }
    for a, b in re.findall(r"\{\s*0x([0-9A-Fa-f]+)\s*,\s*0x([0-9A-Fa-f]+)", text):
        lo, hi = int(a, 16), int(b, 16)
        for cp in range(lo, hi + 1):
            cps.add(cp)
    return cps


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--out",
        default=str(ROOT / "lib/EpdFont/builtinFonts/source/m4_ui_charset.txt"),
    )
    ap.add_argument(
        "--check-font",
        action="append",
        default=[],
        help="Font header path that must cover the charset (repeatable)",
    )
    ap.add_argument("--print-missing", action="store_true")
    args = ap.parse_args()

    chars = collect_chars()
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    ordered = "".join(sorted(chars, key=ord))
    out.write_text(ordered, encoding="utf-8")
    print(f"charset: {len(chars)} unique code points -> {out}")

    # Spot-check known review strings
    required_samples = ["设为主页", "我的书库", "风眼", "未找到含"]
    missing_samples = []
    for s in required_samples:
        bad = [c for c in s if c not in chars]
        if bad:
            missing_samples.append((s, bad))
    if missing_samples:
        print("ERROR: sample UI strings not fully present in charset:", missing_samples)
        return 2
    print("sample UI strings present in charset: OK")

    rc = 0
    for font_path in args.check_font:
        fp = Path(font_path)
        if not fp.exists():
            print(f"ERROR: font header missing: {fp}")
            rc = 1
            continue
        covered = parse_font_codepoints(fp)
        miss = sorted({ord(c) for c in chars if ord(c) not in covered})
        if miss:
            rc = 1
            print(f"ERROR: {fp.name} missing {len(miss)} code points")
            if args.print_missing:
                print("".join(chr(c) for c in miss if c >= 0x80))
        else:
            print(f"OK: {fp.name} covers all {len(chars)} charset code points")
    return rc


if __name__ == "__main__":
    sys.exit(main())
