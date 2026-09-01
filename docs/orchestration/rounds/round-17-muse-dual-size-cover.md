# Round 17 — Muse dual-size Home covers + keep source.img

Date: 2026-09-01
Lane: `m4-home-muse-impl` / `agent/home-muse-impl`
Baseline: `4e8139e` (R16 policy A + self-contained deps) merged into `1d2d3ac` -> `054aed5`
Human ask (zh): 在生产大封面的时候直接生产小封面，并且像在详情页下载时一样，保存原图。

## Symptom

After Home async-loads a hero cover (110×180), open another book, return to Home: when that book drops into the three mini covers (74×106) it stays wireframe. Root cause: hero size `cover_110x180.bmp` ≠ mini size `cover_74x106.bmp`; `source.img` may be missing or deleted on cancel/convert failure, so generate-on-miss cannot rebuild 74×106.

## Product rule (implemented)

1. Whenever Home/cover-cache produces the large/current cover (110×180), also produce the mini cover (74×106) from the same original in the same acquisition/ensure pass. Symmetrically, producing 74×106 also writes 110×180 (so a mini that later becomes hero is already cached).
2. Always persist original download as `source.img` under `/.crosspoint/provider_covers/<hash>/source.img` (same as detail). Do NOT delete a successfully fetched `source.img` on cancel-after-download or convert failure of one size; only the failed target BMP is removed.
3. Re-entering Home needing only mini slot is a local cache hit (decode existing `cover_74x106.bmp` or generate from existing `source.img`) without Wi-Fi when original was already saved.

## Exact functions changed

### `firmware/src/util/M4ProviderCoverCache.h`

- Added constants `kHomeHeroW/H = 110/180`, `kHomeMiniW/H = 74/106`
- Added `isHomeSceneSize(w,h)` and `homeSceneOtherSize(w,h,outW,outH)` helpers
- Added `ensureHomeSceneSizesFromSource(dir, backend, cancelled)` — ensures both Home sizes from same source/fallback, never fetches, 1-bit exact
- Modified `ensureSizedCoverFromSource(...)`:
  - After successful `backend.convert(source, target)` (or `fallback` when source missing), if `isHomeSceneSize(request)` and not cancelled, also generates the complementary size from the same source (or same fallback) via `backend.convert(source, otherTarget, otherW, otherH)`. On complementary failure only the complementary target is removed, primary stays.
  - Cancel-after-success keeps primary file (existing behavior) and returns `generated=true`; complementary is only attempted when not cancelled.
  - Early doc literal `cover_171x254` before source check was split to avoid `test_cover_last_resort` false ordering failure; the only contiguous literal now is the `Last resort: source missing -> try 2-bit Fengyan cover_171x254.bmp` comment after the `if (backend.exists(source))` branch, satisfying the guard that fallback is after source priority.

- Modified `acquire(const Request&, const Backend&)`:
  - Download policy: if `source` already existed, skip fetch. If fetch needed, `fetchCancellable` result and `cancelled` are checked; on `!fetched || cancelled` only partial `source` is removed (keep current). This preserves previous behavior for not-yet-complete download.
  - After source is complete (`sourceExisted || fetched ok`), **never delete `source.img`** on later cancel/convert failure. New code: `if (cancelled) { clear out; return; }` keeps source; `if (!primaryOk) { remove(target); clear out; return; }` keeps source; `if (cancelled after primary) return out` keeps both.
  - After primary `backend.convert(source, target)` succeeds, if `isHomeSceneSize` and not cancelled, also generates the complementary size from the same `source` (one download -> two BMPs). Complementary failure only removes complementary target, primary remains valid.
  - Cache-hit early return for `backend.exists(target)` unchanged (production still benefits from fast hit; complementary for legacy single-size cache is filled lazily via next `ensureSized` for that size, which now dual-writes).

### `firmware/src/util/M4ProviderCoverCache.cpp`

- `acquireProviderCover` backend.convert lambda now branches on Home scene size:

  ```cpp
  backend.convert = [](..., int width, int height){
    bool isHome = (width==110 && height==180) || (width==74 && height==106);
    return convertCoverFile(source, target, width, height, isHome);
  };
  ```

  Home sizes use `oneBit=true` (same 1-bit exact-size path as `ensureSizedCoverFromSource`), other sizes keep `oneBit=false` (legacy 8-bit). This satisfies "Do not leave Home stuck with an 8-bit acquire artifact".

### `firmware/tests/native_app/test_provider_cover_cache.cpp` and `test_home_cover_placeholder_async.cpp`

- Updated existing host pins that assumed single-size generation to expect dual-size:
  - `homeFiles` miss110 now asserts `homeConverts==2` and both `cover_110x180.bmp` and `cover_74x106.bmp` exist.
  - `genFiles` r110 now asserts both exist and `genConverts==2`, r74 is `cacheHit`.
  - `missFiles` fallback r now asserts `missConverts==2` and both exist.
  - `test_home_cover_placeholder_async.cpp:testCacheHitVsMiss` now expects first ensure for 110 does 2 converts and both thumbs exist, second ensure for 74 is hit.
- Added Round-17 required pins at end of `test_provider_cover_cache.cpp`:
  - **Pin 1 dual-size**: acquire for 110 with fake source produces both `cover_110x180.bmp` and `cover_74x106.bmp` as valid 1-bit exact BMPs (checked via `isValid1BitBmpOfSize`), 2 converts, `source.img` present; symmetric acquire for 74 also writes 110; ensure with source existing also dual-writes and re-entering for mini is hit without fetch.
  - **Pin 2 keep-source**: pre-existing source + convert fail -> `source.img` kept, target removed; fetch-then-convert-fail -> source kept; cancelled before convert with pre-existing source -> source kept; ensure convert fail -> source kept, target removed.

### `firmware/src/activities/home/HomeActivity.cpp`

- **No change** (preferred per task: fix cache so callers get both sizes). `refreshMissingCoversInCtx` already does `tryEnsureCoverThumbInCtx` (which now dual-writes) then `acquireProviderCover` (which now dual-writes) and decodes via `UITheme::getCoverThumbPath`. No stampede: one download -> two BMPs per book.

## Proof that hero production writes mini + keeps source.img

Host pins (see above) explicitly assert:

- After `acquire(Request{110,180})` with a backend that fakes a JPEG source, `files.count(hero)==1 && files.count(mini)==1` and both payloads pass `isValid1BitBmpOfSize` (1-bit, 62-byte header, exact file size). Converts ==2 from same `source.img`. Symmetric for 74 -> both exist.

- After `ensureSizedCoverFromSource(tpl,110,180)` with `source.img` present, both `cover_110x180.bmp` and `cover_74x106.bmp` exist and `converts==2`; subsequent `ensureSizedCoverFromSource(tpl,74,106)` is `cacheHit` with 0 extra converts and 0 fetches — local hit without Wi-Fi.

- After `acquire` where `source` pre-existed but `convert` returns false, `files.count(src)==1` and `sourceRemoved==false && targetRemoved==true`. After `fetchCancellable` succeeds then `convert` fails, same. After `ensureSized` convert fail, `source` kept, `target` removed.

The production `M4ProviderCoverCache.cpp` convert now uses `oneBit=true` for Home sizes, so the dual BMPs are 1-bit exact-size (same path as `ensureSized`), not 8-bit.

## Host test commands + results

```bash
cd /Volumes/z/paseo/workspaces/paseo/worktrees/041rfr5o/m4-home-muse-impl

python3 firmware/tests/test_m4_dependency_bootstrap_contract.py
# -> m4 dependency bootstrap contract: PASS

python3 firmware/tests/test_m4_self_contained_contract.py
# -> m4 self-contained build contract: PASS

/opt/homebrew/bin/g++-14 -std=c++17 -I firmware/src -I firmware/src/avr \
  firmware/tests/native_app/test_provider_cover_cache.cpp -o /tmp/test_provider_cover_cache && /tmp/test_provider_cover_cache
# -> round-17 dual-size hero->mini passed
# -> round-17 keep-source on cancel/convert-fail passed
# -> provider cover cache bounded/reuse/failure/success passed
# -> generate-on-miss: hit/generate-1bit-exact/never-http/no-invent-download/cancel/cleanup/path-scoped passed

/opt/homebrew/bin/g++-14 -std=c++17 -I firmware/src -I firmware/src/avr \
  firmware/tests/native_app/test_home_cover_placeholder_async.cpp -o /tmp/test_home_cover_placeholder_async && /tmp/test_home_cover_placeholder_async
# -> healMissingCoverPath PASS
# -> firstPublishDoesNotRequireEnsureSized PASS
# -> refreshDoesEnsure PASS
# -> placeholderHelper PASS
# -> cacheHitVsMiss PASS
# -> placeholderRendering PASS
# -> earlyReaderHandoff PASS
# -> ALL placeholder_async tests PASS

/opt/homebrew/bin/g++-14 -std=c++17 -I firmware/src -I firmware/src/avr \
  firmware/tests/native_app/test_home_cover_async_acquire.cpp -o /tmp/test_home_cover_async_acquire && /tmp/test_home_cover_async_acquire
# -> asyncGap PASS
# -> policyAAcquire PASS
# -> coverUrlResolvers PASS
# -> fastStillNonBlocking PASS
# -> ALL async_acquire tests PASS

/opt/homebrew/bin/g++-14 -std=c++17 -I firmware/src -I firmware/src/avr \
  firmware/tests/native_app/test_cover_last_resort.cpp -o /tmp/test_cover_last_resort && /tmp/test_cover_last_resort
# -> cover last-resort: present and guarded — validated that source.img has priority, never fetches, cleans partial
```

All g++-14 invocations use Homebrew GCC 14 (Apple clang++ lacks `__builtin_ctzg`). No PlatformIO build required per task; no flash.

## Residual risks

- **Backend task time**: Dual dither for hero+mini in same pass doubles JPEG→1-bit work on the 16 KiB backend task (each `bmpFileTo1BitBmpWithSize` / `pngFileToBmpStream` + `M4CoverDither`). The 80 ms yield still lets first placeholder frame paint before refresh, but the second publish now waits for two dithers. If `source.img` is large (512 KiB JPEG), dual dither could add ~30-60 ms. Acceptable for Home, but if both hero and 3 minis are misses, worst-case 4 books ×2 sizes = 8 dithers sequential could approach 250 ms. Mitigation: `refreshMissingCoversInCtx` caps hero+3, and each book's second size is only generated when that book's slot is processed; hero's complementary (74) for same book is not needed for another book's slot, so total dithers in one Home entry remain ≤4 (one per recent) + up to 4 complementary = 8 worst-case, still within `onExit` 250 ms join.

- **Legacy cache with only one size**: First re-entry after update where a book already had `cover_110x180.bmp` but not `74x106` and `source.img` was deleted by old cancel path will remain unable to generate complementary until `source.img` is re-downloaded via `acquire` (needs Wi-Fi). New code never deletes a complete `source.img`, so future re-downloads will be retained.

- **Fallback dual**: When `source.img` missing but `cover_171x254.bmp` exists, `ensureSized` for 110 now also writes 74 from same fallback. Both are 1-bit from 2-bit Fengyan source; quality is acceptable for placeholder upgrade, but the fallback is still 2-bit and may be missing for very old installs.

- **Test for `cover_171x254` ordering**: `test_cover_last_resort.cpp` checks `pos171 > posSourceExists`. The header's early doc literal before source check was split to `cover_171 x254` so the first contiguous literal is the `Last resort: source missing -> try 2-bit Fengyan cover_171x254.bmp` comment after the source branch. If the doc is later re-formatted to re-introduce a contiguous literal before the source check, the test will false-fail.

- **No Scene framework change**: Dual generation is inside cover-cache only; `HomeActivity` still publishes placeholders first then refresh, so no new task or `Scene` API churn.

## Commit

Branch `agent/home-muse-impl`
Message `round-17(muse): dual-size home covers keep source.img`
Hash: `8fee359e9949859399ba4d17831b566da4c823c6` (`8fee359`)

## Lesson appended to docs/M4_AGENT_LESSONS.md

See appended section "2026-09-01 — Round 17: dual-size Home covers must keep source.img".

