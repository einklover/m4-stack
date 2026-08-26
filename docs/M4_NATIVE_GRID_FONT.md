# Murphy M4 native-grid system font (15×16 ROM, 16×16 logical cell)

Offline system UI and the default/fallback reader face on `murphy_m4`
(`OMIT_FONTS=1`) is the uncompressed 15×16 1-bit native-grid corpus. This is
not Luna block/radical/convolution compression.

The **logical** system pixel cell is **16×16**. The stored corpus stays 15×16
(32 outliers already 16×16). Ordinary full-width CJK is 15×16 ink plus one
right-side logical blank column of spacing. That extra column is a
metric/render concept; it is **not** duplicated into flash.

## Integer-N scaling only (native 1-bit path)

Built-in 1-bit native-grid may scale **only** by integer Kronecker
replication N = 1, 2, 3, … :

```
dest(sx*N + ix, sy*N + iy) = src(sx, sy)    for ix, iy in 0..N-1
```

A 1px source stroke becomes exactly N dest pixels everywhere. The logical
16×16 cell becomes exactly 16N×16N (2x = 32×32, 3x = 48×48). Stored bitmap
size is 15N×16N; the 16th column is advance, not stored pixels.

The old `blitCoverage1Bit` / `coverage0` / `coverage1` arbitrary-ratio mapper
is **gone**. It produced 29×31-class rasters (e.g. 15×16 → ~29×31 at nominal
31px) with mixed 1/2/3 dest pixels per source pixel. Native-grid production
calls `ScaledEpdFont::bindInteger` only. Dest-sample `bind(source, scale,
false)` remains solely for canonical 2-bit / non-native bitmap faces.
Runtime TTF/OTF never uses this scaler.

## Call-site split

| Font IDs | Face | Why |
|----------|------|-----|
| `SMALL` | native-grid `bindInteger` **1x** (16px cell) | status / secondary chrome |
| `UI_10` / `UI_12` | native-grid `bindInteger` **2x** (32px cell) | major menu/list labels. Constraint: both roles are 2x because 1x shrinks UI_10 versus the old 22px and 3x (48) overflows Lyra `listRowHeight=40`. Native-app 96px cells cannot fit four UI_10 CJK (32×4=128); those tight cells must use SMALL (16×4=64) or wrap. |
| `NOTOSANS_12/14/16/18` default | native-grid at 1x (16-row metrics) | default/fallback reader body when no SD epdfont / runtime TTF is selected |
| `NOTOSANS_16` after `bindSystemReader` | `bindInteger` over native-grid **or** dest-sample over canonical SD epdfont | reader size only; chrome IDs are not replaced |
| `/fonts/NotoSansCJKsc.epdfont` | canonical SD epdfont | still promoted onto NOTOSANS when present; arbitrary px, no 16×16 snap |
| `/FONT` runtime sfnt | streamed TTF/OTF/CFF | reader hash IDs only at the exact numeric pixel size; `M4FixedRuntimeUiFonts` restores chrome builtins |

UI may still display the user's numeric size (18/22/31). Built-in pixel font
**snaps** to 16/32/48. TTF/OTF uses the exact requested pixel size.

## Reader size mapping (`M4FontPolicy::nativeGridIntegerScale`)

One place. Range is the existing 12–48 `readerPixelSize` clamp. Default
`readerPixelSize=18`. Diagnosed real-device setting 31.

| Requested px | N | Logical cell |
|--------------|---|--------------|
| 12–20 | 1x | 16 |
| 21–39 | 2x | 32 |
| 40–48 | 3x | 48 |

Do not apply this mapping to TTF/OTF.

The legacy compact 2-bit CJK face (`m4_compact_cjk_16`, 14px raster) has been
removed. Changing reader TTF or reader size must not change any system UI
metric or system UI font.

## Native-grid metric rules (15×16 ROM cell, 16×16 logical cell)

`width`/`height` stay 15×16 (or 16×16 outliers) so `pixelPosition=y*width+x`
still addresses the blob. `left` (xOffset) and `advanceX` are synthesized at
1x, then multiplied by exact N:

| Kind | Codepoints | `left` | `advanceX` at 1x |
|------|------------|--------|------------------|
| Space | U+0020, U+00A0 | 0 | 4 |
| Latin | ASCII 0x21–0x7E letters/digits/symmetric punct, Latin-1/Extended, U+2010–U+2027 except pairs | `1 - firstInk` | `1 + inkW + 1` |
| Pair open | `( [ { < ‘ “ 〈 《 「 『 【 〔 〖 （ ［ ｛ ＜` | `2 - firstInk` | `2 + inkW + 1` |
| Pair close | `) ] } > ’ ” 〉 》 」 』 】 〕 〗 ） ］ ｝ ＞` | `1 - firstInk` | `1 + inkW + 2` |
| CJK | everything else (ideographs + `，。、！？` etc.) | 0 | **16** (`kLogicalCellPx`) |

CJK always advances 16/32/48 at 1x/2x/3x, including 16-wide outliers (do not
add a second full gap). The 15px ink plus one logical blank column is the
basic inter-character gap. Latin/digits/ASCII punctuation stay proportional:
`A`/`i`/`M`/`1`/`,` are **not** all 16N.

Opening marks have larger left side bearing than right (sit toward following
text). Closing marks have larger right side bearing (sit toward preceding
text). Source rasters are left-packed in the 15-cell; equal centering is
forbidden — open `(` / `“` / `《` must not share placement with close `)` /
`”` / `》`. Straight ASCII `'` / `"` stay Latin (symmetric 1+ink+1) because
the same glyph is used for both sides. Visual ink origin is
`cursor + left + firstInk`. At N×, bitmap, `left`/`top`, and advance all
scale by exactly N. No activity-level screen-position hacks.

Host regressions walk `设置 阅读 ABC abc 123,.;:!?()[]` and
`“测试” ‘ABC’ （测试） 《书名》 【章节】` plus CJK with 1px H/V strokes
(`中`/`口`/`日`/`工`/`十`). Rendered 2x/3x glyphs must equal nearest-neighbor
replication of the stored 15×16 bitmap.

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
~12–14), then a rejected 18/22/26 arbitrary-ratio coverage blit. Current
chrome is the same 15×16 1-bit face at integer N: SMALL=16 / UI_10=32 /
UI_12=32 (`advanceY` matches those px). Layouts that query `getLineHeight` /
`getTextWidth` pick this up automatically. Fengyan `listRowHeight` 52 /
`headerHeight` 44 still fit UI_12 at 32px. Lyra `listRowHeight=40` is 8px
above a 32px glyph — tight, no layout rewrite. Hardcoded offsets (`rect.y +
6`, `itemY + 4`) still need a real-device spot-check after 2x chrome.

## Production firmware size

Do not commit `.pio` / `firmware.bin`. Numbers below are from `pio run -e
murphy_m4` after integer-N native-grid (logical 16×16 cell, Kronecker scale).

| Field | Value |
|-------|-------|
| `firmware.bin` | 5,457,872 bytes |
| SHA-256 | `b2699bbc03510cf5f392aba09384499d166a59161d7aac900464e82f0f40a4cc` |
| Linked Flash | 5,457,365 / 7,143,424 (76.4%) |
| Linked RAM | 96,964 / 327,680 (29.6%) |
| vs chrome-unification (`dc75082`) | 5,456,256 → 5,457,872 (**+1,616 B**) |
| vs pre-unification (`be53d07`, native-grid reader + compact UI) | 5,715,648 → 5,457,872 (**−257,776 B recovered**) |
| 85% APP1 remaining (`0.85 * 7,143,424 − bin`) | 614,038 B |
| APP1 remaining | 1,685,552 B |

QEMU plugin-debug (`murphy_m4_qemu_plugin`) linked Flash 5,466,289 / 7,143,424
(76.5%), RAM 96,260; `firmware.bin` 5,466,800 B. Host: `test_ui_chrome_font_isolation.py`
7 tests OK, `test_native_grid_font.py` 5 tests OK, `test_font_visual_metrics.py`
3 tests OK, CTest `m4_native_grid_font_tests`, `m4_scaled_epd_font_tests`,
`m4_runtime_ui_font_policy_tests` PASS. m4sim generic smoke PASS (Home /
`sd_ok`). TTF arbitrary-size rasterization is unchanged (no integer snap).
