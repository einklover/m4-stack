# AI quickstart

This repository is an M4-only main-rebuild worktree. The source tree, current handoff, and the active task are more reliable than old chat or historical handoff notes.

## Read first

For a new session, read these in order:

1. `AGENTS.md`
2. `HANDOFF.md`
3. `docs/FAST_FIRMWARE_DEV.md`
4. the active task/issue named by the request
5. the task-specific code and docs

Then inspect the current branch and dirty state. Preserve changes you did not create.

## Repository map

- `firmware/` — production Murphy M4 firmware, PlatformIO configuration, contracts, and device tools.
- `simulator/` — host models, patched QEMU build/runtime helpers, and simulator journeys.
- `plugins/` — fanqie, jjwxc, weread, and related plugin sources.
- `scripts/` — cross-cutting repository helpers, including dependency bootstrap.
- `docs/` — stable workflow, safety, and troubleshooting contracts.

## Choose the smallest work path

Firmware changes start with the production target and its narrowest relevant contract or host test. Use `cd firmware && pio run -e murphy_m4 -j1` for the production compile gate.

Simulator changes use the root `./m4sim` entry point and the targeted journey exposed by its help. Do not invent a second QEMU command path; consult `docs/M4SIM_CLEAN_SETUP.md` and the simulator-owned guide when the task reaches simulator detail.

### Center-kernel font source

The original custom source TTF (`标准像素粗.ttf`) is intentionally not
distributed because redistribution permission is unclear. Normal firmware
builds do not need it: the tracked
`firmware/src/fontdata/m4_center_kernel_16x16.bin` is already present. Only
regeneration or customization needs a source font; use a legally
usable/distributable Chinese-capable pixel-style TTF. The current generator
rejects a different TTF because it enforces the known source-font SHA-256, so
that validation must be intentionally updated before customization; the
command syntax is:

```bash
python3 firmware/scripts/generate_m4_center_kernel.py --font <path-to-font.ttf>
```

A different TTF would change glyph appearance and hash and cannot reproduce
the current artifact byte-for-byte. The ~159 MiB
`firmware/lib/EpdFont/builtinFonts/` tree is a separate
bootstrap-reconstructed dependency, not the source for this custom font.

Plugin changes stay inside the relevant `plugins/*` project and use that project's README and package tooling. Keep plugin packaging and installation detail in the plugin-owned documentation.

## Gather evidence

Record the exact command, result, and artifact location for each meaningful check. Label evidence as one of:

- host/unit/contract test;
- simulator or patched-QEMU runtime;
- production firmware compile;
- physical-device observation.

Only the last category supports a hardware claim. A successful build or QEMU protocol session does not prove USB, RF, power, display, or other physical behavior.

## Small checklist

Before you edit:

- [ ] `git status --short --branch` and `git log -5 --oneline` inspected
- [ ] `AGENTS.md`, `HANDOFF.md`, fast-loop guidance, and active task read
- [ ] smallest relevant command and safety boundary identified

Before you claim done:

- [ ] targeted validation passed, or the failure is recorded
- [ ] production/QEMU/device evidence is labeled correctly
- [ ] `git diff --check` is clean
- [ ] generated outputs and unknown user changes are preserved
- [ ] the final diff is limited to the requested scope
