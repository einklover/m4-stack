# HANDOFF — 2026-08-11 agent-ready monorepo

## One-liner

This tree (`work/m4-stack` / GitHub `einklover/m4-stack`) is a **single checkout** of firmware + simulator + plugins at checkpoint **2026-08-11**, so a new AI can build and work without hunting multi-worktree paths under `m4crosspoint/`.

## What is current

| Area | State |
|------|--------|
| Firmware | `checkpoint/2026-08-11-agent-ready` @ dfec303 — Legado LAN discovery, catalog harden, QEMU env `murphy_m4_qemu` |
| Simulator | `checkpoint/2026-08-11-agent-ready` @ f377493 — board model + QEMU AI debug + `run_murphy_bin.py` |
| Plugins | fanqie/jjwxc/weread on `agent/checkpoint-20260809-native-latest` |
| QEMU full-flash | **Blocked** at second-stage `entry` / PSRAM — see `docs/QEMU_BOOT_HANDOFF.md` |
| Local parent | `/Volumes/z/paseo/m4crosspoint` still has older worktrees (`publish/`, `native-app/`, `vendor/`, `wap-checkpoint/`). **Prefer this monorepo as cwd.** |

## Not done / open

1. QEMU Stage2 (app `setup()`) — production octal hang; qio_qspi profile still hangs with `-m 8M`.  
2. Legado may need more real-device soak after LAN auto-discover.  
3. Optional: mirror monorepo commits back into component repos on each release.

## Do first after clone

```bash
bash scripts/bootstrap_deps.sh
export PATH="$HOME/.platformio/penv/bin:$PATH"
cd firmware && pio run -e murphy_m4
cd ../simulator && cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

## Agent prompt snippet

```text
Cwd: this monorepo root. Read AGENTS.md + HANDOFF.md first.
Bootstrap: bash scripts/bootstrap_deps.sh
Build firmware: cd firmware && pio run -e murphy_m4
Build simulator: cd simulator && cmake -B build && cmake --build build
Do not flash QEMU profile to hardware. APP1-only flash scripts only.
```
