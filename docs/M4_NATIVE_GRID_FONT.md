# Murphy M4 native-grid 15x16 reader face

Offline reader/content CJK on `murphy_m4` (`OMIT_FONTS=1`) is the uncompressed
15x16 1-bit native-grid corpus. UI chrome stays on the existing compact 2-bit
CJK face. This is not Luna block/radical/convolution compression.

## Call-site split

| Font IDs | Face | Why |
|----------|------|-----|
| `SMALL` / `UI_10` / `UI_12` | compact 2-bit Noto (`m4_compact_cjk_16`) | 14px 2-bit metrics; `M4FixedRuntimeUiFonts` and chrome layouts |
| `NOTOSANS_12/14/16/18` | native-grid 15x16 1-bit | full 28,953-glyph offline reader coverage |
| `/fonts/NotoSansCJKsc.epdfont` | canonical SD epdfont | still promoted onto NOTOSANS when present |
| `/FONT` runtime sfnt | streamed TTF/OTF/CFF | unchanged |

Replacing compact CJK on chrome IDs would change 14px 2-bit metrics/visuals.
`bindSystemReader` divides by 14 when `getData()->is2Bit`, else 16 (native-grid
or canonical epdfont).

Missing glyphs still return `nullptr` from `getGlyph`; `EpdFont` /
`GfxRenderer` fall back to `'?'`. Native-grid includes U+0020 and U+003F.

## Blob (`firmware/src/fontdata/m4_native_grid_15x16.bin`)

| Field | Value |
|-------|-------|
| Format | M4NG v1 little-endian |
| Size | 874,726 bytes |
| SHA-256 | `924b4bbd5f45d0208d17477c701df7d459a1966452f98989177358b3788a9960` |
| Strict glyphs | 28,953 × 15×16 1-bit = 30 B = 868,590 B |
| Outliers | 32 × 16×16 1-bit sidecar = 32 B bitmap + uint16 cp |
| Index | ranked two-level BMP bitset, 5,000 B (not 28,953 × 16 B `EpdGlyph`) |
| Occupied BMP pages | 132 |

Header (48 B) + `page_dir[256]` uint16 (512 B) + 132 × 34-byte leaves (4,488 B)
+ bitmaps + outlier codepoints + outlier bitmaps.

Lookup (O(1) BMP): page = `cp >> 8`; if `page_dir[page] != 0xFFFF`, test
occupancy bit (`LSB of byte 0` = first codepoint of the page) and
`rank = rank_base + popcount(prefix)`. Outliers: sorted uint16 binary search.
`NativeGridEpdFont` synthesizes a 4-entry RAM `EpdGlyph` scratch ring; ROM
bitmaps stay in the embedded blob.

## Reproducible generation

Source TTF is external (not vendored): 标准像素粗.ttf SHA-256
`9507b4d3e915455afadfa688e8ea515abf816bce06f76346ee356f0f38810574`.

```bash
# Requires fontTools (PlatformIO venv has it). Bit-identical to the audited corpus.
~/.platformio/penv/bin/python firmware/scripts/generate_m4_native_grid.py \
  --font /path/to/标准像素粗.ttf

# TTF-free identity check of the tracked blob/header/manifest (PIO pre-script).
~/.platformio/penv/bin/python firmware/scripts/generate_m4_native_grid.py --verify
```

`murphy_m4` / `murphy_m4_qemu_plugin` embed the blob via
`board_build.embed_files` and run `pre:scripts/verify_m4_native_grid.py`.

## Production firmware size

`pio run -e murphy_m4` SUCCESS (2026-08-24) + `firmware/scripts/check_m4_build_budget.py`
failures `[]`. APP1 = 7,143,424 B; 85% gate = 6,071,910 B. Do not commit `.pio` / `firmware.bin`.

| Measurement | Value |
|-------------|-------|
| `firmware.bin` bytes | 5,715,648 |
| `firmware.bin` SHA-256 | `1506ae7c8e4188689101ee5d4786b476c50f4e03adea32438f45a8d0249a8019` |
| Linked Flash | 5,715,141 / 7,143,424 (80.0%) |
| Linked RAM | 96,740 / 327,680 (29.5%; 35% gate = 114,688) |
| APP1 remaining | 1,427,776 |
| Remaining inside 85% | **356,262** (gate PASS) |
| Delta vs pre-integration `4,840,048` | +875,600 (blob 874,726 + ~874 B decoder/wiring) |

Host CTest `m4_native_grid_font_tests` / `m4_scaled_epd_font_tests` /
`m4_compact_cjk_font_tests` PASS. `./m4sim test smoke --plugin-debug
--skip-build --ready-seconds 90` PASS (Home, sd_ok, 170704 free heap). No
device flash / ADB.
