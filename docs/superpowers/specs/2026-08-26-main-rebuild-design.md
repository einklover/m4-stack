# M4 Stack clean-main rebuild design

Date: 2026-08-26
Branch: `main-rebuild-20260826`
Base: `origin/main` at `e6da618`

## Goal

Replace the current GitHub `main` with a reproducible, AI-friendly Murphy M4-only development baseline while preserving useful Git history. A fresh clone should be understandable within minutes and should build the production M4 firmware without manually hunting for private/local dependency folders.

## Current state carried into the rebuild

The rebuild branch starts from the current `origin/main` commit and intentionally carries the already-validated local M4 changes that are not yet committed:

- early GPIO46 buzzer sanitizer on custom-firmware boot;
- direct Home/Reader startup without the old boot wallpaper activity;
- new default shutdown wallpaper (`○`, `休息中`, small `CrossPoint`);
- focused contract tests for the above behaviors.

A partially written dependency-bootstrap contract exists from an interrupted attempt and will be reviewed or replaced under TDD before implementation.

## Scope

### 1. Reproducible dependency bootstrap

The repository intentionally does not commit the large reconstructed dependency trees:

- `firmware/open-m4-sdk/`
- `firmware/lib/Epub/`
- `firmware/lib/Lua/`
- `firmware/lib/expat/`
- `firmware/lib/miniz/`
- `firmware/lib/picojpeg/`
- `firmware/lib/EpdFont/builtinFonts/`
- `firmware/src/network/updater_fw.bin`

They are reconstructed from the pinned `einklover/m4-device@f86b134` archive by `scripts/bootstrap_deps.sh`.

The new main must make this automatic for M4 builds. `cd firmware && pio run -e murphy_m4` must:

1. check a small set of dependency sentinel files before the M4 environment resolves local `file://` libraries and the embedded updater image;
2. do nothing and perform no network access when the dependency set is already present;
3. run the pinned root bootstrap exactly once when any required sentinel is missing;
4. fail with a clear actionable error when bootstrap/network/archive validation fails;
5. affect M4 environments only, not legacy X3/X4 environments.

The manual `bash scripts/bootstrap_deps.sh` path remains supported and documented.

The 159 MB `builtinFonts` source/header tree will **not** be committed merely to make cloning easier. The build should obtain the pinned dependency snapshot automatically. A later font-storage redesign may reduce this tree, but it is not required for this rebuild.

### 2. Repository entry points for humans and AI agents

The documentation will be reorganized around a small number of authoritative entry points.

- `README.md` — human 5-minute quick start, project purpose, build/simulator/device commands, and links to deeper docs.
- `AGENTS.md` — machine/AI operating contract: repository map, M4-only production target, required first checks, safe commands, test/build gates, and hardware safety rules.
- `docs/AI_QUICKSTART.md` — exact first-5-minutes workflow for a new coding agent, including what to inspect before editing and how to distinguish simulator, QEMU and hardware verification.
- `docs/BUILD_AND_DEPS.md` — PlatformIO setup, automatic/manual dependency bootstrap, pinned sources, cache behavior, clean-clone troubleshooting.
- `docs/SIMULATOR.md` — host simulator and QEMU roles, commands, capabilities, limitations, known emulator traps, and when simulator evidence is insufficient.
- `docs/DEVICE_AND_M4ADB.md` — m4adb lifecycle, serial behavior, APP1-only flashing, reboot/USB re-enumeration, official APP0 vs custom APP1 distinction, and recovery notes.
- `docs/PLUGINS.md` — plugin layout, package/install path, provider development/testing entry points and common runtime pitfalls.
- `docs/TROUBLESHOOTING.md` — concise known-problem index: missing reconstructed libs, concurrent `.pio` build-directory deletion, m4adb single-owner rule, official firmware hiding custom CDC, QEMU limitations, and similar recurring traps.

Existing deep handoff/reference docs will be kept when still valuable, but README/AGENTS must link to the canonical current document instead of duplicating contradictory instructions.

### 3. Production target and safety contract

The cleaned main treats Murphy M4 as the current production firmware target.

Hardware rules exposed in documentation and agent instructions:

- production build: `pio run -e murphy_m4`;
- QEMU profiles are never flashed to hardware;
- ordinary hardware flashing is APP1-only;
- never erase/write bootloader, partition table, NVS or original APP0 without explicit human approval;
- `/dev/cu.usbmodem*` disappearing while official APP0 runs is not by itself a failure;
- m4adb should have one serial owner/daemon at a time;
- hardware claims require hardware evidence; simulator/QEMU success is not reported as device validation.

### 4. Validation before promotion to GitHub main

Promotion is gated on fresh evidence from the rebuilt branch.

Required checks:

1. dependency bootstrap contract follows RED → GREEN TDD;
2. buzzer, direct-boot and shutdown-wallpaper contracts pass;
3. `git diff --check` passes;
4. production M4 build succeeds with a single-process build (`pio run -e murphy_m4 -j1`);
5. a realistic clean-clone/missing-dependency test proves the automatic bootstrap occurs early enough for PlatformIO local libraries and `embed_files`;
6. the automatic pre-script no-op path is verified with dependencies already populated;
7. host simulator minimum smoke/regression set succeeds using the documented command;
8. every command presented in the top-level quick-start docs is syntax-checked or executed as applicable;
9. final branch state contains no local-only absolute symlinks, generated build products, credentials or device captures;
10. final production `firmware.bin` size and SHA-256 are recorded for the promotion report.

Hardware re-flashing is not required merely to rewrite documentation/dependency bootstrap, but the already-flashed M4 behavior must not be changed accidentally by the portability/documentation work. If firmware source changes beyond the already-tested set, hardware validation is reconsidered before promotion.

## Branch and promotion strategy

- Work on `main-rebuild-20260826`, preserving the existing repository ancestry.
- Commit the design separately from implementation.
- Commit the already-validated local firmware changes in a focused commit after re-verification.
- Commit portability/bootstrap work separately.
- Commit documentation/onboarding cleanup separately where practical.
- Run final verification on the branch tip.
- Push the rebuild branch for traceability.
- Replace remote `main` with the verified rebuild branch tip using an explicit ref update/force-with-lease style operation only after confirming `origin/main` has not moved unexpectedly.
- Keep the old main commit reachable through history and optionally tag the pre-rebuild main tip before replacement.

## Non-goals

- No orphan-history rewrite.
- No wholesale vendoring of the 159 MB font header tree.
- No redesign of the font engine in this pass.
- No unrequested full-flash/partition-table changes.
- No claim that QEMU fully emulates Murphy M4 hardware.
- No unrelated firmware feature work while stabilizing the new main.

## Success condition

A new contributor or coding agent can clone the repository, understand the project boundaries and safety rules quickly, run the documented build/simulator paths, and obtain a valid Murphy M4 production build without knowing about the old local dependency layout in advance.
