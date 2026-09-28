# Murphy M4 AI operating contract

## Canonical workflow (2026-09-28)

The single current role/stage source is `.agents/skills/m4-development-workflow/SKILL.md`.
It replaces historical Grok/Astra and multi-worktree role instructions in older docs or skill names; the hardware, device and data safety rules below remain binding.

**Default path:** Grok 4.7 Low on **zxl4869** writes one scoped runnable V1 → the smallest actual host test → independent read-only Muse High review of a frozen SHA → serial production/QEMU builds at integration → ChatGPT milestone acceptance. One writer per worktree and no speculative framework, refactor, additional agents or broad review before the first testable closed loop. Fix only demonstrated blockers and stop at the acceptance gate.

**Astra is disabled by default.** Only explicit user authorization for a named frozen commit and narrow risk enables one read-only Astra High audit and, if genuinely needed, one differential review. Never schedule, retry on quota reset, automatically dispatch, or let Astra implement, monitor or manage other agents. Existing cancellation remains in force until the user changes it. A host test, binary compile, QEMU boot and real-device test are four different evidence levels; do not conflate them.


This monorepo is the source of truth for Murphy M4 firmware, simulator, and plugins. Work in the current worktree and preserve the repository's boundaries.

## First five minutes

Before editing, inspect only the worktree and base; do not make a full firmware build a prerequisite to the minimal first patch:

```bash
git status --short --branch
git log -5 --oneline
```

After the first runnable V1, run the targeted test. At integration, run `pio run -e murphy_m4 -j1` and then `pio run -e murphy_m4_qemu -j1` for firmware/runtime changes (or the relevant package tests for plugin-only changes).

Read `HANDOFF.md`, `docs/FAST_FIRMWARE_DEV.md`, and the task-specific docs before choosing a broader test. Inspect the current branch and dirty state; never discard, reset, overwrite, or clean unknown user changes.

## Build and test contract

- `murphy_m4` is the production hardware environment and the default PlatformIO environment.
- `murphy_m4_qemu` and `murphy_m4_qemu_plugin` are simulator-only profiles. Never flash either profile to a device.
- Use the smallest relevant host, contract, simulator, or plugin test first. Reuse PlatformIO and patched-QEMU caches; build once and use `--skip-build` for subsequent journeys where supported.
- A production compile is not hardware evidence. A host model or QEMU pass is not hardware evidence. Claim device behavior only with a real-device result and record the command and artifact.
- Keep the vendored in-tree FreeInk trees under `firmware/` (`open-m4-sdk`, `lib/Epub`, `lib/Lua`, `lib/expat`, `lib/miniz`, `lib/picojpeg`). `lib/EpdFont/builtinFonts` is generated locally and stays untracked. Do not restore the legacy SD intermediary updater. Do not fetch private device archives. Do not commit `.pio`, plugin `.m4x` packages, credentials, or device captures.

## Device safety

- Production flashing is APP1-only through `firmware/scripts/flash_app1_once.sh` or the checked helper `firmware/scripts/murphy_m4_app1_flash.py`.
- Do not write APP0, the bootloader, partition table, NVS, or full flash without explicit human approval. The APP1 application offset is part of the production partition contract.
- Keep one global `m4adb` daemon/serial owner. Use repository `m4adb` tooling for device I/O; do not start competing owners and do not use `pkill -f m4adb.py`.
- USB re-enumeration after reset or slot changes is expected. Rediscover the port and reconnect the existing owner instead of launching another daemon.

## Evidence and scope

Keep firmware changes bounded-memory and streaming-first. Do not add private network endpoints to canonical configuration or docs. Update the relevant task/issue evidence when the task authorizes it; keep `HANDOFF.md` as a short pointer, not a history log. Do not flash hardware, publish, push, or modify GitHub unless the current task explicitly authorizes it.


## M4 recurring lessons

Before starting any M4 firmware/Home/Scene/QEMU task, read `docs/M4_AGENT_LESSONS.md`. When a repeatable pitfall or fix is discovered, append it there before considering the task complete.
