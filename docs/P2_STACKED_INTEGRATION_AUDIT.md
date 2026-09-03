# P2 Stacked Integration Audit

Status: **post-integration + exact-main revalidation + gap-review closeout evidence**. Documentation/review only; this document itself adds no runtime or firmware logic.

Authoritative program source: #50 comment `5519324576`.

Operational sequence:

`P0 -> P1A -> P1B -> P1C -> P1D -> P2 -> integration audit -> gap review -> optional P1E only if a concrete infrastructure gap remains -> future system-wide phase`

The older #50 P0-P10 design numbering is not the execution sequence. In particular, old-design P3 is **not** started or defined by this closeout.

## Executive conclusion

The P0-P2 stacked runtime integration is complete, the documentation closeout PR #65 is merged, and the exact integrated runtime merge commit has now been revalidated in GitHub Actions.

The exact runtime baseline is:

`e5517805f55a0ebd09d31b1b0498b4d1e4054e11`

Exact-SHA revalidation run `33716310860` completed **SUCCESS** on that SHA for:

- P2 host/Python memory-governance contract;
- P2 host C++ memory-governance contract;
- production `murphy_m4` firmware build;
- `murphy_m4_qemu_plugin` firmware build.

The later `main` commit `7fa8be25f8cfb48160140705e03bfd499baeafc5` is a documentation-only descendant created by merging PR #65. Direct comparison from `e5517805` to `7fa8be25` changes only `docs/P2_STACKED_INTEGRATION_AUDIT.md`; therefore `e5517805` remains the runtime/firmware baseline SHA while `main` continues on the same runtime tree lineage with documentation updates.

Gap review found **one concrete infrastructure gap**: the P2 governance model exists and is tested, but the production file-transfer service owner still admits/owns its runtime allocations without passing through the governance reserve/release boundary. This is sufficiently concrete to justify a narrow P1E. Existing issue #63 has been repurposed to that scope. No P1E implementation code is added by this audit.

P1D navigation behavior is **not reopened** by this gap review. The accepted exit path remains detach-first, uses a bounded rendering-mutex wait on Activity exit, and transfers service cleanup to a deferred worker. #53 remains an independent QEMU NetworkManager regression.

## Final merge ledger

| Phase | PR | Integrated PR head | `main` base immediately before merge | Resulting `main` merge commit |
| --- | --- | --- | --- | --- |
| P0 | #52 | `b63d97863ac28908c1d05d4f66c2c10b1d29571e` | `b7dd293b761a96ea8be44486e82ed99173950bff` | `502b11fedfd21530d4db7e5ac43d777b50a2047c` |
| P1A | #56 | `1590ec27a96dedae1e3d54d13d3b507b7f850a37` | `502b11fedfd21530d4db7e5ac43d777b50a2047c` | `ebe969b1766d8cf1e193bf8a37f4e727ba03cdce` |
| P1B | #58 | `a6dd2c3ce93d807dcf482e19c9aa5d38fbd4e61a` | `ebe969b1766d8cf1e193bf8a37f4e727ba03cdce` | `5c9f39a428fb359c3c4705b98af44a9cd7a7df88` |
| P1C | #60 | `67004e454e6549915be0d79c8679eaef78a90db9` | `5c9f39a428fb359c3c4705b98af44a9cd7a7df88` | `477ae9bd3916b3ff3ee5e0e28819635cafe5f610` |
| P1D | #62 | `83471f4d0dbf22aa4a8dc8967319d4447297e070` | `477ae9bd3916b3ff3ee5e0e28819635cafe5f610` | `863d37e197ac11e6f37f628f79ec22e259295c04` |
| P2 | #64 | `14b14af1fb95d756815826d22be207cd6c3fd5bc` | `863d37e197ac11e6f37f628f79ec22e259295c04` | `e5517805f55a0ebd09d31b1b0498b4d1e4054e11` |
| Docs closeout | #65 | `6207fe8d0cc19ae7acb54fd7937ad2093db9ef23` | `e5517805f55a0ebd09d31b1b0498b4d1e4054e11` | `7fa8be25f8cfb48160140705e03bfd499baeafc5` |

All six runtime PRs were marked Ready before merge and merged parent-to-child with GitHub's normal merge operation and expected head SHA. No force merge, required-check bypass, timeout inflation, or weakened test was used.

## Exact-head evidence retained from the stack

### P1C

- Verified implementation head: `84c14badd89eb4565f59a22b400a6712772541cd`.
- Gate run `33594742670`: contracts SUCCESS; firmware-build SUCCESS, including production and plugin-debug QEMU profiles.
- Integrated head `67004e454e6549915be0d79c8679eaef78a90db9` is a documentation-only descendant.

### P1D

- Verified implementation head: `c2b6604e2a3b48a8656c8fc9c0a95b7dd62a9174`.
- P1D gate run `33627504136`: contracts SUCCESS; production `murphy_m4` SUCCESS; plugin-debug QEMU build SUCCESS.
- Stage 14 run `33627506636`: SUCCESS.
- Integrated head `83471f4d0dbf22aa4a8dc8967319d4447297e070` differs from the GREEN implementation head only by `docs/p1d-green-baseline.md`.

### P2 before integration

- Provenance-verified runtime/hardware baseline: `dd6bdd7e67c3f3631d655bd5e3ed804ef9b58da4`.
- Exact-tip run `33682171431`: contracts SUCCESS; production `murphy_m4` SUCCESS; `murphy_m4_qemu_plugin` SUCCESS.
- Hardware acceptance comment `5519232010`: `PROVENANCE VERIFIED + HARDWARE PASS`; canonical and stale/re-entry lifecycle acceptance passed without accepted-run panic/watchdog/reset or leak-like stepwise heap decline.
- Final integrated PR head `14b14af1fb95d756815826d22be207cd6c3fd5bc` is two audit-document commits ahead of `dd6bdd7e` with no runtime/firmware source change.
- Current-tip P2 gate before #64 merge, run `33713530554`, completed SUCCESS for contracts + both firmware profiles.

## Exact integrated runtime revalidation

### Target

`e5517805f55a0ebd09d31b1b0498b4d1e4054e11`

This is the merge commit produced by #64 and is the first `main` commit that contains the entire P0-P2 runtime stack.

### Reproducible GitHub Actions method

The existing `.github/workflows/m4-p2-contracts.yml` supports the required host contracts and both firmware profiles. Its push trigger is scoped to `runtime/p2-memory-governance-contracts`.

After #64 was merged, that already-merged branch still pointed at `14b14af1...`. The branch was advanced **without force** to its descendant merge commit `e5517805...`. Because `e5517805` has the P2 head as a parent, this is a fast-forward ref move, not a rewritten commit and not a new runtime change. The move triggered the existing workflow against the exact commit object.

### Result

Actions run: `33716310860`

- event: `push`;
- exact `head_sha`: `e5517805f55a0ebd09d31b1b0498b4d1e4054e11`;
- workflow conclusion: **SUCCESS**;
- `contracts`: **SUCCESS**;
  - Python governance contract: SUCCESS;
  - host C++ governance contract: SUCCESS;
- `firmware-build`: **SUCCESS**;
  - `Build unchanged Murphy production profile`: SUCCESS;
  - `Build Murphy plugin-debug QEMU profile`: SUCCESS.

**Verdict:** `e5517805` is now the **exact-SHA revalidated post-integration runtime baseline** for the host contracts and the two required firmware build profiles.

This revalidation does not claim that the independent #53 real-Home NetworkManager QEMU regression is fixed; #53 remains a separate track.

## Gap review against #50

The integrated runtime was reviewed specifically against the concrete #50 checklist rather than creating work to fill phase numbers.

### Navigation-critical teardown: no new concrete P1D gap found

The integrated `CrossPointWebServerActivity` still follows the P1D design:

- `requestNavigationExit()` detaches `M4NavigationSupervisor` before invoking `onGoBack()`;
- `onExit()` detaches again idempotently;
- rendering mutex acquisition on the exit path is bounded to `pdMS_TO_TICKS(25)`;
- the display task and rendering mutex are released from the Activity;
- the `M4FileTransferService` instance is transferred in `DeferredCleanupContext` to a cleanup task;
- the cleanup task performs `service.stop(...)` only after Activity-side detach/exit has proceeded.

There are still `portMAX_DELAY` uses in the deferred worker notification wait and display/render synchronization, but this review did **not** identify an unbounded `service.stop()` wait on the accepted Home/Back Activity-exit critical path. Those occurrences remain candidates for ordinary regression measurement if future latency evidence implicates them; they are not used here to reopen P1D.

### Ownership / deferred cleanup: no concrete callback-after-detach gap found

`M4NavigationSupervisor` gates callbacks after detach, and the deferred cleanup context owns its `M4FileTransferService` independently after Activity exit. The accepted hardware/QEMU lifecycle evidence from P2 also did not show reproducible post-exit accumulation or panic on the tested canonical/re-entry paths.

### Concrete gap: production allocations bypass memory-governance admission

This item is **confirmed**.

P2 introduced `M4MemoryGovernance` and tests deterministic reserve/release, ownership-record accounting, leak detection, and a simulated service/activity multi-owner lifecycle. However, the production `M4FileTransferService` does not currently depend on or invoke that governance boundary.

On the exact integrated runtime tree, `M4FileTransferService` directly owns/allocates service runtime resources including:

- captive `DNSServer`;
- `HttpRuntime`;
- FreeRTOS storage mutex;
- `M4FileTransferHttpRoutes`;
- `M4FileTransferAuxiliaryServer`;
- the ESP HTTP server lifecycle contained by `HttpRuntime`.

The associated startup-failure and normal-stop code cleans the concrete objects, but there is no corresponding `M4MemoryGovernance::reserve/acquireOwnership` admission and `release/releaseOwnership` lifecycle accounting around this production owner.

This is not a claim that every opaque ESP-IDF allocation must be byte-metered. The gap is narrower: the explicitly service-owned runtime admission boundary can currently succeed/fail outside the governance contract that P2 established.

## P1E decision

**P1E is required, narrowly, because a concrete infrastructure gap was demonstrated.**

Existing issue #63 has been repurposed as:

`P1E: wire memory governance into file-transfer service ownership`

The old generic snapshot-observability wording was replaced. The stale `P1D` label was removed.

P1E scope is limited to:

1. establish a service-owned governance integration/dependency for file-transfer runtime admission;
2. account the explicitly owned service runtime lifecycle without pretending to measure opaque ESP-IDF internals byte-for-byte;
3. make partial-start failure rollback leave no governance ownership record or reserved budget;
4. make normal stop and deferred cleanup return governance state to baseline;
5. prove repeated lifecycle does not accumulate ownership records;
6. retain `murphy_m4` and `murphy_m4_qemu_plugin` build gates.

Explicit non-goals remain: no NetworkManager rewrite, no route/UI redesign, no global allocator replacement, no device flash requirement for the first gate, no P1D reopening absent a directly caused regression, and **no P3 definition/implementation**.

No P1E production code is included in this audit commit.

## #53 boundary

Issue #53 remains OPEN and independently tracks `m4sim test network-manager (real Home UI)` failing its publish/ready transition. Its own acceptance explicitly requires measurement-first diagnosis and forbids simply hiding the failure through timeout inflation. This integration/gap review does not change that classification.

## Current program state

- P0-P2 runtime stack: **MERGED**.
- Runtime merge commit: `e5517805f55a0ebd09d31b1b0498b4d1e4054e11`.
- Exact runtime revalidation: **FULL GREEN for required host contracts + `murphy_m4` + `murphy_m4_qemu_plugin`**, run `33716310860`.
- Runtime baseline: **`e5517805`**.
- #65 documentation closeout: **MERGED** as `7fa8be25f8cfb48160140705e03bfd499baeafc5`; runtime/firmware tree unchanged relative to `e5517805`.
- Gap review: **COMPLETE** for the #50 checklist at this stage.
- Concrete infrastructure gap: **file-transfer production allocation admission bypasses P2 memory governance**.
- P1E: **DEFINED narrowly in #63; implementation not started by this audit**.
- #53: **independent OPEN QEMU NetworkManager regression**.
- P3: **UNDEFINED / NOT STARTED**.

## Next executable work

The next runtime coding task is the narrowly defined #63 P1E only. It should begin from the current `main` lineage while preserving `e5517805` as the exact revalidated P0-P2 runtime baseline reference. P1E must use RED/GREEN contracts around production file-transfer governance wiring and must remain within #63's stated non-goals.
