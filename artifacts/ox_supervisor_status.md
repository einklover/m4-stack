# OX supervisor status

Last updated: 2026-08-24T09:45:00Z (all seven accepted+merged; combined-diff audit PASS)

Supervisor worktree: `/Volumes/z/paseo/workspaces/paseo/worktrees/0xdf4ldr/m4-ox-supervisor`
Supervisor branch: `agent/m4-ox-supervisor-integration`
Supervisor HEAD: `561afcf` + this commit (`fix(legado): treat Fanqie http_2xx_empty as Legado stale-shelf`)
Integrated-candidate baseline (read-only): `agent/m4-integrated-candidate` @ `2cdc112`

## Baseline correction

Worktrees were initially on `5d35547` (eink-browser-bridge-m3), which diverges from the integrated candidate. Audit anchors (compact-CJK 14px divisor, residual TXT catch-up after streaming first-open, READER_CLEANUP_REFRESH, bindSystemReader, etc.) only exist on `2cdc112`.

At 2026-08-24 ~08:45Z the supervisor reset `agent/m4-ox-supervisor-integration` and all seven OX branches to `2cdc112`. The original integrated-candidate worktree was not modified and remains at `2cdc112`.

Do not merge experimental TTF compression / Luna work (`agent/m4-font-visual-normalization` / unrelated).

## Agents

| # | Agent | Task | Worktree | Branch | Agent status (09:25Z) | Review | Merge |
|---|-------|------|----------|--------|------------------------|--------|-------|
| 1 | `ac930671-6967-4398-a464-1fb473951bc6` | system font sizing | `m4-fix-system-font-sizing` | `agent/fix-system-font-sizing` | idle after ACCEPT | **ACCEPT** `f78ee0e` | `e1112c6` |
| 2 | `0aaddc9d-db2e-4b8a-b60a-d63978395d7a` | reader settings handoff | `m4-fix-reader-settings-handoff` | `agent/fix-reader-settings-handoff` | idle after ACCEPT | **ACCEPT** `db57f9f` | `0bbe32d` |
| 3 | `f245f652-6ec4-4648-8d6f-bd63fc727297` | large TXT residuals | `m4-fix-large-txt-residuals` | `agent/fix-large-txt-residuals` | idle after ACCEPT | **ACCEPT** `ab3b7b4` on `55e966f` | `a426fb9` |
| 4 | `f4847209-b9e6-47fd-8f03-f8a2731c29ad` | Fanqie empty body/date | `m4-fix-fanqie-empty-body` | `agent/fix-fanqie-empty-body` | idle after ACCEPT (standing down) | **ACCEPT** `46dbacb` | `f5a27a0` |
| 5 | `96456397-b90b-4a5d-9d50-6b775aadb4fc` | Legado TOC consistency | `m4-fix-legado-toc` | `agent/fix-legado-toc` | idle after ACCEPT (stand down) | **ACCEPT** `296852b` | `561afcf` |
| 6 | `b1b353ac-eba4-4c02-89cb-45b5d4df5ce7` | plugin UI blocking | `m4-fix-plugin-blocking` | `agent/fix-plugin-blocking` | idle after ACCEPT (stand down) | **ACCEPT** `d0c95e3` | `627fbaa` |
| 7 | `8b42bcef-af96-4712-8789-2cea7e3b6d5e` | reader LUT waveform | `m4-fix-reader-lut` | `agent/fix-reader-lut` | idle after ACCEPT | **ACCEPT** `97067e7` | `6365d4c` |

## Merge log

| OX | Source commit | Review | Host/static checks | Supervisor merge |
|----|---------------|--------|--------------------|------------------|
| B settings | `db57f9f` | ACCEPT. Child-exit forces `updateRequired=true`; settings onGoBack re-arms auto-turn; menu/settings use `pumpSubActivityFrame()`. | `test_reader_settings_handoff` contract.sh + g++-14 C++ PASS | `0bbe32d` |
| G LUT | `97067e7` | ACCEPT. Tests lock existing arm-before-activate / disarm-before-stock-seed / no FULL-HALF in anim / UI fast-only. Production LUT arming already on `2cdc112`; OX did not re-add it. | `simulator/tests/test_reader_lut_contract.py` 7/7 PASS (source + after merge) | `6365d4c` |
| A font | `f78ee0e` | ACCEPT. Compact-2bit `bindSystemReader` divides by raster px 14; canonical SD epdfont keeps 16; only NOTOSANS_16 rebound. | rebuilt `m4_scaled_epd_font_tests` from `f78ee0e`: PASS (16px compact advance=16 via 16/14; 14px unity) | `e1112c6` |
| C TXT | `55e966f` + `ab3b7b4` | ACCEPT after one correction. Bounded 8-page/256KiB catch-up; load failure ≠ EOF; picker/goToPercent cache-only; skip-fallback two-probe with non-const load trampoline and RAM-visible check; loadPage log gated. First commit mixed batch/chapter units (rejected); follow-up `ab3b7b4` fixed const-cast + empty-batch return. | g++-14 `test_txt_index_policy` PASS | `a426fb9` |
| D Fanqie | `46dbacb` | ACCEPT. 2xx+0B → `http_2xx_empty`; GET-only one retry before `status!=0` guard; `chapterDateUtc` logs `nt=` source; UI map; no Date-header plumbing. | g++-14 `test_fanqie_empty_body` PASS (source + after merge) | `f5a27a0` |
| F plugin | `d0c95e3` (amended `42d8f48`) | ACCEPT after one correction. First claim wiped FileRows catalog before `toc_prefetched`. Follow-up honors prefetch before wipe: FileRows keeps `chapter_catalog`+virtual rows and skips fetch; JSON uses `cached_toc`; miss path wipes then one download hop; `ensure_network` isolated; prefetch one hop/tick. JJWXC/`api.lua`/`tools/` clean. | Independent path assertions both `main.lua` PASS (honor_before_wipe, wipe_only_on_miss, filerows_no_fetch, connect_isolated, json_cache_roundtrip). No lupa/luac in this environment. | `627fbaa` |
| E Legado | `296852b` on `20e01e9` + `5d4d3c8` | ACCEPT after three corrections. Policy header + ensureBook clamp + present-only loadPage + legado-only stale remap + followRedirects. Path double-prefix rejected; PlaceholderThenFull hollow Ready rejected; leftover skeleton `toc_rows.txt` on stale Error rejected (retry would poison the cache guard). Final: `countTocRows(finalPath)`; stale publishes `legado_shelf_stale` then `SdMan.remove`s placeholders; cached-TOC keepPartialOnFail without delete. | g++-14 `test_legado_toc_policy` PASS (source + after merge + after supervisor `http_2xx_empty` integration) | `561afcf` |

## Remaining (not yet mergeable)

None. All seven OX branches accepted and merged. Experimental TTF compression / Luna (`agent/m4-font-visual-normalization`) was not merged.

## Combined-diff audit (vs `2cdc112`)

Shared production file: only `NativeProviderBookActivity.cpp` (D `chapterErrorText` `http_2xx_empty` + E `catalogErrorText` `legado_shelf_stale`) — merge was clean, both strings present.

Interaction matrix:
- Font vs settings: disjoint (`EpdFontLoader`/`M4FontPolicy` vs EPUB `updateRequired` / `pumpSubActivityFrame`).
- LUT vs refresh: LUT contracts 7/7 PASS; settings does not change waveform selection.
- Large-TXT vs settings handoff: disjoint readers (`TxtReader*` vs `EpubReader*`).
- Provider/plugin network: Fanqie firmware empty-2xx retry is host-side; plugin cooperative TOC does not reclassify transport.

Supervisor-owned tiny integration (after both D and E landed):
- `isStaleShelfFetch(!ok, "http_2xx_empty")` is true so Legado empty-2xx catalog fetches remap to `legado_shelf_stale` (otherwise they would miss the stale-shelf path after Fanqie made 2xx+0B `ok=false`).
- `catalogErrorText` maps leftover non-Legado `http_2xx_empty` instead of showing the raw code.

Host/static regressions after integration (g++-14, no device/QEMU/ADB/pio):
- `test_legado_toc_policy` PASS (incl. `http_2xx_empty`)
- `test_fanqie_empty_body` PASS
- `test_txt_index_policy` PASS
- `test_progressive_catalog` PASS
- `test_jjwxc_catalog_shape` PASS
- `test_progressive_http_state` PASS
- `test_reader_settings_handoff` C++ + contract.sh PASS
- `test_reader_lut_contract.py` 7/7 PASS
- plugin FileRows path assertions PASS
- `m4_scaled_epd_font_tests` PASS (16px compact advance=16 via 16/14)

## Direction notes / follow-ups (non-blockers)

- LUT wipe direction follows `SETTINGS.pageTurnAnimationDir` via `logicalToPhysicalAnimationDirection`, not turn sign (shipped intent). EPUB/XTC cleanup cadence not locked by the new tests.
- Font remaining risks (SSD1677 replay pin, device crispness) are out of this OX scope.
- Device compile not verified in these worktrees (open-m4-sdk absent). CI `m4-fast` should still be watched.

## Supervisor actions this cycle (09:38–09:45Z)

- Independently reviewed Legado `296852b`: remove-then-publish on stale PlaceholderThenFull only; cached-TOC untouched. ACCEPT and merged as `561afcf`. ACCEPT-and-stop sent to `96456397`.
- Combined-diff audit after all seven merges. Tiny supervisor integration: Legado-only `http_2xx_empty` stale remap + catalog UI map.
- Candidate worktree not modified (`2cdc112`). No ADB/QEMU/flash/device. Luna TTF compression not merged.
