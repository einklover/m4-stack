# HANDOFF — Murphy M4 current checkpoint

Last updated: **2026-08-13**

## Start here in every new conversation

Use `einklover/m4-stack` as the durable source of truth. Read in order:

1. `AGENTS.md`
2. `HANDOFF.md`
3. `docs/FAST_FIRMWARE_DEV.md`

Then inspect the current branch HEAD and CI before editing. Do not reconstruct project state from old chat history.

Current working branch:

```text
agent/m4-emulator-stage13-e2e-validation
```

## m4sim status: frozen

The simulator has reached the agreed firmware-development gate:

- patched QEMU `murphy-m4` machine boots the real plugin-debug firmware
- m4adb control path works
- generic plugin-debug smoke passes
- Network Manager real-Home 3-mode E2E passes
- Network Manager E2E itself takes about 42 seconds

The final Network Manager blocker was the Calibre path treating `WiFi.status()` as the only connectivity source under QEMU. It now reuses the existing QEMU/open_eth network compatibility helper instead of entering an unmodelled ESP Wi-Fi radio scan.

**Do not expand m4sim further unless a future production firmware change proves a simulator fidelity gap.**

## Fast firmware CI: verified

Daily development feedback is:

```text
.github/workflows/m4-fast.yml
```

Final/checkpoint simulator gate remains:

```text
.github/workflows/m4sim-smoke.yml
```

The fast workflow caches patched QEMU separately from firmware source, caches `~/.platformio` and `PLATFORMIO_BUILD_CACHE_DIR`, builds `murphy_m4_qemu_plugin` once, then runs smoke and Network Manager with `--skip-build`.

### Cold seed run

- QEMU build: 3m35s
- plugin-debug firmware build: 6m12s
- smoke: 13s
- Network Manager: 42s
- PASS; both caches saved successfully

### Warm-cache validation

GitHub Actions run `31704315361`: **PASS in 2m09s total**.

- QEMU cache restore: ~2s
- QEMU rebuild: skipped
- PlatformIO cache restore: ~17s
- plugin-debug firmware build: **28s** (was 6m12s cold)
- smoke: 13s
- Network Manager: 42s

This is now the normal development loop. Use the fast workflow for ordinary firmware iteration; reserve the full gate for checkpoints.

## Fast local commands

```bash
export PATH="$HOME/.local/bin:$HOME/.platformio/penv/bin:$PATH"
export PLATFORMIO_BUILD_CACHE_DIR="$HOME/.cache/murphy-m4/platformio-build-cache"

# Only when dependency copies are absent
bash scripts/bootstrap_deps.sh

# Build simulator firmware once
cd firmware && pio run -e murphy_m4_qemu_plugin && cd ..

# Reuse it
./m4sim test smoke --plugin-debug --skip-build --ready-seconds 90
./m4sim test network-manager --plugin-debug --skip-build --ready-seconds 90

# Final production compile gate
cd firmware && pio run -e murphy_m4
```

Patched QEMU is a toolchain dependency, not a per-firmware-build product. Ordinary firmware changes must not rebuild it.

## Current production work remaining

### Runtime fonts

Host/CI support already covers TTF/TTC glyf, CFF1/CID-keyed CFF1, CFF2 and variable OTF/OTC default-instance paths with bounded-memory/PSRAM-aware processing and real Source Han/Noto-class fonts.

Still needed: **real-device Chinese CID-keyed OTF/OTC reading soak** — repeated page turns, size/face switches, reopen/close and heap/PSRAM stability.

### JJWXC streaming

Host/CI already covers long catalog progressive streaming, chunk framing boundaries, payload-inactivity timeout semantics, bounded memory and Murphy M4 production compilation.

Still needed: **live-site long-catalog E2E** — HTTPS → progressive parser → SD catalog → TOC → first chapter.

Do not claim either real-device font soak or live JJWXC E2E until actually run.

## Non-negotiable boundaries

- real-device profile: `murphy_m4`
- QEMU profile: `murphy_m4_qemu_plugin`; never flash it to hardware
- production flashing is APP1-only
- large fonts/network/catalogs remain streaming-first, bounded-memory and PSRAM-aware
- do not reintroduce whole HTTP bodies or whole fonts into internal heap
- update this file whenever a new stable checkpoint materially changes what the next conversation should do
