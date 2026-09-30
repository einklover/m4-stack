#!/usr/bin/env python3
"""Insert M4SdVolumeGuard into SdFat functions that use the shared cache.

Idempotent. Edits the PlatformIO libdeps copies in place. The tracked source
of the fix is this script plus include/M4SdVolumeGuard.h. Debug dump files are
left unchanged: they print while walking the cache and are not on the product
read/write path.
"""
from __future__ import annotations

import sys
from pathlib import Path

FILES = (
    "src/FatLib/FatFile.cpp",
    "src/FatLib/FatPartition.cpp",
    "src/FatLib/FatFileLFN.cpp",
    "src/FatLib/FatFileSFN.cpp",
    "src/FatLib/FatName.cpp",
    "src/ExFatLib/ExFatFile.cpp",
    "src/ExFatLib/ExFatFileWrite.cpp",
    "src/ExFatLib/ExFatPartition.cpp",
    "src/ExFatLib/ExFatName.cpp",
)
# Direct cache helpers, plus consumers that keep the returned pointer
# (cacheDir / readDirCache / dirCache / cacheAddress) until the last use.
TOKENS = (
    "dataCachePrepare",
    "fatCachePrepare",
    "bitmapCachePrepare",
    "cacheSync",
    "cacheSafeRead",
    "cacheSafeWrite",
    "cacheDir(",
    "readDirCache(",
    "dirCache(",
    "cacheDirEntry(",
    "cacheAddress(",
)
# Whole-function guards. print_t functions are excluded so a Print callback
# is not taken under the volume lock; date printers copy integers first.
REQUIRED = (
    "bool FatFile::openNext(",
    "bool FatFile::open(FatFile* dirFile, FatLfn_t*",
    "bool FatFile::openCachedEntry(",
    "bool FatFile::cmpName(",
    "bool FatFile::createLFN(",
    "size_t FatFile::getName7(",
    "size_t FatFile::getName8(",
    "size_t FatFile::getSFN(",
    "bool FatFile::dirEntry(",
    "bool FatFile::getAccessDate(",
    "bool FatFile::getCreateDateTime(",
    "bool FatFile::getModifyDateTime(",
    "bool ExFatFile::openPrivate(",
    "size_t ExFatFile::getName7(",
    "size_t ExFatFile::getName8(",
    "bool ExFatFile::getAccessDateTime(",
    "bool ExFatFile::getCreateDateTime(",
    "bool ExFatFile::getModifyDateTime(",
)
INCLUDE = '#include "M4SdVolumeGuard.h"\n'
GUARD = "  M4SdVolumeGuard m4SdVolumeGuard_;\n"
MARKER = "M4SdVolumeGuard m4SdVolumeGuard_;"


def _match_paren(text: str, open_at: int) -> int:
    depth = 0
    i = open_at
    while i < len(text):
        c = text[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def _match_brace(text: str, open_at: int) -> int:
    depth = 0
    i = open_at
    while i < len(text):
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def _functions(text: str):
    i = 0
    n = len(text)
    while i < n:
        if i == 0 or text[i - 1] == "\n":
            if text[i] not in " \t#/{}":
                paren = text.find("(", i)
                line_end = text.find("\n", i)
                if paren != -1 and (line_end == -1 or paren < line_end or True):
                    # Signature may span lines. Reject statements that end in ';'.
                    close = _match_paren(text, paren)
                    if close != -1:
                        semi = text.find(";", i)
                        if semi == -1 or semi > close:
                            j = close + 1
                            while j < n and text[j] in " \t\r\n":
                                j += 1
                            if j < n and text.startswith("const", j):
                                j += 5
                                while j < n and text[j] in " \t\r\n":
                                    j += 1
                            if j < n and text[j] == "{":
                                end = _match_brace(text, j)
                                if end != -1 and text[i:paren].strip() and "::" in text[i:paren]:
                                    yield i, j, end
                                    i = end + 1
                                    continue
        i += 1


def _wants_guard(sig: str, body: str) -> bool:
    # print_t stays outside the volume lock. Date printers copy integers
    # through the guarded getters, then print.
    if "print_t*" in sig:
        return False
    if any(req in sig for req in REQUIRED):
        return True
    return any(tok in body for tok in TOKENS)


def patch_text(text: str) -> str:
    inserts = []
    for start, brace, end in _functions(text):
        body = text[brace : end + 1]
        if not _wants_guard(text[start:brace], body):
            continue
        if MARKER in body[:200]:
            continue
        inserts.append(brace + 1)
    if not inserts:
        if INCLUDE.strip() in text:
            return text
        # No cache functions and no include needed.
        return text
    if INCLUDE.strip() not in text:
        anchor = text.find("\n")
        # Place include after the first include block line that mentions Fat or ExFat header,
        # else after the last leading comment's first include.
        inc = text.rfind('#include "', 0, inserts[0] if inserts else len(text))
        if inc == -1:
            text = INCLUDE + text
            inserts = [p + len(INCLUDE) for p in inserts]
        else:
            line_end = text.find("\n", inc)
            insert_at = line_end + 1
            text = text[:insert_at] + INCLUDE + text[insert_at:]
            inserts = [p + len(INCLUDE) if p >= insert_at else p for p in inserts]
    # Recompute because include insertion shifted offsets when done above.
    # Do a second pass on the updated text instead of trusting shifted indexes
    # when include was added. Easiest: if we added include, rescan.
    return _insert_guards(text)


def _insert_guards(text: str) -> str:
    pieces = []
    cursor = 0
    count = 0
    for start, brace, end in _functions(text):
        body = text[brace : end + 1]
        if not _wants_guard(text[start:brace], body):
            continue
        if MARKER in body[:180]:
            continue
        pieces.append(text[cursor : brace + 1])
        pieces.append("\n" + GUARD)
        cursor = brace + 1
        count += 1
    pieces.append(text[cursor:])
    out = "".join(pieces)
    if count and INCLUDE.strip() not in out:
        raise RuntimeError("guard inserted without include")
    return out


def patch_file(path: Path) -> int:
    original = path.read_text()
    if INCLUDE.strip() not in original:
        inc = original.rfind('#include "')
        # temporary: let _insert figure include first
        text = original
        first_cache = min((text.find(tok) for tok in TOKENS if tok in text), default=-1)
        if first_cache == -1:
            return 0
        inc_at = text.rfind('#include "', 0, first_cache)
        if inc_at == -1:
            text = INCLUDE + text
        else:
            line_end = text.find("\n", inc_at)
            text = text[: line_end + 1] + INCLUDE + text[line_end + 1 :]
        updated = _insert_guards(text)
    else:
        updated = _insert_guards(original)
    if updated == original:
        return 0
    path.write_text(updated)
    return updated.count(MARKER) - original.count(MARKER)


def _guarded(text: str, signature: str) -> bool:
    for start, brace, end in _functions(text):
        sig = text[start:brace]
        if signature not in sig:
            continue
        body = text[brace : end + 1]
        return MARKER in body[:200]
    return False


def assert_volume_guard(root: Path) -> None:
    props = (root / "library.properties").read_text()
    version = ""
    for line in props.splitlines():
        if line.startswith("version="):
            version = line.split("=", 1)[1].strip()
    # ^2.3.1 may resolve to a later 2.3 patch. The function list is the 2.3 API.
    # A different major.minor is a hard failure once the dependency is installed.
    # Absence of libdeps stays a soft skip in patch_tree (install order).
    if not version.startswith("2.3."):
        raise SystemExit(f"SdFat {version or 'unknown'} at {root} is outside the 2.3 guard list")
    blob = ""
    for rel in FILES:
        blob += (root / rel).read_text() + "\n"
    missing = [sig for sig in REQUIRED if not _guarded(blob, sig)]
    if missing:
        raise SystemExit("volume guard missing:\n  " + "\n  ".join(missing))
    for name in ("FatFile::printName7(", "FatFile::printName8(", "ExFatFile::printName7(", "ExFatFile::printName8("):
        if name in blob and _guarded(blob, name):
            raise SystemExit(f"{name} holds the volume lock across Print")


def patch_tree(lib_root: Path) -> int:
    sdfat_roots = sorted(lib_root.glob("*/SdFat"))
    total = 0
    if not sdfat_roots:
        # PlatformIO installs lib_deps before extra scripts. An empty tree is
        # still not a hard error here: a caller may import the patcher before
        # the first install. A present tree with the wrong version fails closed.
        print(f"no SdFat libdeps under {lib_root}", file=sys.stderr)
        return 0
    for root in sdfat_roots:
        for rel in FILES:
            path = root / rel
            if not path.is_file():
                raise SystemExit(f"missing {path}")
            added = patch_file(path)
            total += added
            print(f"{path}: +{added}")
        assert_volume_guard(root)
    return total


def main() -> int:
    firmware = Path(__file__).resolve().parents[1]
    patch_tree(firmware / ".pio" / "libdeps")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
