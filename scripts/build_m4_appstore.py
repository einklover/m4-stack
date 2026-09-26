#!/usr/bin/env python3
"""Build and validate every tracked M4 plugin for the public GitHub Pages store.

Usage:
  python3 scripts/build_m4_appstore.py /tmp/m4-appstore-build
  cp /tmp/m4-appstore-build/index.json docs/appstore/index.json
  # Publish the generated package files and index.json to gh-pages/appstore/.
"""
import argparse
import hashlib
import importlib.util
import json
import re
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PREFIX = "https://einklover.github.io/m4-stack/appstore/packages/"
SOURCE = "https://github.com/einklover/m4-stack/tree/feature/firstboot-home-appstore-20260926/plugins/"
MAX_PACKAGE = 2 * 1024 * 1024
MAX_UNPACKED = 2 * 1024 * 1024
ORDER = [
    "com.weread.client", "com.fanqie.client", "com.jjwxc.client",
    "com.legado.client", "com.m4.tarot", "com.m4.pet", "com.m4.game2048",
    "com.m4.tetris", "com.m4.minesweeper", "com.m4.sudoku", "com.m4.gomoku",
    "com.m4.huarong", "com.m4.hanoi", "com.m4.tictactoe",
    "com.m4screenbridge.client",
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("out_dir", type=Path)
    args = parser.parse_args()
    out = args.out_dir.resolve()
    packages = out / "packages"
    packages.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location(
        "m4_package_builder", ROOT / "plugins/m4-jjwxc-plugin/tools/package.py"
    )
    builder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(builder)
    manifests = sorted((ROOT / "plugins").glob("*/manifest.json"))
    if len(manifests) != 15:
        raise ValueError(f"Expected 15 tracked plugin manifests, found {len(manifests)}")
    catalog = {}
    for manifest in manifests:
        src = manifest.parent
        info = json.loads(manifest.read_text(encoding="utf-8"))
        app_id = info["id"]
        version = info["version"]
        if app_id in catalog or not re.fullmatch(r"[a-zA-Z0-9.]+", app_id):
            raise ValueError(f"Duplicate or invalid plugin ID: {app_id}")
        if not re.fullmatch(r"\d+\.\d+\.\d+", version):
            raise ValueError(f"Invalid version: {app_id}")
        filename = f"{app_id}-v{version}.m4x"
        dest = packages / filename
        builder.build_m4x(src, dest)  # Derives JJWXC gbk_table.bin without committing it.
        with zipfile.ZipFile(dest) as archive:
            names = archive.namelist()
            expected = {"manifest.json", info.get("entry", "main.lua")}
            expected.update(info.get("files", []))
            if info.get("icon"):
                expected.add(info["icon"])
            if len(names) != len(set(names)) or set(names) != expected:
                raise ValueError(f"Incorrect payload inventory: {app_id}")
            if len(names) > 160:
                raise ValueError(f"Too many files for firmware: {app_id}")
            unpacked = sum(z.file_size for z in archive.infolist())
            if unpacked > MAX_UNPACKED:
                raise ValueError(f"Too large unpacked: {app_id} ({unpacked})")
            for z in archive.infolist():
                cap = 16*1024 if z.filename == "manifest.json" else (
                    256*1024 if z.filename == info.get("entry", "main.lua") else 512*1024
                )
                if z.file_size > cap:
                    raise ValueError(f"File exceeds installer cap: {app_id}/{z.filename}")
            embedded = json.loads(archive.read("manifest.json"))
            if embedded["id"] != app_id or embedded["versionCode"] != info["versionCode"]:
                raise ValueError(f"Package manifest mismatch: {app_id}")
        if dest.stat().st_size > MAX_PACKAGE:
            raise ValueError(f"Download exceeds firmware cap: {app_id}")
        description = re.sub(r"\s+", " ", info.get("description", "")).strip()
        while len(description.encode("utf-8")) > 220:
            description = description[:-1]
        category = ("reading" if info.get("runtime") == "native" and app_id != "com.m4screenbridge.client"
                    else "tools" if app_id == "com.m4screenbridge.client"
                    else "lifestyle" if app_id == "com.m4.tarot" else "games")
        catalog[app_id] = {
            "id": app_id,
            "name": info["name"].replace(" ", "").strip(),
            "version": version,
            "versionCode": info["versionCode"],
            "description": description,
            "category": category,
            "packageUrl": PREFIX + filename,
            "sha256": hashlib.sha256(dest.read_bytes()).hexdigest(),
            "sourceUrl": SOURCE + src.name,
        }
        print(f"{app_id:28} v{version:8} code={info['versionCode']:3} "
              f"files={len(names):3} zip={dest.stat().st_size:7} raw={unpacked:7}")
    if set(catalog) != set(ORDER):
        raise ValueError("Missing or unexpected catalog entry")
    output = {"schemaVersion": 1, "apps": [catalog[app_id] for app_id in ORDER]}
    (out / "index.json").write_text(
        json.dumps(output, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(f"CATALOG_OK {len(catalog)} apps {sum(f.stat().st_size for f in packages.glob('*.m4x'))} package bytes")


if __name__ == "__main__":
    main()
