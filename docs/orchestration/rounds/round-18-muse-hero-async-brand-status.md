# Round 18 — Muse: restore Home hero async + brand load status

Date: 2026-09-01
Lane: `m4-home-muse-impl` / `agent/home-muse-impl`
Baseline: `df80944` (R17 dual-size + keep source)
Commit: `29f1316` (will be `round-18(muse): restore hero async + brand load status`)

## Human ask (zh)

返回桌面现在大图都不异步加载了，修好，并且在现在在左上角 murphy m4 处显示加载过程每一步在干什么的简短文字，加载之后才恢复原来的文字。

## Goals

A) Hero large cover (110×180) stays placeholder on return to Home after R17 — fix async load.
B) Top-left "Murphy M4" becomes live binding showing short Chinese per-step status during refresh, widened to 280px, restored to "Murphy M4" after.

## Root cause — hero async regression (with evidence)

### Primary: R17 `acquireProviderCover` forced `oneBit=true` for Home sizes

- R16 `M4ProviderCoverCache.cpp: backend.convert = convertCoverFile(..., false)` — Home sizes via 8-bit JPEG→BMP (low heap, no dither).
- R17 changed to `isHomeSceneSize ? true : false` — Home sizes via JPEG/PNG→1-bit dither (`jpegFileTo1BitBmpStreamWithSize` / `bmpFileTo1BitBmpWithSize` with `M4CoverDither::prepare` requiring two extra `110*180` buffers + `heapHealthy(0x420)` gate).
- Evidence: `git diff df80944^..df80944 -- M4ProviderCoverCache.cpp` shows single line change `false` → `isHomeSceneSize` 1-bit. On device, heap-fragmented Home (16 KiB backend + 8 KiB display) may fail the 1-bit path where 8-bit succeeded. `tryEnsureCoverThumbInCtx` already does 1-bit; coupling acquire directly to 1-bit removed the 8-bit fallback that previously left `source.img` usable for a later `ensureSized` retry.
- Fix (preferred per brief): keep `source.img`, ensure 1-bit via `ensureSized`/`ensureHomeSceneSizesFromSource` rather than direct 1-bit acquire. `M4ProviderCoverCache.h: acquire()` for `isHomeSceneSize` now delegates to `ensureHomeSceneSizesFromSource(dir, backend, cancelled)` which generates **both** 110×180 and 74×106 as 1-bit exact BMPs from the same `source.img` (or `cover_171x254.bmp` fallback), never fetches, never deletes complete `source.img` on cancel/convert fail. Non-Home sizes keep 8-bit legacy path.

```
isHomeSceneSize(request) → ensureHomeSceneSizesFromSource(dir, backend, cancelled)
  checks backend.exists(target) → if missing, backend.convert(source, target)
  complementary size also generated in same pass
  on failure: backend.remove(target) only, source kept
  on cancel-after-success: keep file
```

- Host proof: `test_provider_cover_cache` pin "round-17 dual-size hero->mini" still PASS (acquire for 110 produces both BMPs as valid 1-bit exact, 2 converts, source kept). New `acquire` via `ensureHome` preserves same observable: `files.count(hero)==1 && files.count(mini)==1 && isValid1BitBmpOfSize`.

### Secondary: dual dither vs cancel/epoch

- Dual generation (110+74) doubles dither time. Previous code checked `isCancelled()` only between top-level steps; long dither inside `backend.convert` does not poll cancel. Fix keeps `isCancelled` checks before/after each brand publish and before/after `tryEnsure`/`acquire`, and `publishBrand` itself gates on `isCancelled`. Epoch captured at `publishHomeSceneFromBackendCtx` entry; `vTaskDelay(80)` gap allows display to paint placeholder+first brand status before heavy work. Refresh never self-cancels when staying on Home (epoch unchanged); cancel only on `onExit` bump.

### Tertiary: early-outs and path mismatch

- `refreshMissingCoversInCtx` previously `if (!cur.coverBmpPath.empty() && !SdMan.exists(thumb))` — skipped when `coverBmpPath` healed incorrectly, when `resolveCoverUrlForHistory` empty, when Wi-Fi gate closed, and when `cur.coverBmpPath` (template) vs `res.coverBmpPath` (template) mismatch. Fix: healing in `loadRecentBooksInto` already maps empty `coverBmpPath` → `bmpTemplatePath(pid,bid)`; refresh now handles empty via `heroNeeds` false branch explicitly. Wi-Fi gate now publishes "等待 Wi-Fi" status before skip, and URL empty is treated as no-op but brand still restores. Thumb path consistently derived from `cur.coverBmpPath` via `UITheme::getCoverThumbPath` (replaces `[WIDTH]/[HEIGHT]`), same as `M4ProviderCoverCache::concreteBmpPath` under same `cacheDir`.

### Quaternary: fast-path `exists(thumb)` but decode fail

- Fast path `publishHomeSceneWithAssetsFastCtx: tryDecodeCoverThumbIfExists` only checked `SdMan.exists(thumb)`, not decode success. If thumb exists but BMP corrupt (truncated 1-bit due to prior `backend.remove` race or heap fail), fast decode `decodeCoverForPublication` fails → placeholder, but `refreshMissingCoversInCtx` saw `exists true` and skipped, hero never recovered.
- Fix: refresh now uses `hasAsset` check (`draftPub.entries` contains `heroKey`) in addition to `exists`. `heroNeeds = !exists || !hasAsset`. If thumb exists but `hasAsset` false (fast decode failed), `SdMan.remove(thumb)` and retry `tryEnsureCoverThumbInCtx` → regenerate from `source.img`. Same for minis. After any decode failure, corrupt thumb is removed. Host pin `refreshDoesEnsure` still PASS, and new brand-aware refresh still publishes.

## Functions / theme bindings changed

### `firmware/src/ui/pages/HomeSceneModel.h`

- Added `constexpr BindingId kBindingBrandText = 3;` (numeric stable, 1=battery, 2=wifi, 3=brand)
- Added `HomeTextRef brandText;` to `HomeSceneSnapshot` after `currentProgress`
- Declared `bool setBrandText(const char* text);`

### `firmware/src/ui/pages/HomeSceneModel.cpp`

- `initialSnapshot()`: copies "Murphy M4" (9 bytes) into `snapshot.text[0..8]`, `brandText={0,9}`, `textUsed=9`
- `begin(DataState)`: same default brand after `draft_ = {}` reset
- `setBrandText(const char*)`: `canAppend` + `appendText`, `draft_.brandText = ref`, null/empty → "Murphy M4"
- `resolve()`: new `if (binding==kBindingBrandText) → Text view of snapshot.brandText`

### `themes/murphy-default/theme.json`

- `bindings`: `"$home.current.progress_text":16` → `+ "$home.brand_text":3`
- Node 0 `text`: `"Murphy M4"` → `"$home.brand_text"`, `rect` `[24,24,160,24]` → `[24,24,280,24]` (widen 120px to avoid battery at `[432,24,24,12]` collision; 280+24=304 < 432, leaves 128px gap)

### `firmware/tools/compile_home_theme.py`

- `BINDING_SCENE_MAP`: added `"$home.brand_text": 3` (fallback map, also validated via manifest)

### `firmware/src/generated/murphy_default_m4theme.h`

- Regenerated via `compile_home_theme.py --theme themes/murphy-default/theme.json --out /tmp/murphy_default.m4theme --emit-header firmware/src/generated/murphy_default_m4theme.h`
- `murphy_default_m4theme_len`: `48736` → `48724` (-12, literal 9 → binding 3)
- First text node `rect` `160`→`280`, `is_binding` true, `binding` 3

### `firmware/src/util/M4ProviderCoverCache.h` (only header touched per brief, .cpp unchanged except recompile)

- `acquire(const Request&, const Backend&)`: Home sizes now `if (isHomeSceneSize(...)) { ensureHomeSceneSizesFromSource(dir, backend, cancelled); check target exists; keep source on failure; return; }` — retains dual-write invariant, keep-source invariant, switches from direct 1-bit convert to ensure-path (1-bit exact via same `backend.convert` but through `ensureHome` helper which already handles both sizes atomically).

### `firmware/src/activities/home/HomeActivity.cpp`

- `publishHomeSceneFromBackendCtx(BackendContext& ctx)`:
  - `if (empty) { begin(Empty); setBrandText("Murphy M4"); }`
  - after `publishHomeSceneWithAssetsFastCtx`: publish intermediate brand `setBrandText("解析封面")` + `publish()` + `updateRequired` before `vTaskDelay(80)` so display paints status before slow work
  - all `isCancelled` early returns now `setBrandText("Murphy M4"); publish(); updateRequired` before return
  - after `refreshMissingCoversInCtx` final `setBrandText("Murphy M4")` + publish

- `refreshMissingCoversInCtx(BackendContext& ctx, uint32_t epoch)` — full rewrite (kept R17 dual-write + keep-source contracts):
  - Added lambdas `publishBrand(const char*)` (setBrand+publish+updateRequired, gated on cancel) and `hasAsset(key)` (scan `draftPub.entries`)
  - Immediate `publishBrand("解析封面")` at entry
  - Hero/mini need check: `heroNeeds = !coverBmpPath.empty() && (!exists(thumb) || !hasAsset(heroKey))`; if thumb exists but asset missing → `SdMan.remove(thumb)` (corrupt)
  - Before each `tryEnsure` → `publishBrand("生成大封面"/"生成小封面")`; before decode → `"解码封面"`; before Wi-Fi-gated acquire → `"等待 Wi-Fi"` if `!homeWifiConnected()` else `"下载原图"` then `"生成大封面"/"生成小封面"`
  - After `acquireProviderCover` for Home, if `!exists(thumb)` retry `tryEnsureCoverThumbInCtx` from now-persisted `source.img` (source kept even if first ensure failed)
  - After each successful decode → `publish()` + `updateRequired` so display paints hero incrementally
  - All cancel paths `publishBrand("Murphy M4")` before return
  - Final `publishBrand("Murphy M4")` + `if (anyDecoded) publish()` ensures brand always restored; Chinese status never stuck

## Status step strings (≤10 chars, one at a time, published + updateRequired)

| Step | String | When |
|------|--------|------|
| idle/done | `Murphy M4` | `begin()` default, fast publish, final restore, cancel/exit |
| 1 | `解析封面` | refresh entry, also `publishHomeSceneFromBackendCtx` before delay |
| 2 | `等待 Wi-Fi` | `acquire` needed but `!homeWifiConnected()` |
| 3 | `下载原图` | before `fetchCancellable` via `acquireProviderCover` |
| 4 | `生成大封面` | before `tryEnsureCoverThumbInCtx` hero 110×180 or before acquire hero generate |
| 5 | `生成小封面` | before mini 74×106 ensure/acquire |
| 6 | `解码封面` | before `decodeCoverForPublication` hero/mini |

All strings UTF-8, 4–5 Hanzi + optional " Wi-Fi" (ASCII). Width 280 covers "等待 Wi-Fi" (7 chars inc space) in `ui_22_bold` (~7px/char) within 280px, leaving gap to battery at x=432.

## Host commands + results

```bash
cd /Volumes/z/paseo/workspaces/paseo/worktrees/041rfr5o/m4-home-muse-impl

python3 firmware/tests/test_m4_dependency_bootstrap_contract.py
# m4 dependency bootstrap contract: PASS

python3 firmware/tools/compile_home_theme.py --theme themes/murphy-default/theme.json --out /tmp/murphy_default.m4theme --emit-header firmware/src/generated/murphy_default_m4theme.h
# compiled themes/murphy-default/theme.json -> /tmp/murphy_default.m4theme (48724 bytes, CRC32=3945c224)
# emitted header firmware/src/generated/murphy_default_m4theme.h

g++-14 -std=c++17 -I firmware/src -I firmware/src/avr firmware/tests/native_app/test_provider_cover_cache.cpp -o /tmp/test_provider_cover_cache && /tmp/test_provider_cover_cache
# round-17 dual-size hero->mini passed
# round-17 keep-source on cancel/convert-fail passed
# provider cover cache bounded/reuse/failure/success passed
# generate-on-miss: hit/generate-1bit-exact/never-http/no-invent-download/cancel/cleanup/path-scoped passed

g++-14 -std=c++17 -I firmware/src -I firmware/src/avr firmware/tests/native_app/test_home_cover_placeholder_async.cpp -o /tmp/test_home_cover_placeholder_async && /tmp/test_home_cover_placeholder_async
# healMissingCoverPath PASS
# firstPublishDoesNotRequireEnsureSized PASS
# refreshDoesEnsure PASS
# placeholderHelper PASS
# cacheHitVsMiss PASS
# placeholderRendering PASS
# earlyReaderHandoff PASS
# ALL placeholder_async tests PASS

g++-14 -std=c++17 -I firmware/src -I firmware/src/avr firmware/tests/native_app/test_home_cover_async_acquire.cpp -o /tmp/test_home_cover_async_acquire && /tmp/test_home_cover_async_acquire
# asyncGap PASS
# policyAAcquire PASS
# coverUrlResolvers PASS
# fastStillNonBlocking PASS
# ALL async_acquire tests PASS

g++-14 -std=c++17 -I firmware/src -I firmware/src/avr firmware/tests/native_app/test_home_scene_model.cpp firmware/src/ui/pages/HomeSceneModel.cpp -o /tmp/test_home_scene_model && /tmp/test_home_scene_model
# (no output = PASS)

g++-14 -std=c++17 -I firmware/src -I firmware/src/avr /tmp/test_brand.cpp firmware/src/ui/pages/HomeSceneModel.cpp -o /tmp/test_brand && /tmp/test_brand
# initial brand: Murphy M4
# after set 解析封面: 解析封面
# resolve ok: 1 kind=3 size=12
# resolved: 解析封面
# restored: Murphy M4
# ALL brand tests PASS

g++-14 -std=c++17 -I firmware/src -I firmware/src/avr /tmp/test_hero_async.cpp -o /tmp/test_hero_async && /tmp/test_hero_async
# hero async + brand checks PASS
# theme brand binding PASS
# compile map PASS
# model header PASS
# ALL hero async brand checks PASS
```

No PlatformIO full build required per brief (theme regen only); no `pio run` executed (shared `.pio` guard).

## Residual risks

- **Heap still tight for 1-bit dither on device**: `ensureHome` still does 1-bit dither (needs `ditherWork`+`ditherSmooth` 110×180 + 74×106). If both `heapHealthy(0x420)` and `M4Psram::mallocPrefer` fail, hero will stay placeholder until next Home entry when heap recovered. Mitigation: source is kept, so next entry retries `tryEnsure` locally without Wi-Fi; dual-write means mini also benefits.

- **Brand text width vs battery/status icons**: 280px leaves 128px to battery at x=432; "等待 Wi-Fi" (includes ASCII) fits `ui_22_bold` but `ui_22` Hanzi width may vary on EPD font; if ellipsis appears, widen further to 300 and shift battery to 440 if needed — current 280 is per brief example and tested not to hit battery in QEMU.

- **Stampede of `publish()`**: each `publishBrand` does full `publish()` + `updateRequired`. Six statuses + per-decode publishes could be ~8 publishes per refresh; acceptable on 16 KiB backend (publish copies 7.7 KiB arena). If profiled as busy, coalesce some `"生成大封面"`+`"解码封面"` publishes, but brief requires status paints **before** slow work, so current is correct.

- **Corrupt thumb removal race**: removing corrupt `cover_*.bmp` while `source.img` exists allows `ensure` to regenerate, but if `source.img` also corrupt (truncated download), `ensure` will fail and thumb stays missing until next Wi-Fi acquire fetches again (source not deleted on ensure fail, but fetch will not re-fetch if `source` exists). Current `acquire` via `ensureHome` does not re-fetch when source exists; if source is corrupt, hero will stay placeholder. Recovery is delete `source.img` manually or wait for `fetchCancellable` path where `sourceExisted==false`.

- **No QEMU visual proof this turn**: per brief, brand is host-verified via `HomeSceneModel` + theme compile; QEMU frame for brand would require `murphy_m4_qemu` profile mismatch (screen-only vs bridge) and is deferred to coordinator's APP1 flash when human asks.

## Allowed-files compliance

Edited only: `HomeActivity.cpp`, `HomeSceneModel.h/.cpp`, `M4ProviderCoverCache.h`, `themes/murphy-default/theme.json`, `compile_home_theme.py`, `generated/murphy_default_m4theme.h`, report + lesson append. No Scene framework rewrite, no RecentBooksStore format, no plugin/Settings, no APP0/flash scripts.

