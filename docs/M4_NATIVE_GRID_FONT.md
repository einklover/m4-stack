# Murphy M4 native-grid 15x16 system font

Offline system UI and the default/fallback reader face on `murphy_m4`
(`OMIT_FONTS=1`) is the uncompressed 15x16 1-bit native-grid corpus. This is
not Luna block/radical/convolution compression.

## Call-site split

| Font IDs | Face | Why |
|----------|------|-----|
| `SMALL` / `UI_10` / `UI_12` | native-grid 15x16 1-bit at native metrics | Home, menus, dialogs, settings, chapter lists, plugin UI, status bars. Never a reader TTF, never `ScaledEpdFont`, never `readerPixelSize`. |
| `NOTOSANS_12/14/16/18` default | same native-grid instance | default/fallback reader body when no SD epdfont / runtime TTF is selected |
| `NOTOSANS_16` after `bindSystemReader` | `ScaledEpdFont` over native-grid or canonical SD epdfont | reader pixel size only; chrome IDs are not replaced |
| `/fonts/NotoSansCJKsc.epdfont` | canonical SD epdfont | still promoted onto NOTOSANS when present |
| `/FONT` runtime sfnt | streamed TTF/OTF/CFF | reader hash IDs only; `M4FixedRuntimeUiFonts` restores chrome builtins |

`bindSystemReader` always divides by 16 (`M4FontPolicy::systemReaderSourcePx()`).
The legacy compact 2-bit CJK face (`m4_compact_cjk_16`, 14px raster) has been
removed. UI glyph metrics, line height, and layout must not change when the
user switches a reader TTF or reader font size.

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

## UI metrics vs the removed compact 2-bit face

Chrome previously used a 14px 2-bit Noto raster (`advanceY=20`, CJK advance
~12–14). Native-grid chrome is 15×16 1-bit (`advanceY=16`, CJK advance 15).
Layouts that query `getLineHeight` / `getTextWidth` pick this up automatically.
Hardcoded offsets (`rect.y + 6`, `itemY + 4`) still need a real-device
spot-check: tighter line pitch, slightly wider CJK wrap/truncate. No chrome
`ScaledEpdFont` wrapper was added; 16px glyphs in typical ~40px rows should
not overlap, but that is not proven in QEMU.

## Production firmware size

Do not commit `.pio` / `firmware.bin`. Numbers below are from `pio run -e
murphy_m4` after chrome IDs moved to native-grid and compact 2-bit CJK was
deleted.

| Field | Value |
|-------|-------|
| `firmware.bin` | 5,456,256 bytes |
| SHA-256 | `ad061883509fb84140db5ec12031a421ff792e4d28727cc9c76d7ace7c3b2ee7` |
| Linked Flash | 5,455,749 / 7,143,424 (76.4%) |
| Linked RAM | 96,700 / 327,680 (29.5%) |
| vs pre-unification (`be53d07`, native-grid reader + compact UI) | 5,715,648 → 5,456,256 (**−259,392 B recovered**) |
| 85% APP1 remaining (`check_m4_build_budget.py`) | 615,654 B (`failures: []`) |
| APP1 remaining | 1,687,168 B |

QEMU plugin-debug (`murphy_m4_qemu_plugin`) linked Flash 5,464,653 / 7,143,424
(76.5%), RAM 95,996. Host: `test_ui_chrome_font_isolation.py` 7 tests OK,
`test_native_grid_font.py` 5 tests OK, CTest `m4_native_grid_font_tests` and
`m4_scaled_epd_font_tests` PASS. m4sim generic smoke PASS (Home / `sd_ok`);
reader-ui journey PASS (`Reader overlays + Catalog/More full pages + Bookmark
manager`).
