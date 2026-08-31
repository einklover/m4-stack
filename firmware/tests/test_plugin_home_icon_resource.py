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

def test_icon_is_not_all_white_or_all_empty_placeholder():
    # Additional Round 2 lock: icons must not be degenerate all-white / all-black placeholders.
    # This is a soft visual sanity check without requiring pixel-perfect mockup match.
    # Each 62x64 1-bit BMP has 8*64=512 bytes of pixel data; check it contains both ink and paper,
    # and black ink ratio is in a plausible window (not 0% nor >95% ink for these line icons).
    for rel, _ in PLUGINS:
        p = ROOT / rel / "icon_home.bmp"
        data = p.read_bytes()
        off = struct.unpack_from("<I", data, 10)[0]
        pix = data[off:]
        assert len(pix) == 512, f"{rel} pixel bytes {len(pix)} !=512 (62x64 stride 8)"
        # Palette is 0=black (00000000), 1=white (ffffffff) per current toolchain; bit=0 is black, 1 is white.
        assert not all(b == 0xFF for b in pix), f"{rel} icon is all-white (all 0xFF) placeholder — must contain ink"
        assert not all(b == 0x00 for b in pix), f"{rel} icon is all-black (all 0x00) placeholder — must contain paper"
        # Mixed content: need at least a few bytes that are neither all 0 nor all FF, proving dither/detail exists
        mixed = sum(1 for b in pix if b not in (0x00, 0xFF))
        # Icons are mostly white background, so mixed may be small but must be non-zero; line icons have at least 10 mixed bytes
        assert mixed >= 3, f"{rel} icon has only {mixed} mixed bytes (need >=3) — likely placeholder"
        # Black pixel ratio: count bits that are 0 (black) vs total 3968. Allow 1%–80% black.
        total_bits = 62 * 64
        white_bits = sum(bin(b).count("1") for b in pix)
        black_bits = total_bits - white_bits
        ratio = black_bits / total_bits
        assert 0.01 <= ratio <= 0.80, f"{rel} icon black ratio {ratio:.3f} out of [0.01,0.80] — degenerate"
        # Also ensure not all identical nibble patterns (e.g. checkerboard placeholder would be 0xAA/0x55 only)
        distinct = len(set(pix))
        assert distinct >= 3, f"{rel} icon has only {distinct} distinct byte values — likely synthetic placeholder"

