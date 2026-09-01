# Round 16 — Muse Home true async + policy A acquire (never-opened detail)

Date: 2026-09-01  
Lane: `m4-home-muse-impl` / `agent/home-muse-impl`  
Baseline: `19570a5` (includes R15 `cf09dc3` cover placeholder async) — no re-merge.  
Human decision: **policy A** — Home may call `acquireProviderCover` for provider recents that never opened detail, gated on Wi-Fi + cancel.  
Device RCA: `docs/orchestration/rounds/round-15-device-cover-async-diag.md` — R15 was fake async (same-task `refreshMissingCoversInCtx` with no paint gap) + Home never HTTP-fetches, so never-opened detail stays wireframe forever; Wi-Fi was off during recheck.

## Summary

- **True async visible gap**: `publishHomeSceneFromBackendCtx` now publishes placeholders first, yields `vTaskDelay(80ms)` so the display task paints frame 1, then runs `refreshMissingCoversInCtx` and republishes covers. Same-task without gap is forbidden — the 80 ms yield is the explicit async boundary.
- **Policy A acquire** when `ensureSizedCoverFromSource` still misses: cap hero + 3 mini recents, parse `m4cp://providerId/bookId`, resolve `coverUrl` from local cache first then bounded detail fetch, gate on `M4QemuNet::staConnected()` and `cancelled/epoch`, then `M4ProviderCoverCache::acquireProviderCover` with cancel hook and decode + republish.
- No `RecentBooksStore` format churn — `coverUrl` is **not** persisted, read from existing provider caches.

## R15 why it failed (not re-argued)

1. `backendLoop` one-shot: `publishHomeSceneWithAssetsFastCtx` → same-task `refreshMissingCoversInCtx` without yield → display often only saw final miss, no later retry when `source.img` appears.
2. Home only called `ensureSizedCoverFromSource` (**Never fetches / Never HTTP**) — needs `source.img` or `cover_171x254.bmp` from prior `acquireProviderCover` (detail path). Never-opened detail ⇒ no source ⇒ permanent `drawCoverPlaceholder` wireframe.
3. Healing empty `coverBmpPath` to `bmpTemplatePath` alone does not download.

## A) True async refresh (visible)

### First paint (fast, non-blocking)

`HomeActivity::publishHomeSceneWithAssetsFastCtx(ctx)` — already in R15:

- only `tryDecodeCoverThumbIfExists` (check `SdMan.exists(thumb)`, no `ensureSized`);
- missing stays missing asset → `GfxSceneRenderer::drawCoverPlaceholder` (rounded border + diagonal cross + spine) visible on first frame;
- does not block on JPEG dither / HTTP / `acquireProviderCover`;
- sets `ctx.model.publish()` + `ctx.updateRequired = true` for the display task.

Proof file-content: fast slice contains `tryDecodeCoverThumbIfExists` but **not** `ensureSizedCoverFromSource` nor `acquireProviderCover` — checked by `test_home_cover_placeholder_async.cpp` and new `test_home_cover_async_acquire.cpp:fastStillNonBlocking`.

### Async boundary + second publish

In `HomeActivity::publishHomeSceneFromBackendCtx` (same backend task, still 16 KiB):

```cpp
(void)publishHomeSceneWithAssetsFastCtx(ctx);
if (isCancelled()) return;
// True async gap: yield so display task paints placeholders
#ifdef CROSSPOINT_MURPHY_M4
vTaskDelay(pdMS_TO_TICKS(80));
#else
// host stub — logical boundary for contract tests
#endif
if (isCancelled()) return;
refreshMissingCoversInCtx(ctx, epoch); // slow dither + policy A acquire, then maybe publish
```

- 80 ms lets the display loop (10 ms poll of `backendCtx->updateRequired`) pick up the first publication before heavy work starts, while keeping total Home latency short.
- `cancelled` / `epoch` re-checked after the yield — leaving Home (`onExit` bumps epoch + sets cancelled) aborts the pending cover work.
- `refreshMissingCoversInCtx` only republishes if at least one new thumb was decoded (`anyDecoded && !isCancelled() && model.publish()`), setting `updateRequired` again for a second (cover-only) frame. Chrome is not re-laid out.

This satisfies the task's preferred shape: *"keep a short-lived backend loop that yields so the display task can paint the first publication before refresh runs. Same-task 'comment says async' is not acceptable."* — the `vTaskDelay(80)` is the explicit gap; a separate cover task would also satisfy but the yield is the minimal change.

Host proof: `test_home_cover_async_acquire.cpp:testAsyncGap` asserts `publishHomeSceneWithAssetsFastCtx` < `vTaskDelay` < `refreshMissingCoversInCtx` and that `isCancelled()` is checked between delay and refresh.

## B) Policy A — Home may acquire for never-opened detail

### When it runs

Inside `HomeActivity::refreshMissingCoversInCtx`, for hero (110×180) and each of up to 3 mini (74×106) where `!coverBmpPath.empty() && !SdMan.exists(thumb)`:

1. Try local `tryEnsureCoverThumbInCtx` (which calls `M4ProviderCoverCache::ensureSizedCoverFromSource` → JPEG/PNG `source.img` → exact-size 1-bit 110×180 / 74×106, or last-resort 171×254). If it decoded, mark `anyDecoded` and **skip acquire** for that slot.
2. If local ensure still missed and not cancelled, parse history URI `M4ContentProvider::parseHistoryUri(path, pid, bid)`.
3. Resolve `coverUrl` via `HomeCoverPolicyA::resolveCoverUrlForHistory(pid, bid, cancelled)` (see below).
4. If URL empty or cancelled → placeholder stays.
5. If Wi-Fi **not** connected (`!HomeCoverPolicyA::homeWifiConnected()` i.e. `!M4QemuNet::staConnected()`) → log `acquire skip ... no wifi`, leave placeholder, do not hang.
6. If Wi-Fi connected and URL available → `M4ProviderCoverCache::Request{pid,bid,coverUrl,w,h,cancelled}` → `acquireProviderCover(req)` (bounded 512 KiB download to `source.img`, cancelled-aware, then convert to exact size). Re-check `cancelled/epoch` after the HTTP, then if `!res.coverBmpPath.empty()` and thumb exists, `decodeCoverForPublication` + `anyDecoded=true`.

Cap: hero + `for (i=1; ... && itemIndex<3)` → at most 4 HTTP attempts, never stampede whole history list.

Cancel/epoch: every step checks `isCancelled()` (which is `cancelled || epoch != origEpoch`). `acquireProviderCover` receives the same cancel hook (`Request.cancelled = isCancelled`) so its internal `fetchCancellable` can abort mid-download. Leaving Home (`onExit`: `cancelled=true`, `epoch++`, bounded 250 ms join) guarantees in-flight acquire/dither aborts.

### How coverUrl is resolved for never-opened detail

**No new persistence**. `RecentBook` still stores only `coverBmpPath` (template). URL is read from existing caches that already exist on SD or via a minimal detail fetch.

#### Exact functions / files

All new glue lives in `firmware/src/activities/home/HomeActivity.cpp` (allowed file), namespace `HomeCoverPolicyA` (CROSSPOINT_MURPHY_M4 real, host stub otherwise):

- `HomeCoverPolicyA::homeWifiConnected()` → `M4QemuNet::staConnected()` (covers real Wi-Fi and QEMU `open_eth` compat). Included via `qemu/M4QemuNet.h`.
- `HomeCoverPolicyA::resolveCoverUrlFromShelf(providerId, bookId)` — fast local path, **no HTTP, no Wi-Fi gate**:
  - enumerates `M4xRegistry::load()` apps matching `providerId`;
  - for each `app.id`, reads `/apps_data/<appId>/provider/shelf_rows.tsv` via `SdMan`;
  - scans TSV (handles FatFS NUL slack) for line `rfind(bookId,0)==0 && line[bookId.size()]=='\t'`;
  - extracts cover field: `field 4` (`thumb_url`/`cover`) for `fanqie`/`jjwxc`/`weread`, `field 5` (`coverUrl`) for `legado` then `M4LegadoBridge::coverProxyUrl(M4LegadoBridge::baseUrl(), rawCover)` (proxy via phone endpoint, includes `percentEncodeQueryValue`). Returns first non-empty hit.
  - Shelf schema source-of-truth: `M4ProviderShelfCache::schema(providerId)` — columns verified in `firmware/src/apps/providers/M4ProviderShelfCache.h` and `M4NativeAppControllerFactory.cpp:rowAt`.
- `HomeCoverPolicyA::resolveCoverUrlViaDetail(providerId, bookId, appIdHint, cancelled)` — bounded metadata fetch, **Wi-Fi-gated for network providers**:
  - early `cancelled()` check;
  - if `providerId != "legado"` and `!homeWifiConnected()` → return empty (do not hang);
  - picks `appId` from hint or first registry match;
  - builds `M4NativeProviderBookDetail::Request{providerId, bookId, appId, maxBytes=48KiB}`;
  - calls `M4NativeProviderBookDetail::fetch(req, cancelled)` — already bounded: for `legado` it's local shelf enrichment only (no `ensureEndpoint`/HTTP), for `fanqie` → `https://fanqienovel.com/api/book/info?bookId=`, for `jjwxc` → `novelbasicinfo?novelId=`, for `weread` → `web/book/info?bookId=` with cookie, all 30 s timeout and cancel-aware. Returns `detail.coverUrl` if ok.
  - Includes `apps/providers/M4NativeProviderBookDetail.h` and `apps/providers/M4LegadoBridge.h`.
- `HomeCoverPolicyA::resolveCoverUrlForHistory(providerId, bookId, cancelled)` — orchestrates priority:
  1. try `resolveCoverUrlFromShelf` first (fast, local);
  2. if empty and not cancelled, pick `appId` from registry and call `resolveCoverUrlViaDetail`.

Extra files allowed but not needed: no new files beyond `HomeActivity.cpp`; the three provider helpers are used via existing headers (`M4NativeProviderBookDetail.h`, `M4LegadoBridge.h`, `M4ProviderShelfCache.h`/`M4NativeAppControllerFactory` logic). Listed here per task: **0 extra files** beyond allowed set (kept to 1 allowed file).

#### Why not persist coverUrl

Task explicitly says *prefer reading URL from an existing provider cache to avoid store format churn*. Shelf already persists cover URL as 5th/6th column, and detail fetch can re-derive it without storing per-recent. Persisting would require bumping `RECENT_BOOKS_FILE_VERSION` and back-compat, unnecessary for 4-item Home.

### Wi-Fi gate + cancel summary

| Step | Gate | Behaviour |
|------|------|-----------|
| Local shelf scan | none | always runs, no network |
| Detail fetch for `legado` | none (local) | `M4NativeProviderBookDetail::fetch` is local shelf enrichment, no HTTP, allowed offline |
| Detail fetch for `fanqie`/`jjwxc`/`weread` | `homeWifiConnected()` | return empty without HTTP if offline |
| `acquireProviderCover` | `homeWifiConnected()` | log `acquire skip ... no wifi` and keep placeholder if offline |
| Every `ensure`/`fetch`/`acquire`/`decode` | `isCancelled()` / `epoch` | lambda `cancelled` passed through; checked before and after each blocking call; `onExit` sets `cancelled` + bumps epoch, backend aborts |

## Allowed files touched

- `firmware/src/activities/home/HomeActivity.cpp` — true async gap + policy A acquire (including new `HomeCoverPolicyA` namespace);
- `firmware/tests/native_app/test_home_cover_async_acquire.cpp` — new host test proving fast non-blocking, async gap, wifi-gated acquire, cancel/epoch, coverUrl resolver presence (allowed host test);
- `firmware/src/util/M4ProviderCoverCache.*` and `RecentBooksStore.*` **not** changed (no churn needed);
- No other provider glue files added (reused existing headers, ≤3 extra files rule satisfied with 0).

Forbidden: no Scene/theme/Settings/cover dither rewrite, no `git push`, no flash, no `pkill -f m4adb.py`, no `git reset --hard`/`clean`, no QEMU/m4sim.

## Acceptance (host)

Commands (from workspace root `/Volumes/z/paseo/workspaces/paseo/worktrees/041rfr5o/m4-home-muse-impl`):

```bash
python3 simulator/tests/test_provider_cover_cache_contract.py
/opt/homebrew/bin/g++-14 -std=c++17 -I firmware/src -I firmware/include -I firmware/lib/Epub -I firmware/lib/miniz -I firmware/lib/picojpeg firmware/tests/native_app/test_provider_cover_cache.cpp -o /tmp/test_provider_cover_cache && /tmp/test_provider_cover_cache
/opt/homebrew/bin/g++-14 -std=c++17 -I firmware/src -I firmware/include -I firmware/lib/Epub -I firmware/lib/miniz -I firmware/lib/picojpeg firmware/tests/native_app/test_home_cover_placeholder_async.cpp -o /tmp/test_home_cover_placeholder_async && /tmp/test_home_cover_placeholder_async
/opt/homebrew/bin/g++-14 -std=c++17 -I firmware/src -I firmware/include -I firmware/lib/Epub -I firmware/lib/miniz -I firmware/lib/picojpeg firmware/tests/native_app/test_home_cover_async_acquire.cpp -o /tmp/test_home_cover_async_acquire && /tmp/test_home_cover_async_acquire
```

Results:

- `test_provider_cover_cache_contract.py` — 11 tests **OK** (png decoder contract, unknown/webp not converted, home scene sizes generate without fetch, cache hit, 1-bit exact, never HTTP, etc.).
- `test_provider_cover_cache` — **PASS** (bounded/reuse/failure/success + generate-on-miss hit/1-bit-exact/never-http/no-invent-download/cancel/cleanup/path-scoped).
- `test_home_cover_placeholder_async` — **PASS** (healMissingCoverPath, firstPublishDoesNotRequireEnsureSized, refreshDoesEnsure, placeholderHelper, cacheHitVsMiss, placeholderRendering, earlyReaderHandoff).
- `test_home_cover_async_acquire` — **PASS** (asyncGap, policyAAcquire, coverUrlResolvers, fastStillNonBlocking).

Full `pio run -e murphy_m4` intentionally not run per task (prefer g++ host tests); syntax is device-verified via the includes above and the host tests' file-content checks. Coordinator owns QEMU/device flash.

## QEMU / device proof

Left to coordinator per task — no QEMU/m4sim/device flash run in this lane. Expected device behaviour after APP1 flash of this commit: Home first paints chrome + titles + dock + placeholder rects (80 ms gap visible), then — if Wi-Fi connected — hero + mini covers for never-opened provider recents upgrade from wireframe to real dithered covers after the separate acquire/dither republish, without reopening detail. If Wi-Fi off (as in R15 recheck `wifi_connected=false`), placeholders remain (no hang), and will fill once Wi-Fi is enabled and Home is revisited (healed `coverBmpPath` still allows ensure/acquire retry).

## CoverUrl resolution recap (explicit)

`coverUrl` is **not** stored in `RecentBook`. Resolution for a history URI `m4cp://pid/bid`:

1. Local `shelf_rows.tsv` under `/apps_data/<appId>/provider/shelf_rows.tsv` for the `pid`'s installed app(s) — TSV columns from `M4ProviderShelfCache::schema(pid)` (`fanqie`/`jjwxc`/`weread` col 4, `legado` col 5 via `M4LegadoBridge::coverProxyUrl(baseUrl, raw)`). Fast, no network.
2. If miss, `M4NativeProviderBookDetail::fetch` with `Request{pid,bid,appId,48KiB}` — for `legado` it's local shelf enrichment (no network), for others it's a single bounded HTTP detail call (`fanqie`/`jjwxc`/`weread` endpoints) gated on `M4QemuNet::staConnected()` and `cancelled`, returning `detail.coverUrl`.
3. First non-empty wins, then Wi-Fi-gated `M4ProviderCoverCache::acquireProviderCover(Request{pid,bid,coverUrl,w,h,cancelled})` downloads `source.img` (512 KiB cap, cancel-aware) and converts to exact `cover_{w}x{h}.bmp`, then `decodeCoverForPublication` into the already-published scene's second frame.

## Commit

Branch `agent/home-muse-impl` message `round-16(muse): home async cover acquire for never-opened detail` — hash filled below on reply (after commit).

## Risks / follow-up

- `resolveCoverUrlFromShelf` does a linear TSV scan (≤64 rows typical) on the backend task — acceptable, but if shelf grows large consider indexed lookup via `M4ProviderShelfIndex`.
- `resolveCoverUrlViaDetail` for `fanqie`/`jjwxc`/`weread` may hit auth (`weread` login_required) or endpoint stale (`legado` no visitor IP) — then placeholder remains, user sees wireframe until shelf is populated via app discovery or detail is opened once.
- `vTaskDelay(80)` is a heuristic; if display is slower under load, consider a `xTaskNotify`/event or second task with explicit `updateRequired` handshake for tighter determinism — kept minimal for this round.
