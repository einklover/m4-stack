# HANDOFF — current Murphy M4 development checkpoint

Last materially updated: **2026-08-13**

## One-liner

Use `einklover/m4-stack` as the single working tree for Murphy M4 firmware + m4sim + plugins. The simulator has reached the agreed freeze gate; future work should optimize/modify production firmware and use m4sim as a fast regression tool, not continue expanding the simulator framework.

## Mandatory session bootstrap

Every new AI/Codex conversation must read, in order:

1. `AGENTS.md`
2. `HANDOFF.md`
3. `docs/FAST_FIRMWARE_DEV.md`

Then inspect the current branch HEAD and CI. Do not infer current state from old chat summaries or old QEMU documents.

## Current working branch

Primary active branch for the 2026-08-13 simulator/firmware validation work:

```text
agent/m4-emulator-stage13-e2e-validation
```

Always inspect the current HEAD before editing because this handoff intentionally avoids pinning itself to a commit that becomes stale after every documentation or CI optimization commit.

## 2026-08-13 completed checkpoint

### m4sim / QEMU

- patched Espressif QEMU v3 `murphy-m4` machine boots the real plugin-debug firmware path
- m4adb protocol/control path works
- generic plugin-debug smoke passes
- Network Manager real Home UI 3-mode E2E passes
- the final Network Manager failure was traced to Calibre using `WiFi.status()` as its only connectivity source under QEMU, which incorrectly fell into an unmodelled ESP Wi-Fi radio scan even though QEMU/open_eth networking was already available
- the fix reuses the existing QEMU network compatibility helper instead of adding another simulator network model
- representative successful Network Manager E2E runtime: about 42 seconds

**Decision:** m4sim is now frozen/stable enough for firmware development. Do not add simulator features unless a future production firmware change demonstrates a simulator correctness gap.

### Fast CI loop

Daily firmware feedback now has a separate workflow:

```text
.github/workflows/m4-fast.yml
```

It is deliberately separate from the conservative full gate `.github/workflows/m4sim-smoke.yml`.

The fast loop:

- caches patched QEMU independently from firmware source
- caches `~/.platformio` plus `PLATFORMIO_BUILD_CACHE_DIR`
- builds `murphy_m4_qemu_plugin` once
- runs smoke with `--skip-build`
- runs Network Manager E2E with `--skip-build`

First seed run on 2026-08-13: PASS. Timings from that cold-cache run:

- QEMU build: 3m35s
- plugin-debug firmware build: 6m12s
- smoke after explicit build: 13s
- Network Manager E2E: 42s

Both cache post-save steps completed successfully. A subsequent run that starts after this seed is the cache-hit performance check. New agents should use the fast-loop result for ordinary iteration and reserve the full gate for checkpoints.

### Runtime fonts

The runtime font path has already been expanded and host/CI-validated for modern Chinese font packaging, including:

- TTF / TTC glyf
- OTF / TTC/OTC CFF1, including CID-keyed CFF1
- CFF2 / variable collection default-instance paths
- streamed/PSRAM-aware glyph processing rather than whole-font RAM loading
- real Source Han / Noto-class Chinese font fixtures in host/CI tests

Important remaining distinction: host/CI parser+raster validation is not the same as a long real-device reading soak. Do not claim real-device font soak unless it has actually been run.

### JJWXC / progressive network loading

The progressive HTTP/catalog path already has host/CI coverage for:

- long catalog streaming (thousands of chapters)
- chunked framing across pump boundaries
- payload-inactivity timeout semantics
- bounded-memory progressive parsing
- production Murphy M4 compilation

Do not claim a live remote JJWXC end-to-end run unless it has actually been verified against the current live site.

## Next production work

After the m4sim freeze, prioritize real firmware validation rather than simulator expansion:

1. real-device Chinese CID-keyed OTF/OTC reading soak: repeated page turns, size switches, face switches, reopen/close, heap/PSRAM stability
2. real JJWXC long-catalog/live-site progressive stream validation: HTTPS -> progressive parser -> SD catalog -> TOC -> first chapter
3. only after those pass, do the final production regression/build checkpoint

## Fast development loop

The authoritative workflow is `docs/FAST_FIRMWARE_DEV.md`.

Core rule: **build once, then reuse the build for targeted tests**.

```bash
export PATH="$HOME/.local/bin:$HOME/.platformio/penv/bin:$PATH"
export PLATFORMIO_BUILD_CACHE_DIR="$HOME/.cache/murphy-m4/platformio-build-cache"

# Only if dependencies are absent
bash scripts/bootstrap_deps.sh

# Incremental simulator firmware build
cd firmware && pio run -e murphy_m4_qemu_plugin && cd ..

# Reuse the exact build
./m4sim test smoke --plugin-debug --skip-build --ready-seconds 90
./m4sim test network-manager --plugin-debug --skip-build --ready-seconds 90

# Final production compile gate
cd firmware && pio run -e murphy_m4
```

Patched QEMU is a toolchain dependency, not a per-firmware-build product. Reuse it from the persistent cache and rebuild it only if its upstream pin/patch series/builder changes.

## Why this workflow matters

A representative pre-cache CI run took roughly 11.5 minutes, while the actual Network Manager E2E took only ~42 seconds. The bulk of the delay was repeated QEMU and firmware compilation. Future agents must preserve QEMU/PlatformIO caches and choose targeted tests instead of blindly re-running the entire full smoke after every edit.

## Safety / architecture boundaries

- real device profile: `murphy_m4`
- QEMU profile: `murphy_m4_qemu_plugin`; never flash it to hardware
- production flashing is APP1-only
- large font/network/catalog paths stay bounded-memory, streaming-first, PSRAM-aware
- do not reintroduce whole HTTP bodies or whole fonts into internal heap
- do not expand m4sim unless required for fidelity to production firmware

## Handoff discipline

When a task reaches a new stable checkpoint, update this file with:

- what was actually verified
- current remaining blockers
- exact fast test(s) that reproduce the relevant path
- any new non-negotiable architecture constraints

This file is deliberately the durable replacement for chat-only handoffs.
