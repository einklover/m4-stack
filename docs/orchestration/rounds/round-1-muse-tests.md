# Round 1 — Lane C (muse-tests) — cover-cache generate-on-miss contracts

**Branch:** `agent/home-muse-tests` (HEAD `f2b216e` → new commit)  
**Worktree:** `/Volumes/z/paseo/workspaces/paseo/worktrees/041rfr5o/m4-home-muse-tests`  
**Date:** 2026-08-31  
**Lane:** C — tests-only (`muse-spark-1.2-contributor`, thinking ultra)  
**Parent callback:** Grok-coordinated split, notifyOnFinish → Grok audit

## Scope (allowed files only)
- `firmware/tests/native_app/test_provider_cover_cache.cpp` — strengthened
- `simulator/tests/test_provider_cover_cache_contract.py` — strengthened
- `firmware/tests/native_app/test_cover_last_resort.cpp` — new, guarded (skip when Lane A API absent)
- `docs/orchestration/rounds/round-1-muse-tests.md` — this report

No edits to `firmware/src` or `firmware/lib` (contract).

## Contracts strengthened (Home generate-on-miss)

All generate from **already-downloaded** `source.img` (JPEG) → exact-size **1-bit** `cover_{w}x{h}.bmp`. Lane A last-resort from `cover_171x254.bmp` is guarded/skip in this worktree.

| Contract | C++ native | Python static |
|---|---|---|
| **cache hit when cover_{w}x{h}.bmp exists** | hit returns `cacheHit=true`, no `convert`, no `fetch`, thumbPath == target; re-hit stays hit | asserts `if (backend.exists(target)) { out.cacheHit=true; return }` early |
| **generate from source.img JPEG, persist 1-bit exact WxH BMP** | miss calls `convert(source.img, target, W,H)` with exact W/H (110×180, 74×106), creates `cover_110x180.bmp` / `cover_74x106.bmp` that parse as 1-bit BMP (bpp=1, offset=62, palette black/white, rowBytes=(W+31)/32*4, fileSize=62+rowBytes*H), second call is hit (persisted), `source.img` has priority over `cover_171x254.bmp` | asserts `sourcePathInDir`, `sizedBmpPathInDir`, `backend.convert(source,target,width,height)`, `convertCoverFile(...,true)` + `jpegFileTo1BitBmpStreamWithSize` + `writeBmpHeader1bit`/`exactTarget` |
| **never HTTP / never fetch** | every `ensureSizedCoverFromSource` path (hit, miss, no-source, cancel, outside, convert-fail) leaves fetch count 0 including `fetchCancellable` | asserts `backend.fetch` absent in inline `EnsureSizedResult` slice and in production wrapper slice, no `M4NativeProviderHttp`/`requestToSink`; comment `Never fetches` present |
| **missing source does not invent a download** | `source.img` missing → `thumbPath.empty()`, `!cacheHit`, `!generated`, no fetch, no convert, no file created; holds even when `cover_171x254.bmp` exists (no silent fallback) | asserts `if (!backend.exists(source)) { out.thumbPath.clear(); return }` with no fetch in slice |
| **convert failure removes partial target** | convert returns false after partial write, or `backend.exists(target)` false after `convert==true`, both trigger `backend.remove(target)` and `thumbPath.empty()` | asserts `if (!backend.convert(...) || !backend.exists(target)) { if (backend.remove) backend.remove(target); out.thumbPath.clear(); }` |
| **path must be under /.crosspoint/provider_covers/** | `isProviderCoverCacheDir` + `directoryOfCoverPath` checked; outside (`/books/local`, `/tmp/evil`, empty, `/.crosspoint/other`) returns empty with no convert/fetch/remove; helpers validated (`cacheDir`, `bmpTemplatePath`, `sourcePath`, `concreteBmpPath`, `sourcePathInDir`, `sizedBmpPathInDir`) and edge widths/heights ≤0 return empty safely | asserts presence of `isProviderCoverCacheDir`, `directoryOfCoverPath`, `sourcePathInDir`, `sizedBmpPathInDir`, `cacheDir`, `bmpTemplatePath`, `sourcePath`, `concreteBmpPath` and gating `dir.empty() || !isProviderCoverCacheDir(dir)` |
| **cancelled semantics** | `cancelled` true before convert → no convert/fetch, empty; `cancelled` toggled true after convert → file kept, `generated=true` (next Home paint is hit); `cancelled==false` → normal generate | asserts at least 2 `if (cancelled && cancelled())` checks and comment `keep the file so the next Home paint is a hit` |
| **last-resort from cover_171x254.bmp** | `test_cover_last_resort.cpp` guards: if `cover_171x254` file name absent in header/cpp, prints SKIP and passes (Lane A pending); if present, asserts it appears **after** `!backend.exists(source)` and still `Never fetches` + `backend.remove` | python `test_last_resort_from_171x254_is_guarded_or_absent` skips when `cover_171x254` absent (comment `// (171x254)` ignored), otherwise asserts ordering after source missing |

## Additional native guards added
- `makeFake1BitBmp` / `parseBmpHeader` / `isValid1BitBmpOfSize` helpers verify 1-bit BMP header fields (BM, fileSize, offset 62, DIB 40, W/H exactly requested, planes 1, bpp 1, palette 00.. / FF.., rowBytes alignment).
- Helpers `directoryOfCoverPath`, `isProviderCoverCacheDir`, `sourcePathInDir`, `sizedBmpPathInDir` unit-tested.
- Width/height ≤0 and missing backend callbacks return empty safely.
- Fetch counters cover both `backend.fetch` and `backend.fetchCancellable`.

## Commands executed (exact as required)

### Native (MUST use g++-14, not Apple clang++)
```bash
/opt/homebrew/bin/g++-14 -std=c++17 -I firmware/src -I firmware/src/apps firmware/tests/native_app/test_provider_cover_cache.cpp -o /tmp/test_provider_cover_cache
/tmp/test_provider_cover_cache
```

**Result:** PASS
```
provider cover cache bounded/reuse/failure/success passed
generate-on-miss: hit/generate-1bit-exact/never-http/no-invent-download/cancel/cleanup/path-scoped passed
```
Exit 0 via `/opt/homebrew/bin/g++-14` (Homebrew GCC 14.2.0_1).

Guarded last-resort binary:
```bash
/opt/homebrew/bin/g++-14 -std=c++17 -I firmware/src -I firmware/src/apps firmware/tests/native_app/test_cover_last_resort.cpp -o /tmp/test_last_resort
/tmp/test_last_resort
```

**Result:** PASS (SKIPPED — guard)
```
cover last-resort: SKIPPED — production cover_171x254 API not in this worktree yet (Lane A pending); guard passes
```

### Python
```bash
python3 simulator/tests/test_provider_cover_cache_contract.py -v
```

**Result:** PASS (11 tests, 1 skipped)
```
test_ensure_sized_cache_hit_when_target_exists ... ok
test_ensure_sized_cancelled_semantics ... ok
test_ensure_sized_convert_failure_removes_partial_target ... ok
test_ensure_sized_generates_1bit_exact_from_source_img ... ok
test_ensure_sized_missing_source_does_not_invent_download ... ok
test_ensure_sized_never_http_never_fetch ... ok
test_ensure_sized_path_must_be_under_provider_covers ... ok
test_home_scene_sizes_generate_from_source_img_without_fetch ... ok
test_last_resort_from_171x254_is_guarded_or_absent ... skipped 'last-resort cover_171x254 API not in this worktree yet (Lane A) — guard passes'
test_png_uses_existing_bounded_decoder_and_bmp_output_contract ... ok
test_unknown_and_webp_are_not_converted ... ok

OK (skipped=1)
```

## Files changed
- `firmware/tests/native_app/test_provider_cover_cache.cpp` — expanded from ~200 to ~687 lines; retains all original acquire/bounded/reuse/format/extensionless checks; adds exhaustive generate-on-miss, 1-bit exact, never-fetch, missing-source, cleanup, path-scoped, cancelled, and helper tests.
- `simulator/tests/test_provider_cover_cache_contract.py` — 3 → 11 tests; adds 8 new static contract assertions; last-resort test guards on `cover_171x254` file name (ignores comment) and skips.
- `firmware/tests/native_app/test_cover_last_resort.cpp` — new guarded fixture; skips when `cover_171x254` absent, validates priority/never-fetch/cleanup when present.
- `docs/orchestration/rounds/round-1-muse-tests.md` — this report.

## Hard bans complied
- No edit to `firmware/src` or `firmware/lib`.
- No Codex Sol, no GitHub push, no hardware flash, no `pkill -f m4adb.py`, no `:18080` hostfwd, no concurrent PlatformIO.
- No full PlatformIO build, no QEMU, no flash (as instructed).

## Git
Branch `agent/home-muse-tests` only. Message: `round-1(muse-tests): cover-cache generate-on-miss contracts` (next step).
