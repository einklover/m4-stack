# HANDOFF — Murphy M4 session entry

Last updated: **2026-08-17**

This file is intentionally thin. **Active goals, progress, measurements and acceptance evidence live in GitHub Issues, not here.**

## New conversation bootstrap

Use `einklover/m4-stack` as the durable source of truth. Read in order:

1. `AGENTS.md` — permanent rules / safety / architecture boundaries
2. `HANDOFF.md` — this entry point
3. `docs/FAST_FIRMWARE_DEV.md` — fast build/test/cache workflow
4. the active GitHub Issue(s) below
5. current branch HEAD / recent CI

Do not reconstruct current project state from old chat history.

## Active project tracking

Umbrella production-readiness issue:

- **#17 — Murphy M4 production readiness — fonts, streaming, device validation**

Current execution issues:

- **#18 — Real-device runtime OTF/OTC soak validation**
- **#19 — JJWXC live long-catalog progressive-stream E2E**
- **#26 — M0 Android virtual-display browser capture validation**

Detailed progress, commit SHAs, measurements, failures, fixes and acceptance evidence belong in those Issues. Update the relevant Issue during work instead of growing this file.

## Current working branch

Firmware / production-readiness work stays on:

```text
main
```

E-ink Browser Bridge M0 is on a parallel branch:

```text
agent/eink-browser-bridge
```

Stage 13 (`agent/m4-emulator-stage13-e2e-validation`) plus the 2026-08-17 QEMU AES/GDMA, TTF advance-cache, and native-provider first-window work was merged to `main`. Start firmware work from `main`. Use `agent/eink-browser-bridge` only for the virtual-browser path (#26).

Always inspect HEAD before editing.

## Stable infrastructure checkpoint

- m4sim generic smoke: PASS
- Network Manager real-Home 3-mode E2E: PASS
- m4sim: frozen for ordinary firmware work
- daily fast CI: `.github/workflows/m4-fast.yml`
- checkpoint/full simulator gate: `.github/workflows/m4sim-smoke.yml`
- verified warm fast-loop run `31704315361`: PASS in **2m09s**
- warm QEMU rebuild: skipped via cache
- warm plugin-debug firmware build: ~28s

Do not expand m4sim unless a production firmware change proves a simulator fidelity gap.

## Management convention

- **Issue** = goal / status / acceptance criteria / execution evidence
- **PR + commit** = implementation
- **CI** = test evidence
- **docs** = stable how-to / architecture
- **AGENTS.md** = permanent working rules
- **HANDOFF.md** = only the pointer that tells a new session where to look

When #18 or #19 changes materially, update the Issue. Only update this file when the active Issue set, branch strategy or stable infrastructure entry points change.
