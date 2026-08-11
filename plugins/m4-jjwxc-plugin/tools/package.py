"""Build .m4x packages and content hashes (stdlib only)."""

from __future__ import annotations

import hashlib
import json
import zipfile
from pathlib import Path
from typing import Optional


def content_hash_dir(src: Path) -> str:
    """Stable hash of package source directory (paths + file bytes)."""
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
    """Zip a manifest allowlist into .m4x (stored or deflated).

    Release documentation and tooling must never accidentally enter an
    installable plugin.  When ``manifest.files`` is present it is the only
    payload allowlist; manifests without that field retain the old all-files
    behavior for small examples.
    """
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
    # Prefer STORED for large / binary entries: device ZipFile DEFLATE path
    # mallocs compressed+inflated at once; main.lua (~127KiB) + gbk_table (~48KiB)
    # used to fail extract with "missing:gbk_table.bin" under low free heap.
    with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for rel in sorted(set(rels), key=lambda p: p.as_posix()):
            path = src / rel
            size = path.stat().st_size if path.is_file() else 0
            use_store = (
                rel.suffix.lower() in {".bin", ".png", ".jpg", ".jpeg", ".gif"}
                or size >= 48 * 1024
            )
            comp = zipfile.ZIP_STORED if use_store else zipfile.ZIP_DEFLATED
            zf.write(path, rel.as_posix(), compress_type=comp)
    return out


def resolve_package(path: Path, cache_dir: Path) -> tuple[Path, str, Optional[dict]]:
    """
    Return (m4x_path, content_hash, manifest_or_None).
    If path is a directory, package into cache_dir.
    """
    path = path.resolve()
    cache_dir.mkdir(parents=True, exist_ok=True)
    if path.is_dir():
        ch = content_hash_dir(path)
        mf = read_manifest(path)
        name = f"{mf.get('id', 'pkg').replace('.', '_')}-{ch[:12]}.m4x"
        out = cache_dir / name
        side = cache_dir / f"{name}.hash"
        if not out.is_file() or not side.is_file() or side.read_text().strip() != ch:
            build_m4x(path, out)
            side.write_text(ch + "\n", encoding="utf-8")
        return out, ch, mf
    if path.is_file() and path.suffix.lower() == ".m4x":
        return path, content_hash_file(path), None
    raise FileNotFoundError(f"not a .m4x or source dir: {path}")


def validate_inbox_filename(name: str) -> Optional[str]:
    """Return error key or None if OK."""
    if not name or len(name) < 5 or len(name) > 64:
        return "bad_name"
    if "/" in name or "\\" in name or ".." in name:
        return "path_traversal"
    if not name.lower().endswith(".m4x"):
        return "bad_ext"
    for c in name:
        if not (c.isalnum() or c in "._-"):
            return "bad_name"
    return None
