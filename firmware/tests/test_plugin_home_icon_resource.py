import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PLUGINS = [
    ("plugins/m4-weread-plugin", "com.weread.client"),
    ("plugins/m4-fanqie-plugin", "com.fanqie.client"),
    ("plugins/m4-jjwxc-plugin", "com.jjwxc.client"),
]

def test_manifest_declares_icon_and_file_exists():
    for rel, expected_id in PLUGINS:
        src = ROOT / rel
        mf = json.loads((src / "manifest.json").read_text(encoding="utf-8"))
        assert mf["id"] == expected_id
        assert "icon" in mf, f"{rel} must declare icon"
        assert mf["icon"] == "icon_home.bmp", f"{rel} icon must be icon_home.bmp, got {mf['icon']}"
        icon_path = src / mf["icon"]
        assert icon_path.is_file(), f"{rel} icon file missing {icon_path}"

def test_icon_is_62x64_1bit_bmp():
    for rel, _ in PLUGINS:
        p = ROOT / rel / "icon_home.bmp"
        # Check BMP header 1-bit, 62x64
        data = p.read_bytes()
        assert data[0:2] == b"BM", f"{rel} not BMP"
        w = struct.unpack_from("<i", data, 18)[0]
        h = struct.unpack_from("<i", data, 22)[0]
        bpp = struct.unpack_from("<H", data, 28)[0]
        # Height may be negative for top-down; use abs
        assert abs(w) == 62 and abs(h) == 64, f"{rel} BMP {w}x{h} !=62x64"
        assert bpp == 1, f"{rel} BMP bpp {bpp} !=1"

def test_package_includes_icon():
    # Use cheapest stable check: manifest.files must contain icon and build_m4x would include it
    for rel, _ in PLUGINS:
        src = ROOT / rel
        mf = json.loads((src / "manifest.json").read_text(encoding="utf-8"))
        files = mf.get("files", [])
        assert "icon_home.bmp" in files, f"{rel} files must include icon_home.bmp (existing packaging convention: manifest.files is allowlist)"
        # Also verify package.py build would succeed (file exists)
        assert (src / "icon_home.bmp").is_file()
