# Murphy M4 fast firmware development loop

**This file is the persistent source of truth for future AI/Codex sessions.**

If a new conversation is asked to continue Murphy M4 firmware work, read these files first, in order:

1. `AGENTS.md`
2. `HANDOFF.md`
3. `docs/FAST_FIRMWARE_DEV.md` (this file)
4. Only then read task-specific code/docs.

Do not reconstruct the workflow from chat history when the repository already contains the answer.

## Current policy

- `einklover/m4-stack` is the preferred working repository for firmware + simulator + plugins.
- The QEMU/m4sim framework is **frozen unless a firmware change exposes a simulator correctness bug**. Do not expand it for convenience features.
- The real-device firmware profile is `murphy_m4`.
- The simulator profile is `murphy_m4_qemu_plugin`; never flash it to hardware.
- Production hardware flashing remains APP1-only.
- Large network/font work must remain bounded-memory / streaming-first.

## Why the old loop was slow

A representative GitHub Actions run on 2026-08-13 took about 11.5 minutes:

- dependency install: ~27 s
- bootstrap: ~9 s
- patched QEMU rebuild: ~4 min
- generic plugin-debug smoke (including firmware build): ~6 min
- Network Manager 3-mode E2E itself: ~42 s

The target E2E was not the bottleneck. Recreating the environment and recompiling QEMU/firmware were.

## Required fast-loop architecture

Use two loops:

### 1. Fast development loop

For ordinary firmware edits:

1. Reuse an existing patched QEMU binary. Build QEMU only when the pinned upstream QEMU or Murphy patch series changes.
2. Reuse PlatformIO packages/toolchain.
3. Use `PLATFORMIO_BUILD_CACHE_DIR` for compiler-object reuse.
4. Build `murphy_m4_qemu_plugin` once.
5. Run only the smallest relevant E2E/contract for the files changed.
6. Do not run the full generic smoke before every targeted test.

Typical target: feedback in roughly 1-3 minutes on a warm cache.

### 2. Merge / freeze loop

Before declaring a firmware task done:

1. relevant host/unit/contracts
2. generic m4sim smoke
3. relevant real-Home E2E journey (when one exists)
4. full `pio run -e murphy_m4`
5. real-device smoke for hardware-only behaviour

## GitHub CI entry points

There are intentionally two different workflows:

### Daily feedback: `.github/workflows/m4-fast.yml`

Use this as the normal firmware-development signal.

It:

- caches the patched QEMU build/runtime independently from firmware source
- caches `~/.platformio` and `PLATFORMIO_BUILD_CACHE_DIR`
- builds `murphy_m4_qemu_plugin` once
- runs smoke with `--skip-build`
- runs Network Manager E2E with `--skip-build`
- uploads only short-lived diagnostics

A new agent should use this result to continue ordinary implementation. **Do not wait for the full m4sim gate before every edit.**

### Checkpoint gate: `.github/workflows/m4sim-smoke.yml`

This is the slower, last-known-good full simulator gate. Keep it conservative and use it when declaring a checkpoint/release/freeze result. Do not casually refactor it just to shave seconds from the daily loop.

## Persistent local caches

Preferred local paths:

```text
~/.cache/murphy-m4/espressif-qemu-v3
~/.cache/murphy-m4/platformio-build-cache
~/.platformio
```

Set:

```bash
export PATH="$HOME/.local/bin:$HOME/.platformio/penv/bin:$PATH"
export PLATFORMIO_BUILD_CACHE_DIR="$HOME/.cache/murphy-m4/platformio-build-cache"
```

Patched QEMU should normally resolve to one of:

```text
~/.cache/murphy-m4/espressif-qemu-v3/build-murphy-v3/qemu-system-xtensa
~/.cache/murphy-m4/espressif-qemu-v3/build-murphy-v3/qemu-system-xtensa-unsigned
```

Only rebuild it if missing or if one of these changes:

- `simulator/qemu/patches/upstream.json`
- `simulator/qemu/patches/series-v3`
- any file under `simulator/qemu/patches/`
- `simulator/qemu/build.py`
- `simulator/qemu/build_patched_qemu_v3.py`

## Fast commands

Bootstrap only when dependency copies are absent:

```bash
bash scripts/bootstrap_deps.sh
```

Build simulator firmware once:

```bash
cd firmware
pio run -e murphy_m4_qemu_plugin
cd ..
```

Then reuse that firmware build for tests:

```bash
./m4sim test smoke --plugin-debug --skip-build --ready-seconds 90
./m4sim test network-manager --plugin-debug --skip-build --ready-seconds 90
```

Production compile gate:

```bash
cd firmware
pio run -e murphy_m4
```

Host simulator tests:

```bash
cmake -S simulator -B simulator/build
cmake --build simulator/build -j
ctest --test-dir simulator/build --output-on-failure
```

## GitHub Actions cache contract

CI should use cache keys with different invalidation rules.

### Patched QEMU runtime/build cache

QEMU cache key must depend on:

```text
runner OS (and arch if heterogeneous runners are introduced)
hash(simulator/qemu/build.py)
hash(simulator/qemu/build_patched_qemu_v3.py)
hash(simulator/qemu/patches/**)
```

**Firmware source changes must not invalidate the QEMU cache.**

On an exact QEMU cache hit, skip `build.py` entirely. Do not restore a QEMU build and then run `git clean -fdx` / `--reconfigure`, because the builder intentionally deletes its build directory.

### PlatformIO cache

Use `PLATFORMIO_BUILD_CACHE_DIR` and cache it across runs. Its stable compatibility key is tied to `firmware/platformio.ini` and the runner platform. The cache provides a baseline of reusable compiler objects; source files changed after that baseline are recompiled normally.

Also cache `~/.platformio` (packages/platform downloads) with `firmware/platformio.ini` as an invalidation input.

## Docker policy

A container image is useful for reproducibility, but it is not the first or largest speedup.

A future image such as `ghcr.io/einklover/m4sim-ci:<version>` should contain stable host tooling only:

- Ubuntu userspace
- compiler/build tools, Ninja/Meson
- QEMU host libraries
- Python + pyserial
- PlatformIO CLI

Do **not** bake fast-changing Murphy QEMU patch output or firmware binaries into the base image. Keep those as caches/artifacts keyed by their real inputs. This avoids rebuilding a large image for each QEMU patch or firmware edit.

Only introduce the Docker/GHCR layer after the cache-based fast loop is measured. The purpose of Docker is reproducibility and removing repeated apt/pip setup; it should not become a new source of image rebuild latency.

## Test selection rule for agents

Do not run every expensive test after every edit. Select by impact:

- pure parser/algorithm change -> host/unit/contract first
- UI/activity/navigation change -> targeted m4sim journey
- Network Manager -> `./m4sim test network-manager ...`
- QEMU machine/patch change -> rebuild QEMU + generic smoke + targeted journey
- production-only hardware/resource change -> production build + real-device smoke
- cross-cutting or release-ready change -> full merge/freeze loop

If a targeted test passes, continue implementation without waiting for unrelated full smoke. Run the full gate only at checkpoints.

## 2026-08-13 Network Manager freeze checkpoint

The Network Manager gate has passed after fixing the Calibre/QEMU network compatibility path:

- generic plugin-debug smoke: PASS
- Network Manager real Home UI 3-mode E2E: PASS
- Network Manager E2E runtime itself: ~42 s

The key firmware fix uses the existing QEMU network compatibility helper instead of treating `WiFi.status()` as the sole connectivity source in the Calibre path. QEMU `open_eth` therefore no longer falls into an unmodelled ESP Wi-Fi radio scan.

After this checkpoint, do not expand m4sim unless a future production firmware change proves a simulator correctness gap.

## What a new AI session should do

When the user says “continue” or asks to modify M4 firmware:

1. read `AGENTS.md`, `HANDOFF.md`, and this file
2. inspect current branch / recent commits and CI before changing code
3. identify the smallest relevant test
4. preserve QEMU and PlatformIO caches
5. make the firmware change
6. use `m4 fast firmware loop` / targeted local commands for feedback
7. continue implementation once the fast signal is green; do not block on unrelated full gates
8. run the full gate only at the task checkpoint
9. update `HANDOFF.md` when the current task/status materially changes

The repository, not prior chat history, is the durable handoff mechanism.
