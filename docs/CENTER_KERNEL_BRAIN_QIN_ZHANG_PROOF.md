# Font CenterKernel — Brain/Qin/Zhang/Mang Proof & 10-Glyph Lock

## 1. Brain U+8111 XOR=13 — Source-faithful duplicate removal

**Pre-collapse sampler:** point sampling at `Xcanon(cls,col)=80+PHASE_DELTA[cls]+60*col`, `Ycanon(row)=751.5-60*row` with `PHASE_DELTA[20.5]=0.5` (class2, Kx=75).
**Production decoder:** geometry-exact run-deconvolution: for each Y-slab filled X interval `[x0,x1]` with `W=x1-x0`, if `W == K+(n-1)*60` for `K∈{74,75}` decode `n` centers `x_i=x0+K/2+i*60`; otherwise, for `W>=74`, use only endpoint anchors `x0+K/2` and `x1-K/2`. Apply the symmetric rule to X-slab Y intervals and Y-slab spans. Keep only if the `Kx*Ky` cell is fully contained (winding-aware) and has `x_support OR y_support`, then quantize once via `Xcanon/Ycanon` with half-pitch tie `≤30` toward lower index. Source recovery is class-independent; storage is class-aware.

**All 13 removed bits are duplicate samples of genuine 74-UPM phase-50.0 single cells:**

- Source cell `x=650` spans `[613,687]` (W=74, K=74). Class2 lattice points `620.5` (col9) and `680.5` (col10) both lie inside `[613,687]` (dist 29.5/30.5), so pre-collapse emitted two columns for one pixel. New decoder recovers `650` once and `Xcanon` nearest tie picks `col9` once.
- Each of the 13 positions maps to a Y-slab containing an exact 74-wide clean interval:

| row | col | Xcanon | Ycanon | Y-slab | X-interval | K | centers | overlapping cols |
|-----|-----|--------|--------|--------|------------|---|---------|------------------|
| 0,10 | 680.5 | 751.5 | [729,789] | [613,687] W74 | 74 | [650] | [9,10] |
| 1,10 | 680.5 | 691.5 | [654,729] | [613,687] W74 | 74 | [650] | [9,10] |
| 2,10 | 680.5 | 631.5 | [609,654] | [613,687] W74 | 74 | [650] | [9,10] |
| 6,8  | 560.5 | 391.5 | [369,414] | [493,567] W74 | 74 | [530] | [7,8] |
| 6,12 | 800.5 | 391.5 | [369,414] | [733,807] W74 | 74 | [770] | [11,12] |
| 7,9  | 620.5 | 331.5 | [309,354] | [553,627] W74 | 74 | [590] | [8,9] |
| 7,11 | 740.5 | 331.5 | [309,354] | [673,747] W74 | 74 | [710] | [10,11] |
| 8,10 | 680.5 | 271.5 | [249,294] | [613,687] W74 | 74 | [650] | [9,10] |
| 9,10 | 680.5 | 211.5 | [189,249] | [613,687] W74 | 74 | [650] | [9,10] |
|10,9  | 620.5 | 151.5 | [129,174] | [553,627] W74 | 74 | [590] | [8,9] |
|10,11 | 740.5 | 151.5 | [129,174] | [673,747] W74 | 74 | [710] | [10,11] |
|11,8  | 560.5 |  91.5 | [54,114]  | [493,567] W74 | 74 | [530] | [7,8] |
|11,12 | 800.5 |  91.5 | [54,114]  | [733,807] W74 | 74 | [770] | [11,12] |

Analogous to 白/美 duplicate-row removal (horizontal 75-on-60 double hits collapsed to single row). Do NOT restore.

**Regression test:** `test_8111_brain_single_width_phase50_duplicates` locks `NAO_GRID` (60 bits vs old 73) and asserts the 13 positions are `.`.

## 2. 寝 and 胀 — Production OR support preserves 0 XOR

The production transition rule uses endpoint anchors plus `x_support OR y_support`; full containment remains mandatory. The legitimate source centers `(140,691.5),(140,631.5),(153,584.5)` for 寝 map to `(1,1),(2,1),(3,1)`, and the legitimate bottom centers `(281,-148.5)` and `(320,-148.5)` for 胀 map to `(15,3),(15,4)`. These are genuine interior cells, not corrections to be removed, so both glyphs remain 0 XOR versus the pre-collapse sampler.

`test_qin_zhang_production_behavior` locks these coordinates and `test_ten_glyph_production_locked` locks both complete production grids.

## 3. 10-Glyph Production Lock

| Glyph | CP | Class | Phase | Kx | XOR vs pre-collapse | Expected Grid (16×16) |
|-------|----|-------|-------|----|---------------------|-----------------------|
| 田 | U+7530 | 1 | 20.0 | 74 | 0 | TIAN_GRID (anchors) |
| 中 | U+4E2D | 1 | 20.0 | 74 | 0 | ZHONG_GRID |
| 白 | U+767D | 1 | 20.0 | 74 | 9 | BAI_GRID (dup rows removed) |
| 美 | U+7F8E | 1 | 20.0 | 74 |10 | MEI_GRID |
| 寝 | U+5BDD | 2 | 20.5 | 75 | 0 | QIN_GRID |
| 寫 | U+5BEB | 2 | 20.5 | 75 | 0 | XIE_GRID |
| 胀 | U+80C0 | 2 | 20.5 | 75 | 0 | ZHANG_GRID |
| 脑 | U+8111 | 2 | 20.5 | 75 |13 | NAO_GRID |
| 盲 | U+76F2 | 2 | 20.5 | 75 |18 | MANG_GRID (six side pixels preserved) |
| 猫 | U+732B | 3 | 50.0 | 74 | 0 | MAO_GRID (single-thick) |

`test_ten_glyph_production_locked` asserts these production grids. The approved lock deltas are 9+10+13+18 removed bits for 白/美/脑/盲; 田/中/寝/寫/胀/猫 remain 0 XOR versus pre-collapse.

## 4. Full Corpus Regeneration

- **Production generator:** `firmware/scripts/generate_m4_center_kernel.py::geometry_source_grid` is the sole decoder: geometry-exact run-deconvolution, `W=K+(n-1)*60` / `H=75+(m-1)*60`, symmetric slab-span evidence, endpoint anchors for non-decodable spans, full-containment, winding-aware `x_support OR y_support`, class-independent source recovery, class-aware storage `Xcanon=80+PHASE_DELTA+60*col`, and half-pitch tie 30 toward lower index.
- **Regenerated at 2026-08-25 21:02:07 +0800** via `python3 firmware/scripts/generate_m4_center_kernel.py --font '/Users/zhouxinlai/Downloads/TTF字体（放FONT文件夹）/标准像素粗.ttf'`
- **Artifacts:**
  - `firmware/src/fontdata/m4_center_kernel_16x16.bin` 939815 bytes, sha256 `3584128056dbe4c1fe7e7995344ba92f06f0890ab22544aedb721393025ecc99`
  - `m4_center_kernel_16x16.h` / `.json` updated, `supported_corpus_sha256` `585eaec2fb0d198ad54d0c622fb90de7e35e9f18b5b2a3cbb99b0ed31b7a5513`
  - `joint_class_histogram` {0:18376, 1:10604, 2:4, 3:1}, `latin_and_other_count` 1432, `clipping_stats` {}
- **Verify:** `python3 firmware/scripts/generate_m4_center_kernel.py --verify` → `center-kernel ok glyphs=28985 bytes=939815 sha256=35841280...`
- **Self-test:** `--self-test` → `self-test ok`

## 5. Focused Tests

- `python3 -m unittest simulator.tests.test_center_kernel_reader_contract.CenterKernelSourceGridContract` — 10 tests OK (geometry-native, zero clipping, Tian/Zhong, Bai/Mei, class-X doc, cat single-thick, brain 13, 寝/胀 production behavior, 10-glyph lock including 盲)
- `CenterKernelReaderContract` 8 core tests OK (allowed sizes, N16 1x1 for all 4 classes, N32 split excluded, advance 960 vs 1000, occupancy centered)
- `test_chrome_path_untouched_by_reader_kernel` and `test_native_grid_font::test_main_binds_chrome…` still fail on current `main.cpp` (pre-existing, chrome now uses CenterKernel not NativeGrid; not related to font occupancy; tracked separately).

## 6. Representation Invariance (class0/2/3)

- `kClasses` table: `{960,30.5,75},{1000,20.0,74},{1000,20.5,75},{1000,50.0,74}`
- `PHASE_DELTA` per class: `-49.5, 0.0, 0.5, 30.0` with `X_BASE=80.0`
- `test_n16_kernels_are_1x1_for_all_four_classes` → `kernelX/Y` at `N=16` → `(1,1)` for all 4 classes (rhu)
- `test_n32_splits_k74_vs_k75_and_is_excluded` → `N=32` `K74=2` vs `K75=3` split correctly excluded
- `test_advance_960_vs_1000_differs` → `advancePx` `n*advance/960` differs per class
- `test_class_specific_x_alignment_documented` → generator documents `class0` offset (`960/30.5` vs `left+1`)
- No per-character exceptions (`0x767D`/`0x7F8E` absent)


## 7. Deterministic Spread Audit (blob vs pre-collapse point sampler)

For each sorted CJK class list `lst` of length `L`, samples use
`lst[int((i + off) * L / n) % L]` with `off=0`.

- class1 (`L=9172`): n=1200 → 21 differing glyphs, 0 added, 131 removed, 0 unproven; n=2400 → 46, 0, 271, 0.
- class0 (`L=18376`): n=1200 and n=2400 → 0 differing glyphs, 0 added, 0 removed, 0 unproven.
- class2 (`L=4`): all 4 → 1 differing glyph (脑), 0 added, 13 removed, 0 unproven.
- class3 (`L=1`): all 1 → 0 differing glyphs, 0 added, 0 removed, 0 unproven.

Known lock deltas are 9+10+13+18 removed bits for 白/美/脑/盲. 田、中、寝、寫、胀、猫 remain 0 XOR; the 寝/胀 transition cells are legitimate source occupancy, not corrections.

*FT17 (17ppem MONO, x0=left-1 y0=13-top) vs new geometry:*

- Tian 0, Zhong 0, Bai 0, Mei 0, Xie 0, Mao 59, Qin 2, Zhang 2, Nao 13
- FT17 is pixel-perfect for class1 (verified Tian/Zhong) but at 58.82 UPM/px it mis-aligns class2 phase 20.5 (K75) by ~0.84px vs class1, causing 2-bit residual for Qin/Zhang and 13 for Nao (same duplicate as old). For class3 (Mao) FT scale 1.02× creates double-thick artifact (59 bits).

Top 20 vs FT17 (blob vs FT17, full scan):
```
59 U+732B 猫 cls3  (FT double-thick)
13 U+8111 脑 cls2
 2 U+5BDD 寝 cls2
 2 U+80C0 胀 cls2
 0 others for class1
```

No full-corpus audit was run in this handoff; the bounded deterministic spread audit above is the required pre-promotion evidence.

## 8. Verification Gates

- `python3 firmware/scripts/generate_m4_center_kernel.py --verify` : **OK** `glyphs=28985 bytes=939815 sha256=35841280...`
- `python3 firmware/scripts/generate_m4_center_kernel.py --self-test` : **OK**
- `python3 -m unittest simulator.tests.test_center_kernel_reader_contract.CenterKernelSourceGridContract` : **10 OK**
- `CenterKernelReaderContract` 8 core tests **OK**
- Representation invariance: `N16 1×1` for all 4 classes, `N32` split correctly excluded, `advance 960 vs 1000` differs, `PHASE_DELTA` documented.

## 9. PlatformIO Firmware Build

- Previous `pio run -e murphy_m4` at 2026-08-25 17:30 produced `firmware/.pio/build/murphy_m4/firmware.bin` 5.3M (5,457,872 bytes) OK.
- Current blob same size (939815) and `verify` OK → build expected OK. Direct `pio run -e murphy_m4` in this sandbox hits `PermissionError` on `~/.platformio` (sandbox restricts `~/.platformio/.cache/uv`) and background download of `platform-espressif32@55.3.37` still in progress (`/tmp/pio_packages`). No flash performed as requested. Recommend host run: `export PATH=$HOME/.platformio/penv/bin:$PATH; export PLATFORMIO_BUILD_CACHE_DIR=$HOME/.cache/murphy-m4/platformio-build-cache; cd firmware && pio run -e murphy_m4` to refresh.
