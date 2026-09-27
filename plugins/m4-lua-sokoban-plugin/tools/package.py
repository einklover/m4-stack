"""Build .m4x packages and content hashes (stdlib only)."""

from __future__ import annotations

import hashlib
import json
import zipfile
from pathlib import Path
from typing import Optional


def content_hash_dir(src: Path) -> str:
    h = hashlib.sha256()
    files = sorted(p for p in src.rglob("*") if p.is_file())
    for p in files:
        rel = p.relative_to(src).as_posix()
        h.update(rel.encode("utf-8"))
        h.update(b"\0")
        h.update(p.read_bytes())
        h.update(b"\0")
    return h.hexdigest()


def content_hash_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_manifest(src: Path) -> dict:
    mf = src / "manifest.json"
    if not mf.is_file():
        raise FileNotFoundError(f"manifest.json missing in {src}")
    return json.loads(mf.read_text(encoding="utf-8"))


def build_m4x(src: Path, out: Path) -> Path:
    src = src.resolve()
    if not (src / "manifest.json").is_file():
        raise FileNotFoundError("manifest.json required")
    manifest = read_manifest(src)
    declared = manifest.get("files")
    if isinstance(declared, list):
        entry = manifest.get("entry", "main.lua")
        rels = [Path("manifest.json"), Path(str(entry)), *(Path(str(name)) for name in declared)]
        for rel in rels:
            if rel.is_absolute() or ".." in rel.parts:
                raise ValueError(f"manifest path escapes source: {rel}")
            if not (src / rel).is_file():
                raise FileNotFoundError(f"manifest file missing: {rel}")
    else:
        rels = sorted(p.relative_to(src) for p in src.rglob("*") if p.is_file())
    out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for rel in sorted(set(rels), key=lambda p: p.as_posix()):
            zf.write(src / rel, rel.as_posix())
    return out


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    out = root / "dist" / "com.m4.sokoban.m4x"
    built = build_m4x(root, out)
    print(built)


if __name__ == "__main__":
    main()
