# P2 Stacked Integration Audit

Status: **post-integration closeout evidence record**. Documentation/review only; this document adds no runtime or firmware logic.

Authoritative program source: #50 comment `5519324576`.

Operational sequence:

`P0 -> P1A -> P1B -> P1C -> P1D -> P2 -> integration audit -> gap review -> optional P1E only if a concrete gap remains -> future system-wide phase`

The older #50 P0-P10 design numbering is not the execution sequence. In particular, the older-design "P3 Wi-Fi event facade" is not treated as the next task.

## Final integration conclusion

The authorized P0-P2 stacked integration is complete. The six PRs were processed strictly in parent-to-child order:

`#52 -> #56 -> #58 -> #60 -> #62 -> #64`

For every layer, the live head/base, CI evidence, GitHub mergeability, and review threads were rechecked before the state transition. Each PR was marked Ready for review before merge. Each merge used GitHub's normal merge operation with the expected head SHA; no force update, required-check bypass, timeout inflation, or test masking was used.

The final integrated `main` commit is:

`e5517805f55a0ebd09d31b1b0498b4d1e4054e11`

This commit is the **recommended post-integration runtime baseline anchor**, subject to the exact-main verification note below.

## Final merge ledger

| Phase | PR | Integrated PR head | `main` base immediately before merge | Resulting `main` merge commit |
| --- | --- | --- | --- | --- |
| P0 | #52 | `b63d97863ac28908c1d05d4f66c2c10b1d29571e` | `b7dd293b761a96ea8be44486e82ed99173950bff` | `502b11fedfd21530d4db7e5ac43d777b50a2047c` |
| P1A | #56 | `1590ec27a96dedae1e3d54d13d3b507b7f850a37` | `502b11fedfd21530d4db7e5ac43d777b50a2047c` | `ebe969b1766d8cf1e193bf8a37f4e727ba03cdce` |
| P1B | #58 | `a6dd2c3ce93d807dcf482e19c9aa5d38fbd4e61a` | `ebe969b1766d8cf1e193bf8a37f4e727ba03cdce` | `5c9f39a428fb359c3c4705b98af44a9cd7a7df88` |
| P1C | #60 | `67004e454e6549915be0d79c8679eaef78a90db9` | `5c9f39a428fb359c3c4705b98af44a9cd7a7df88` | `477ae9bd3916b3ff3ee5e0e28819635cafe5f610` |
| P1D | #62 | `83471f4d0dbf22aa4a8dc8967319d4447297e070` | `477ae9bd3916b3ff3ee5e0e28819635cafe5f610` | `863d37e197ac11e6f37f628f79ec22e259295c04` |
| P2 | #64 | `14b14af1fb95d756815826d22be207cd6c3fd5bc` | `863d37e197ac11e6f37f628f79ec22e259295c04` | `e5517805f55a0ebd09d31b1b0498b4d1e4054e11` |

GitHub confirms `main` now points to `e5517805f55a0ebd09d31b1b0498b4d1e4054e11`, whose parents are the pre-P2 integrated `main` (`863d37e...`) and the final P2/audit head (`14b14af1...`).

## Integration behavior by layer

### P0 / #52

- Verified head: `b63d97863ac28908c1d05d4f66c2c10b1d29571e`.
- Successful phase evidence included Murphy production BIN runtime and progressive stream regressions.
- Existing `m4 fast firmware loop` / clean-BIN failures remained associated with the independently tracked #53 NetworkManager QEMU path.
- Before merge, GitHub reported `mergeable=true` with `mergeable_state=unstable`, not a merge conflict.
- Ready transition and normal merge succeeded.

### P1A / #56

- The earlier topology defect was corrected first: #56 was retargeted from `main` to P0 for clean stacked review.
- After #52 merged, #56 was retargeted again to the newly integrated `main` at `502b11fe...`.
- Head remained `1590ec27a96dedae1e3d54d13d3b507b7f850a37`.
- The P1A-only comparison remained 7 commits / 6 changed files.
- Successful production, Stage 14, and progressive-stream evidence remained attached; the known #53/clean-BIN red context was not altered.
- GitHub reported `mergeable=true`; Ready transition and normal merge succeeded.

### P1B / #58

- Integrated head: `a6dd2c3ce93d807dcf482e19c9aa5d38fbd4e61a`.
- Retargeted to the newly integrated `main` after #56.
- Production BIN, Stage 14, and progressive-stream runs were GREEN; existing #53-path failures remained separate.
- GitHub reported `mergeable=true`; Ready transition and normal merge succeeded.

### P1C / #60

- Verified GREEN implementation head: `84c14badd89eb4565f59a22b400a6712772541cd`.
- P1C gate run `33594742670`: `contracts` SUCCESS and `firmware-build` SUCCESS, including both `murphy_m4` and `murphy_m4_qemu_plugin` builds.
- Integrated PR head: `67004e454e6549915be0d79c8679eaef78a90db9`.
- `84c14bad -> 67004e45` is exactly one documentation-only commit modifying `docs/p1c-green-baseline.md`; no runtime/firmware source changed.
- After retarget to the integrated P1B `main`, GitHub reported `mergeable_state=clean`.
- Ready transition and normal merge succeeded.

### P1D / #62

- Verified GREEN implementation head: `c2b6604e2a3b48a8656c8fc9c0a95b7dd62a9174`.
- P1D gate run `33627504136`: contracts SUCCESS, production `murphy_m4` build SUCCESS, and plugin-debug QEMU build SUCCESS.
- Stage 14 regression run `33627506636`: SUCCESS.
- Integrated PR head: `83471f4d0dbf22aa4a8dc8967319d4447297e070`.
- `c2b6604e -> 83471f4d` is exactly one documentation-only commit adding `docs/p1d-green-baseline.md`; no runtime/firmware source changed.
- After retarget to the integrated P1C `main`, GitHub reported `mergeable_state=clean`.
- Ready transition and normal merge succeeded.

### P2 / #64

- Provenance-verified runtime/hardware baseline: `dd6bdd7e67c3f3631d655bd5e3ed804ef9b58da4`.
- Exact baseline workflow `33682171431`: contracts SUCCESS and firmware-build SUCCESS, including both production `murphy_m4` and `murphy_m4_qemu_plugin`.
- Hardware acceptance comment `5519232010` records `PROVENANCE VERIFIED + HARDWARE PASS`, including clean canonical/re-entry lifecycle runs and no accepted-run panic/watchdog/reset or leak-like stepwise heap decline.
- Final integrated PR head: `14b14af1fb95d756815826d22be207cd6c3fd5bc`.
- `dd6bdd7e -> 14b14af1` is two commits ahead and changes only `docs/P2_STACKED_INTEGRATION_AUDIT.md`; no runtime/firmware source changed.
- The current-tip P2 workflow `33713530554` was allowed to finish before Ready/merge. Its `contracts` job SUCCESS and `firmware-build` job SUCCESS, including both production and plugin-debug QEMU profiles.
- No submitted reviews or unresolved review threads blocked #64.
- Ready transition and normal merge succeeded.

## Review and merge-gate interpretation

At the pre-merge audits, no PR in the six-layer chain had unresolved inline review threads. No force merge was used.

The known #53 NetworkManager QEMU failures on older P0/P1A/P1B check sets were preserved as evidence and were not hidden by timeout inflation or weakened tests. GitHub's actual merge operation accepted those layers; they were therefore not treated as required merge blockers for this integration.

P1C and P1D became `mergeable_state=clean` after retarget to the newly integrated parent. P2 was not merged while its current-tip firmware check was still running; the final current-tip P2 contract and both firmware builds completed successfully before the Ready/merge transition.

## Post-integration baseline recommendation

### Recommended anchor

Use `main@e5517805f55a0ebd09d31b1b0498b4d1e4054e11` as the **post-integration runtime baseline anchor** for subsequent planning.

Future Home/UI/plugin/reader/runtime work should not branch from the old stacked feature branches. It should use this integrated `main` lineage.

### Exact-main verification note

The final #64 head was fully green before merge, but the resulting merge commit has a new SHA. At the time of this closeout refresh, the only check visible directly on `e5517805...` is a skipped dispatch check; a dedicated integrated P0-P2 contracts + production `murphy_m4` + `murphy_m4_qemu_plugin` + relevant QEMU regression gate has not yet been recorded on the exact final `main` SHA.

Therefore:

- `e5517805...` is the correct **integration anchor** now;
- before treating it as a fully revalidated exact-SHA runtime baseline for broad future runtime work, run/record the integrated gate on this exact `main` commit;
- do not reopen P0-P2 merely because the merge SHA differs from the already validated feature heads unless exact-main verification exposes a real regression.

## Remaining gap review checklist

Integration completion does not create P1E work automatically. Inspect the integrated tree only for concrete remaining violations:

- [ ] Synchronous blocking service teardown reachable from a navigation-critical path.
- [ ] `portMAX_DELAY` or another unbounded wait reachable from Home/Back or Activity-exit UI flow.
- [ ] Ambiguous service/resource ownership after the File Transfer migration.
- [ ] Multi-owner cleanup holes or non-deterministic final cleanup order.
- [ ] Raw/unbudgeted allocation paths bypassing memory governance where reserve/release is expected.
- [ ] Budget reserve/release imbalance or missing owner lifecycle release coverage.
- [ ] Service/task/server remnants after Activity exit.
- [ ] Callback/event delivery targeting a detached/freed Activity.
- [ ] Home/Back acceptance latency not bounded by the navigation supervisor contract.
- [ ] Repeated enter/exit or stale re-entry producing reproducible stepwise heap/PSRAM decline on the integrated baseline.
- [ ] #53 NetworkManager QEMU regression being conflated with this integration or hidden through timeout/test weakening.

If a concrete infrastructure gap is demonstrated, define the narrowest possible P1E scope using #63 only if it actually matches that gap. If no concrete gap remains, skip P1E.

## Current program state

- P0-P2 stacked integration: **COMPLETE**.
- Final integrated `main`: `e5517805f55a0ebd09d31b1b0498b4d1e4054e11`.
- #53 NetworkManager QEMU regression: **independent existing track**.
- Exact-main integrated revalidation: **recommended next verification step**, not a reason to add new runtime scope.
- Gap review: **next architectural review step**.
- P1E: **optional only if a concrete gap is found**.
- P3/new system-wide code: **not started by this closeout**.
