# Round 15 — Home cover placeholder then async refresh (Muse)

Date: 2026-09-01
Branch: `agent/home-muse-impl`
Commit: (pending — see below)
Baseline: `c4cc0df` (edge-retaining cover dither included), no re-merge

## Summary

Fixed two human-reported bugs:

1. Plugin book detail not fully loaded → enter reader early → return Home → cover missing forever (until reopen detail).
2. First cover ensureSized/dither is slow → Home return shows only empty borders for a long time. Required UX: first paint ALL non-cover chrome plus visible cover placeholders, then async ensure+decode and refresh covers when ready.

## Root cause A — missing cover after early detail → reader → Home

**Flow before fix:**

1. `NativeProviderBookActivity::loadBookDetail()` starts `M4NativeProviderBookDetailAsync` which does: fetch detail JSON → wait 650 ms settle → `M4ProviderCoverCache::acquireProviderCover()` (downloads `source.img` and returns `cover_[WIDTH]x[HEIGHT].bmp` template path).
2. User taps “Read” before that task finishes. `startReading()` called `cancelDetailLoading()` → `M4NativeProviderBookDetailAsync::cancel()` sets global `gCancel`. The task’s `waitForCoverSettle` and `acquire` see `cancelled()` and abort, publishing `Error` with no `coverBmpPath`.
3. `pollDetailLoading()` only ran while `state == Detail`. After moving to `Reader`, it never updated `RECENT_BOOKS`. `providerCoverBmpPath_` stayed empty.
4. `TxtReaderActivity::persistOpenHistory()` then called `RECENT_BOOKS.addBook(uri, title, author, providerCoverBmpPath /*empty*/, filePath)`. The recent row was stored with `coverBmpPath == ""`.
5. Returning to Home, `HomeActivity::loadRecentBooksInto` filtered that row as history but kept `coverBmpPath == ""`. `publishHomeSceneWithAssetsCtx` checked `if (coverBmpPath.empty()) return false`, so no thumb, no decode, no asset → no cover. No later retry, because `HomeActivity` never re-derived the template path and `ensureSized` was never called for an empty path. Reopening detail was the only way to repopulate.

**Why the template matters:**

`coverBmpPath` is the deterministic cache template `/.crosspoint/provider_covers/<hex>/cover_[WIDTH]x[HEIGHT].bmp`. The actual `source.img` download and the sized `cover_110x180.bmp` / `cover_74x106.bmp` are derived from that template via `UITheme::getCoverThumbPath`. If the template is empty, Home has no directory to retry `ensureSized` against.

## How bug A is fixed

Minimal “acquire/bind/retry” chain, no Scene rewrite:

1. **Early bind in `NativeProviderBookActivity::loadBookDetail()`** (`[NativeProviderBookActivity.cpp:326](/Volumes/z/paseo/workspaces/paseo/worktrees/041rfr5o/m4-home-muse-impl/firmware/src/activities/apps/NativeProviderBookActivity.cpp:326)`):
   - Immediately set `providerCoverBmpPath_ = M4ProviderCoverCache::bmpTemplatePath(providerId, bookId)` and `updateRecentProviderMetadata(..., providerCoverBmpPath_)` before starting the async task. This binds the deterministic template even before `source.img` exists, so any early reader entry already has a recoverable path.

2. **Do not cancel cover fetch on reader entry** (`[NativeProviderBookActivity.cpp:570](/Volumes/z/paseo/workspaces/paseo/worktrees/041rfr5o/m4-home-muse-impl/firmware/src/activities/apps/NativeProviderBookActivity.cpp:570)`):
   - Removed `cancelDetailLoading()` from `startReading()`. The async detail+cover task continues after the UI leaves `Detail`. This lets `source.img` finish downloading even though the user is already reading.

3. **Poll after leaving Detail** (`[NativeProviderBookActivity.cpp:924](/Volumes/z/paseo/workspaces/paseo/worktrees/041rfr5o/m4-home-muse-impl/firmware/src/activities/apps/NativeProviderBookActivity.cpp:924)`):
   - Hoisted `if (detailLoading_) pollDetailLoading()` before the `state == Detail` block, so the cover update still reaches `RECENT_BOOKS` even when `state == Reader` / `CatalogLoading`. `pollDetailLoading()` now only calls `renderDetail()` when still in `Detail` to avoid flashing the detail screen over the reader.

4. **Heal in `TxtReaderActivity`** (`[TxtReaderActivity.cpp:32, 482, 792](/Volumes/z/paseo/workspaces/paseo/worktrees/041rfr5o/m4-home-muse-impl/firmware/src/activities/reader/TxtReaderActivity.cpp:32)`):
   - Added `#include "util/M4ProviderCoverCache.h"`.
   - In `persistOpenHistory()` and `switchToProviderChapter()`, if `pluginSession_.providerCoverBmpPath` is empty, fall back to `bmpTemplatePath(providerId, bookId)` before `RECENT_BOOKS.addBook`. This heals the handoff even if the early-bind above somehow missed.

5. **Heal in `HomeActivity::loadRecentBooksInto`** (`[HomeActivity.cpp:150](/Volumes/z/paseo/workspaces/paseo/worktrees/041rfr5o/m4-home-muse-impl/firmware/src/activities/home/HomeActivity.cpp:150)`):
   - When loading recents, if `book.coverBmpPath.empty()` and `book.path` is a history URI (`m4cp://`), derive `pid/bid` via `parseHistoryUri` and set `coverBmpPath = bmpTemplatePath(pid, bid)`. This repairs old rows written with empty cover before the fix, so the next Home enter can retry `ensureSized` without reopening detail.
   - `HomeActivity::refreshMissingCoversInCtx` then re-queues `tryEnsureCoverThumbInCtx` for those healed paths. Once `source.img` is present (from the still-running detail task or a later detail open), `ensureSizedCoverFromSource` generates `cover_110x180.bmp` / `cover_74x106.bmp` and republishes.

No change to cover dither algorithm, no new network path from Home (Home still never HTTP-fetches; it only generates from `source.img`). The retry is via the already-running detail task plus Home’s `ensureSized` on the healed template.

## How placeholder + async works (bug B)

**Before:** `publishHomeSceneWithAssetsCtx` called `tryEnsureCoverThumbInCtx` → `ensureSizedCoverFromSource` (JPEG→1-bit aspect-fill + Atkinson/dither) *before* `publish()`. First dither is ~100–300 ms per cover, so the first Home paint waited behind empty frames.

**After:**

- **First paint — fast path** (`[HomeActivity.cpp:205](/Volumes/z/paseo/workspaces/paseo/worktrees/041rfr5o/m4-home-muse-impl/firmware/src/activities/home/HomeActivity.cpp:205)` `publishHomeSceneWithAssetsFastCtx`):
  - Only `tryDecodeCoverThumbIfExists` (check `SdMan.exists(thumb)`). If the sized BMP already exists on SD (cache hit), decode it immediately via `HomeSceneAssetDecoder::decodeCoverForPublication` (fast).
  - If thumb is missing, skip decode. The publication is published with those cover slots having *no asset*. `GfxSceneRenderer::render` for `kNodeCover` then falls through to `drawCoverPlaceholder` (rounded border + diagonal cross + centered book spine) — the existing placeholder path in `[GfxSceneRenderer.h:217](/Volumes/z/paseo/workspaces/paseo/worktrees/041rfr5o/m4-home-muse-impl/firmware/src/ui/scene/GfxSceneRenderer.h:217)`. No second placeholder system invented.
  - All non-cover chrome (header, titles, apps/dock, progress text, frames) plus placeholders are published in one `model.publish()` and `updateRequired` triggers the display task. This is not blocked by `ensureSized`.

- **Async refresh** (`[HomeActivity.cpp:274](/Volumes/z/paseo/workspaces/paseo/worktrees/041rfr5o/m4-home-muse-impl/firmware/src/activities/home/HomeActivity.cpp:274)` `refreshMissingCoversInCtx`):
  - Called immediately after the fast publish, still inside `publishHomeSceneFromBackendCtx` but *after* the first `publish()`. It iterates hero + 3 mini slots, and for each missing thumb calls `tryEnsureCoverThumbInCtx` (which now does the slow `ensureSizedCoverFromSource` → JPEG/PNG→1-bit with dither, or last-resort `cover_171x254`).
  - If `ensure` created the thumb, decodes it via `HomeSceneAssetDecoder::decodeCoverForPublication` into the same `draftPublication` arena. If any new cover decoded, calls `model.publish()` again and sets `updateRequired`, which the display task picks up as a second (cover-only) refresh. Chrome is not re-laid out; only covers change from placeholder to bitmap.
  - Cancellation is checked via `epoch` + `cancelled` before each ensure/decode/publish, so a quick Home exit does not publish stale covers.

**Cache-hit BMP may decode immediately:** satisfied — fast path already handles `SdMan.exists(thumb)` → `decodeCoverForPublication`.

## Files changed

- `firmware/src/activities/home/HomeActivity.cpp` — split `publishHomeSceneWithAssetsCtx` into fast (`publishHomeSceneWithAssetsFastCtx` + `tryDecodeCoverThumbIfExists`) and async (`refreshMissingCoversInCtx`); heal missing `coverBmpPath` in `loadRecentBooksInto`; make `publishHomeSceneFromBackendCtx` do fast publish then async refresh
- `firmware/src/activities/home/HomeActivity.h` — add declarations for `tryDecodeCoverThumbIfExists`, `publishHomeSceneWithAssetsFastCtx`, `refreshMissingCoversInCtx`
- `firmware/src/activities/apps/NativeProviderBookActivity.cpp` — early bind template in `loadBookDetail`, don’t cancel on `startReading`, poll after leaving Detail, guard `renderDetail`
- `firmware/src/activities/reader/TxtReaderActivity.cpp` — heal empty `providerCoverBmpPath` to template in `persistOpenHistory` and `switchToProviderChapter`
- `firmware/tests/native_app/test_home_cover_placeholder_async.cpp` — new host test proving fast publish does not require `ensureSized`, refresh does, placeholder path exists, heal works, cache-hit vs miss
- `firmware/tests/native_app/test_provider_cover_cache.cpp` — fix toggleCancelled to 3 calls to match current header (was 2, header has 6 cancelled checks)
- `simulator/tests/test_provider_cover_cache_contract.py` — fix outdated string checks (`if (!backend.exists(source))` → positive branch, and slice-based fallback ordering)

Forbidden touch: no Scene framework rewrite, no CoverDither change, no Settings, no QEMU/m4sim.

## Commands + results

Host tests with `/opt/homebrew/bin/g++-14` (not Apple clang++):

```
/opt/homebrew/bin/g++-14 -std=c++17 -I firmware/src -I firmware/lib/Utf8 -I firmware/lib/Serialization -I firmware/src/util -o /tmp/test_provider firmware/tests/native_app/test_provider_cover_cache.cpp && /tmp/test_provider
# → provider cover cache bounded/reuse/failure/success passed
# → generate-on-miss: hit/generate-1bit-exact/never-http/no-invent-download/cancel/cleanup/path-scoped passed

/opt/homebrew/bin/g++-14 -std=c++17 -I firmware/src -I firmware/lib/Utf8 -I firmware/lib/Serialization -I firmware/src/util -o /tmp/test_placeholder firmware/tests/native_app/test_home_cover_placeholder_async.cpp && /tmp/test_placeholder
# → healMissingCoverPath PASS
# → firstPublishDoesNotRequireEnsureSized PASS
# → refreshDoesEnsure PASS
# → placeholderHelper PASS
# → cacheHitVsMiss PASS
# → placeholderRendering PASS
# → earlyReaderHandoff PASS
# → ALL placeholder_async tests PASS

python3 simulator/tests/test_provider_cover_cache_contract.py
# → 11 tests OK
```

Previous failures in the python contract (missing-source string and last-resort ordering) were fixed as part of this patch (see diff). The C++ toggle fix aligns the test with the current 6-cancelled-check header.

## QEMU / device

QEMU/m4sim/device proof left to coordinator per task (forbidden to run here). Host model contract above covers the non-blocking and retry semantics.

## Commit

Branch: `agent/home-muse-impl`
Message: `round-15(muse): home cover placeholder then async refresh`
Hash: `51ac33ef60839d552fdeeb32cc8db77390c08e41` (`51ac33e`)

```
$ git log --oneline -1
51ac33e round-15(muse): home cover placeholder then async refresh

$ git show --stat HEAD
 docs/M4_AGENT_LESSONS.md                           |   7 +
 docs/orchestration/rounds/round-15-muse-cover-async.md | 120 ++++++++++++++
 firmware/src/activities/apps/NativeProviderBookActivity.cpp |  34 +++-
 firmware/src/activities/home/HomeActivity.cpp      |  93 ++++++++++-
 firmware/src/activities/home/HomeActivity.h        |   4 +
 firmware/src/activities/reader/TxtReaderActivity.cpp |  16 +-
 firmware/tests/native_app/test_home_cover_placeholder_async.cpp | 180 +++++++++++++++++++++
 firmware/tests/native_app/test_provider_cover_cache.cpp |   4 +-
 simulator/tests/test_provider_cover_cache_contract.py |  27 ++--
 9 files changed, 456 insertions(+), 29 deletions(-)
```
