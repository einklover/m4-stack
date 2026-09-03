# P2 Stacked Integration Audit

Status: integration-audit evidence record; documentation/review only. No runtime or firmware logic is changed by this document.

Authoritative program source: #50 comment `5519324576`.

Operational sequence used here:

`P0 -> P1A -> P1B -> P1C -> P1D -> P2 -> integration audit -> gap review -> optional P1E only if a concrete gap remains -> future system-wide phase`

The older #50 P0-P10 design numbering is not used as the execution sequence. In particular, the older "P3 Wi-Fi event facade" is not treated as the next task.

## Audit scope

This audit covers the open Draft stacked PR chain:

`#52 -> #56 -> #58 -> #60 -> #62 -> #64`

It records live base/head relationships, exact tips, phase-specific CI evidence, review blockers, integration gaps, recommended merge sequencing, and the required post-integration baseline.

Audit snapshot: 2026-09-02 (GitHub live state at audit time).

## Executive conclusion

The implemented P0-P2 runtime stack is technically complete at the recorded phase gates, and P2 has exact-tip FULL GREEN CI plus QEMU and provenance-verified hardware acceptance. The chain is not yet integration-ready as a whole because P1A PR #56 is still based on `main` instead of the P0 branch. All six PRs remain Draft. No PR has submitted reviews or unresolved inline review threads at audit time.

P1D's recorded GREEN runtime head `c2b6604e2a3b48a8656c8fc9c0a95b7dd62a9174` and live PR tip `83471f4d0dbf22aa4a8dc8967319d4447297e070` are not competing runtime implementations: live tip is exactly one commit ahead and only adds `docs/p1d-green-baseline.md`. P2 is correctly based on that live P1D tip.

Therefore:

1. Do not merge yet.
2. Repair/confirm the stacked metadata beginning with P1A before merge execution.
3. Preserve the recorded GREEN runtime heads as evidence while treating their documentation-only descendants as the live integration tips.
4. Integrate in parent-to-child order, rechecking the resulting `main` baseline after each retarget/merge step.
5. Run a final integrated gate on the eventual post-#64 `main` commit before future runtime work branches from it.

## Live stacked PR topology

| Phase | PR | State | Base | Base SHA | Head | Live head SHA | Topology verdict |
| --- | --- | --- | --- | --- | --- | --- | --- |
| P0 | #52 | OPEN Draft | `main` | `b7dd293b761a96ea8be44486e82ed99173950bff` | `runtime/p0-memory-manager-baseline` | `b63d97863ac28908c1d05d4f66c2c10b1d29571e` | Correct root |
| P1A | #56 | OPEN Draft | `main` | `b7dd293b761a96ea8be44486e82ed99173950bff` | `runtime/p1a-file-transfer-service-contracts` | `1590ec27a96dedae1e3d54d13d3b507b7f850a37` | **Stack defect: base still `main`; should be P0 branch for clean stacked review** |
| P1B | #58 | OPEN Draft | `runtime/p1a-file-transfer-service-contracts` | `1590ec27a96dedae1e3d54d13d3b507b7f850a37` | `runtime/p1b-file-transfer-service-ownership` | `a6dd2c3ce93d807dcf482e19c9aa5d38fbd4e61a` | Correct |
| P1C | #60 | OPEN Draft | `runtime/p1b-file-transfer-service-ownership` | `a6dd2c3ce93d807dcf482e19c9aa5d38fbd4e61a` | `runtime/p1c-esp-http-server-contracts` | `67004e454e6549915be0d79c8679eaef78a90db9` | Correct |
| P1D | #62 | OPEN Draft | `runtime/p1c-esp-http-server-contracts` | `67004e454e6549915be0d79c8679eaef78a90db9` | `runtime/p1d-navigation-supervisor` | `83471f4d0dbf22aa4a8dc8967319d4447297e070` | Correct |
| P2 | #64 | OPEN Draft | `runtime/p1d-navigation-supervisor` | `83471f4d0dbf22aa4a8dc8967319d4447297e070` | `runtime/p2-memory-governance-contracts` | `dd6bdd7e67c3f3631d655bd5e3ed804ef9b58da4` | Correct; base equals P1D live tip exactly |

### Required topology correction

PR #56 says in its own body that it was initially based on `main` to record the TDD RED/GREEN sequence and would then be retargeted to `runtime/p0-memory-manager-baseline`. That retarget has not happened. This is the only observed broken base/head link in the six-PR stack.

No base mutation is performed by this audit document. The recommended action is to retarget #56 to `runtime/p0-memory-manager-baseline`, then re-audit its diff/check context before merge operations begin.

## Exact-head and CI evidence

CI is recorded using two distinct concepts:

- **phase gate evidence**: the checks explicitly required by that phase and recorded at its verified implementation head;
- **repository-wide workflow state**: other workflows that may include the independent #53 QEMU NetworkManager regression.

A repository-wide failure that is already isolated to #53 is not rewritten as a phase regression. Conversely, this audit does not call those older heads "FULL GREEN" when unrelated workflows are red.

### P0 / PR #52

- Live/verified head: `b63d97863ac28908c1d05d4f66c2c10b1d29571e`.
- PR evidence records GREEN dependency/bootstrap, runtime snapshot/facade/Home contracts, plugin-debug build, production build/composition, production-profile QEMU boot, and progressive stream regressions.
- Live Actions on that head include successful `Murphy production BIN runtime` and `Murphy progressive stream regressions` runs.
- `m4 fast firmware loop` and `m4sim clean BIN smoke` include the known NetworkManager regression tracked separately in #53; #52 explicitly records that boundary.
- Audit verdict: phase-complete evidence exists; #53 remains separate; not yet merge-executed.

### P1A / PR #56

- Live head: `1590ec27a96dedae1e3d54d13d3b507b7f850a37`.
- Phase intent is the pure host-testable File Transfer lifecycle/state contract before ownership migration.
- Live Actions include successful `Murphy production BIN runtime`, `Murphy Stage 14 regression contracts`, and `Murphy progressive stream regressions` runs.
- `m4 fast firmware loop` / `m4sim clean BIN smoke` still contain the independent #53 path.
- Integration blocker: PR base remains `main`, so the review diff is not a clean child of P0.
- Audit verdict: technical phase evidence is not the blocker; topology metadata is.

### P1B / PR #58

- Exact verified/live head: `a6dd2c3ce93d807dcf482e19c9aa5d38fbd4e61a`.
- PR records GREEN P0/P1A/P1B host contracts, plugin-debug build, fast boot/protocol smoke, and production compile/boot verification at the exact head.
- Live Actions include successful production BIN, Stage 14 contracts, and progressive-stream runs.
- `m4 fast firmware loop` / clean-BIN failures remain attributed to independently tracked #53 unless new causality is demonstrated.
- Audit verdict: phase-complete; stack link to P1A is correct.

### P1C / PR #60

- Recorded GREEN implementation head: `84c14badd89eb4565f59a22b400a6712772541cd`.
- Recorded gate run: Actions `33594742670`; `contracts` SUCCESS and `firmware-build` SUCCESS.
- Live PR head: `67004e454e6549915be0d79c8679eaef78a90db9`.
- `84c14bad -> 67004e45` is exactly one commit ahead and modifies only `docs/p1c-green-baseline.md` (5 additions / 2 deletions); no runtime/firmware source change.
- Audit verdict: live tip is a documentation-only descendant of the verified GREEN runtime head; stack link to P1B is correct.

### P1D / PR #62

- Recorded GREEN implementation head: `c2b6604e2a3b48a8656c8fc9c0a95b7dd62a9174`.
- Recorded P1D gate run: Actions `33627504136`; contracts SUCCESS, `murphy_m4` SUCCESS, plugin-debug QEMU firmware build SUCCESS.
- Recorded Stage 14 regression run: `33627506636`; contracts SUCCESS.
- Live PR head: `83471f4d0dbf22aa4a8dc8967319d4447297e070`.
- Direct GitHub compare `c2b6604e -> 83471f4d` reports `ahead_by=1`, `behind_by=0`; the only changed file is newly added `docs/p1d-green-baseline.md` (34 lines). No runtime/firmware source changed.
- Live tip also has a successful Stage 14 contracts workflow.
- Audit verdict: the live tip preserves the verified GREEN runtime tree plus closeout documentation; P1D does not need to be reopened for code because of the SHA difference.

### P2 / PR #64

- Exact live tip: `dd6bdd7e67c3f3631d655bd5e3ed804ef9b58da4`.
- Base: `runtime/p1d-navigation-supervisor` at exact SHA `83471f4d0dbf22aa4a8dc8967319d4447297e070`.
- Exact-tip workflow: `m4 P2 memory governance contracts`, Actions run `33682171431`, conclusion SUCCESS.
- `contracts` job: SUCCESS.
- `firmware-build` job: SUCCESS, including both:
  - `Build unchanged Murphy production profile` (`murphy_m4`)
  - `Build Murphy plugin-debug QEMU profile` (`murphy_m4_qemu_plugin`)
- PR #64 hardware acceptance comment `5519232010` records the same exact tip, FULL GREEN CI, clean QEMU A/B, fresh production rebuild, APP1 write/readback provenance, canonical hardware 5/5 PASS, stale/re-entry 1+5 PASS, no panic/watchdog/reset in the accepted runs, and no leak-like stepwise heap decline.
- Audit verdict: P2 implementation/CI/QEMU/hardware acceptance is complete. Remaining work is integration/review workflow, not new P2 runtime code.

## Review audit

At audit time, for each of #52, #56, #58, #60, #62, and #64:

- submitted pull-request reviews: **none**;
- inline review threads: **none**;
- unresolved inline review threads: **none**;
- requested reviewers: none shown in live PR metadata;
- PR state: OPEN Draft.

This means there is no unresolved review-thread blocker, but it does **not** mean the PRs are approved. They have not entered normal Ready-for-review approval flow yet.

## Merge/readiness assessment

| PR | Technical evidence | Stack metadata | Review state | Current recommendation |
| --- | --- | --- | --- | --- |
| #52 P0 | Sufficient phase evidence; #53 separated | Correct | Draft; no reviews/threads | Candidate parent once integration starts |
| #56 P1A | Phase evidence exists | **Incorrect base (`main`)** | Draft; no reviews/threads | **Retarget first; do not merge as-is** |
| #58 P1B | Exact-head phase evidence exists | Correct child of P1A | Draft; no reviews/threads | Hold until #56 topology/integration is clean |
| #60 P1C | GREEN runtime head + docs-only live descendant | Correct child of P1B | Draft; no reviews/threads | Hold until parent layers integrate |
| #62 P1D | GREEN runtime head + docs-only live descendant | Correct child of P1C | Draft; no reviews/threads | Hold until parent layers integrate |
| #64 P2 | Exact-tip FULL GREEN + QEMU/hardware acceptance | Correct child of P1D live tip | Draft; no reviews/threads | Technically eligible to become Ready only after stack metadata/integration closeout is recorded; this audit does not flip it |

## Recommended integration / merge sequence

Do not merge child PRs into their feature-branch parents as a substitute for mainline integration. Preserve reviewable phase boundaries.

Recommended sequence:

1. Retarget #56 from `main` to `runtime/p0-memory-manager-baseline`; confirm the resulting P1A-only diff is sane.
2. Reconfirm #52 and #56 base/head SHAs have not moved unexpectedly.
3. Merge #52 (P0) into `main` only when explicitly authorized.
4. Retarget #56 to the new `main`; verify the P1A-only diff/check context, then merge #56 when authorized.
5. Retarget #58 to the new `main`; verify P1B-only diff/check context, then merge #58.
6. Retarget #60 to the new `main`; verify P1C-only diff/check context, then merge #60.
7. Retarget #62 to the new `main`; verify P1D-only diff/check context, then merge #62.
8. Retarget #64 to the new `main`; verify P2-only diff/check context, then merge #64.
9. On the final post-#64 `main` commit, run the integrated contracts + production `murphy_m4` + `murphy_m4_qemu_plugin` gates and the relevant QEMU runtime regression suite.
10. Record that final `main` SHA as the **post-integration runtime baseline**. Future Home/UI/plugin/reader/runtime work should branch from that recorded commit, not from an intermediate stacked branch.

The exact post-integration baseline SHA cannot be named before merges occur because it will be the resulting final `main` commit after the authorized integration sequence.

## Gap review checklist (record only; no P1E implementation here)

After integration, inspect the integrated tree for concrete remaining violations. Do not create work merely to fill a phase number.

- [ ] Synchronous blocking service teardown still reachable from a navigation-critical path.
- [ ] `portMAX_DELAY` or another unbounded wait reachable from Home/Back or Activity-exit UI flow.
- [ ] Ambiguous service/resource ownership after the File Transfer migration.
- [ ] Multi-owner cleanup holes: service/activity owners can release in different orders without deterministic final cleanup.
- [ ] Raw/unbudgeted allocation paths bypassing memory governance where the migrated runtime is expected to reserve/release.
- [ ] Budget reserve/release imbalance or missing owner lifecycle release coverage.
- [ ] Service/task/server remnants after Activity exit.
- [ ] Callback/event delivery can target a detached/freed Activity.
- [ ] Home/Back acceptance latency is not bounded by the navigation supervisor contract.
- [ ] Repeated enter/exit or stale re-entry shows reproducible stepwise heap/PSRAM decline on the integrated baseline.
- [ ] #53 QEMU NetworkManager regression remains unresolved or gets incorrectly hidden by timeout inflation instead of independently fixed.

If this review finds a specific infrastructure gap, define the narrowest possible P1E scope using #63 only if it actually matches the discovered gap. If no concrete gap remains, skip P1E.

## P2 closeout / Ready-for-review recommendation

P2's own technical closeout conditions are satisfied at `dd6bdd7e`: exact-tip CI is green for contracts + both firmware profiles, QEMU evidence is clean for the accepted lifecycle scenarios, and the separately authorized hardware acceptance is provenance-verified and PASS.

However, this audit does **not** change #64 from Draft to Ready because the stacked chain still has a known P1A base defect and the user explicitly requested evidence/recommendation before state transitions.

Recommended decision after the #56 topology correction and final audit refresh:

- if no new CI/review blocker appears, mark #64 Ready for review;
- do not merge #64 ahead of its parent chain;
- do not start P1E/P3 code before the integration audit and gap review are complete.

## Current blockers and non-blockers

### Blocking integration workflow

1. #56 base is still `main`, contrary to the intended clean stacked topology.
2. All PRs are still Draft and have no submitted approvals; Ready/review policy has not yet been exercised.
3. Final post-integration baseline does not exist until authorized merges occur and the integrated gates are rerun.

### Not a P1D/P2 runtime blocker

- P1D `c2b6604e` vs `83471f4d`: documentation-only descendant; no runtime change.
- P1C `84c14bad` vs `67004e45`: documentation-only descendant; no runtime change.
- P2 earlier polluted/noncanonical QEMU panic observation: not reproduced in clean QEMU or provenance-verified hardware acceptance; retain as observation, not active P2 blocker.
- #53: independent QEMU NetworkManager regression; do not mask it with timeout inflation and do not conflate it with P2 closeout without new causal evidence.

## Next action after this document

1. Refresh the PR metadata/CI snapshot after this audit-doc commit.
2. Leave a concise evidence comment on #64 or #50 pointing to this document and the P1A base correction.
3. Do not mark Ready or merge without the requested state-transition decision.
4. After topology is clean, execute integration in the parent-to-child sequence above under explicit merge authorization.
