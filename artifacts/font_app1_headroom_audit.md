# APP1 font-headroom audit (measurement only)

Date: 2026-08-24
Supervisor: `agent/m4-ox-supervisor-integration` @ `5ba8be2` (`bfbc7fe` + simulator host contracts)
Production env: `murphy_m4` (not `murphy_m4_qemu_plugin`)
Device flash: none. ADB: none.

This is a size/partition audit for the 15×16 1-bit glyph-corpus decision. Luna compression experiments were **not** integrated. Production font representation was **not** changed.

## Method and limitations

- Local PlatformIO SDK/toolchain **was available**. Production firmware was compiled in this worktree:
  `pio run -e murphy_m4` SUCCESS in 82.5 s.
- Image numbers below are this branch's `firmware/.pio/build/murphy_m4/firmware.bin` plus PlatformIO RAM/Flash lines. The QEMU-plugin 4.85 MiB bin is a different partition profile and is not used here.
- The 28,953-glyph 15×16 corpus was **not regenerated** on this branch (source TTF is an external input). Byte counts come from the already-generated experiment artifacts on `m4-integrated-candidate` (`artifacts/standard_pixel_native_compression_report.json`, `standard_pixel_font_native_sizing.json`). Those files were read only; that worktree was not modified.
- Corpus identity: `标准像素粗.ttf` SHA-256 `9507b4d3e915455afadfa688e8ea515abf816bce06f76346ee356f0f38810574`; supported-corpus SHA-256 `f953cc612ae2fc412bda55b4aae8a105b8d6eb105ae3c06269b84c976c2cc1b3`.

## Partition capacity (this firmware)

Factory 16 MiB table `firmware/partitions_murphy_m4.csv` (`m4_base` / `murphy_m4`):

| Region | Offset | Size (bytes) | Role |
|--------|--------|--------------|------|
| Flash chip | 0 | 16,777,216 | ESP32-S3 N16R8 |
| otadata | 0xe000 | 8,192 | OTA slot select |
| app0 `ota_0` | 0x10000 | **7,143,424** | factory / rollback |
| app1 `ota_1` | 0x6e0000 | **7,143,424** | production APP1 (flash target) |
| spiffs | 0xdb0000 | 2,097,152 | not usable for the app image |
| coredump | 0xFF0000 | 65,536 | crash dump |

Applicable firmware capacity is **one OTA app slot: 7,143,424 bytes** (`0x6d0000`). Dual-OTA requires the new image to fit entirely in the inactive slot. SPIFFS cannot absorb glyph tables.

Existing release gate (`firmware/scripts/check_m4_build_budget.py`): firmware ≤ **85% of APP1 = 6,071,910 bytes**; static RAM ≤ 35% of 327,680.

## Current production image (this branch)

| Measurement | Bytes | Notes |
|-------------|------:|-------|
| `firmware.bin` (OTA payload) | 4,840,048 | SHA-256 `e7f32092a72d7fe3bb09e1b1067dba29f2bc4b1b514495e9b24a3a1f4583970a` |
| Linker “total image size” | 4,839,885 | `.bin` padded +163 B |
| Linked Flash used | 4,839,537 | PIO: 67.7% of 7,143,424 |
| Linked RAM used | 96,724 | PIO: 29.5% of 327,680; font tables are `.rodata` |

| Headroom vs | Bytes | % used |
|-------------|------:|-------:|
| APP1 slot 7,143,424 | **2,303,376** | 67.76% |
| 85% release budget 6,071,910 | **1,231,862** | 79.71% of budget |
| 15% official slot reserve | 1,071,514 | reserved by policy, not by hardware |

Budget script: **no failures**.

Already-resident compact 2-bit CJK (map-confirmed, stays unless product replaces it):

| Symbol | Size |
|--------|------:|
| `m4_compact_cjk_16` header | 32 |
| Intervals 2,519 × 12 | 30,228 |
| Glyphs 3,625 × 16 | 58,000 |
| Bitmaps | 170,882 |
| **Static total** | **259,142** |

## Generated 15×16 1-bit corpus

| Item | Count / bytes |
|------|----------------:|
| Mapped cmap | 28,985 |
| Strict 15×16 glyphs | **28,953** |
| Outliers (ink in x=15) | 32 |
| Unreconstructable | 0 |
| Packed bitmap | 15×16×1-bit = **30 B/glyph** |
| **Raw bitmaps** | **868,590** |

Index/metadata needed in production (native-grid face, **not** EpdFont per-glyph records):

| Piece | Bytes | Why |
|-------|------:|-----|
| 16-byte common header | 16 | sizing experiment `common_header_metadata_bytes` |
| Sorted uint16 BMP lookup | 57,906 | compression-report baseline |
| Two-level ranked bitset (132 BMP pages) | 5,000 | smaller production index; O(1) page + popcount |
| 6-byte run table (190 runs) | 1,140 | smallest index; O(log R) |
| 32 outlier 16×16 1-bit sidecars | 1,024 | 32 × 32 |
| Outlier uint16 index | 64 | |
| Uncompressed decoder glue | ~2,048 | estimate; memcpy 30 B |
| 5×8 decoder glue | ~4,096 | estimate; 6 × 5 B lookups |

Do **not** dump this corpus through current `EpdFont` / `EpdGlyph` (16 B/glyph + 12 B/interval): 868,590 + 463,248 + 2,280 + 32 = **1,334,150 B**. That representation fails the 85% gate.

### Uncompressed production envelopes

| Envelope | Bytes |
|----------|------:|
| Experiment raw + sorted lookup (no header) | 926,496 |
| + 16 B header | 926,512 |
| Inclusive sorted (header + outliers) | 927,600 |
| **Production sorted + decoder** | **929,648** |
| Ranked bitset + header + align4 | 873,608 |
| Inclusive ranked (header + outliers) | 874,696 |
| **Production ranked + decoder** | **876,744** |
| Run table + header + outliers + decoder | 872,884 |

### Fixed-block 5×8 (best lossless tiling in the experiment)

Measured `shared-block-5x8` `total_rom_bytes` = **728,652**:

| Piece | Bytes |
|-------|------:|
| 64,662 unique 5-byte blocks | 323,310 |
| 12 B/glyph block indices | 347,436 |
| Sorted uint16 lookup | 57,906 |
| **Total (report)** | **728,652** |

**Inclusive 728,652 + 1,024 outlier bitmaps = 729,676** — this is the quoted ~729,676-byte fixed-block inclusive figure.

Production 5×8 envelope (header + outlier index + decoder): **733,852**.

Ratio vs raw+lookup: 728,652 / 926,496 = 0.786 (saves 197,844 B of table ROM before extras).

## Headroom after adding the corpus (keep compact CJK)

Base image: 4,840,048.

| Add | Image | APP1 left | 85% left | Slot % | Under 85%? |
|-----|------:|----------:|---------:|-------:|:----------:|
| Uncompressed production ranked | 5,716,792 | 1,426,632 | **355,118** | 80.03% | yes |
| Uncompressed production sorted | 5,769,696 | 1,373,728 | **302,214** | 80.77% | yes |
| Fixed-block inclusive 729,676 only | 5,569,724 | 1,573,700 | 502,186 | 77.97% | yes |
| Fixed-block production | 5,573,900 | 1,569,524 | 498,010 | 78.03% | yes |
| Naive EpdFont 16 B glyphs | 6,174,198 | 969,226 | **−102,288** | 86.43% | **no** |

If the pixel corpus **replaces** the 259,142 B compact 2-bit face instead of adding:

| Replace compact with | Image | 85% left |
|----------------------|------:|---------:|
| Uncompressed ranked | 5,457,650 | 614,260 |
| Uncompressed sorted | 5,510,554 | 561,356 |
| Fixed-block production | 5,314,758 | 757,152 |

RAM impact: decode scratch is a 30-byte bitmap plus a few index bytes. Not a RAM constraint (current 96,724 / 114,688 RAM budget).

## Safety margin

Defined for this decision:

1. **Hard OTA/partition limit:** `firmware.bin` ≤ APP1 **7,143,424**. Dual-OTA has no extra scratch; overflow is unflashable.
2. **Existing release gate:** `firmware.bin` ≤ **85% × APP1 = 6,071,910** (1,071,514 B / 15% reserved for growth + padding). Already enforced by `check_m4_build_budget.py`.
3. **Font-decision extra:** after the corpus is present, keep **≥ 256 KiB remaining inside the 85% line** so near-term firmware (diagnostics still on, OX follow-ups, small features) does not immediately fail the gate. A 512 KiB remainder is nicer-to-have, not required to ship the uncompressed face.

All native-grid add options satisfy (1), (2), and (3). Sorted uncompressed meets (3) with 302,214 B; ranked with 355,118 B. The 512 KiB nicer-to-have is missed by uncompressed-add and almost hit by 5×8-add (498 KiB). Replacing compact CJK recovers another 259,142 B.

## Recommendation

**Uncompressed full corpus.** Compression is **not required**.

Reasons:

- The generated raw tables are 868,590 B, not a multi-megabyte mystery. With a production index (ranked bitset 5,000 B or run table 1,140 B), header, 32 outlier sidecars, and a small decoder, adding the face costs about **0.87–0.93 MiB**.
- After that add, APP1 still has **1.37–1.43 MiB** hard headroom and the 85% gate still has **302–355 KiB**. OTA dual-slot remains valid.
- Fixed-block 5×8 inclusive **729,676 B** saves ~197 KiB vs sorted uncompressed, ~143 KiB vs ranked uncompressed. That is optional insurance, not a fit requirement. It needs a 64,662-entry dictionary and a custom decoder; do not take that complexity until a later feature actually crowds the 85% line.
- The only representation that **does** require compression (or rejection) is stuffing the corpus into `EpdGlyph` 16-byte records (~1.33 MiB), which breaks the 85% gate. Do not do that.

Product note (not done here): if the 15×16 pixel face is meant to **replace** the 14 px 2-bit compact CJK fallback, uncompressed is even more comfortable (~561–614 KiB remaining to 85%). This audit does not change that asset.

Explicitly out of scope: integrating Luna `shared-block-*` experiments, changing `m4_compact_cjk_16.h`, or flashing hardware.
