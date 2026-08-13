# Murphy M4 Stack — Agent rules

This monorepo is the **default cwd and durable source of truth** for new AI / Codex work on Murphy M4.

Do not rely on previous chat history when the repository contains a current handoff.

## Mandatory first read for every new session

Read in this order before changing code:

1. `AGENTS.md` — rules and safety boundaries
2. `HANDOFF.md` — current checkpoint / what remains
3. `docs/FAST_FIRMWARE_DEV.md` — fast build/test/cache workflow
4. task-specific docs/code only after the above

If the task materially changes current status, update `HANDOFF.md` before ending the work checkpoint.

## Layout

| Path | What |
|------|------|
| `firmware/` | m4-firmware (ESP32-S3 / PlatformIO `murphy_m4`) |
| `simulator/` | Murphy M4 host/QEMU simulator + E2E journeys |
| `plugins/` | fanqie / jjwxc / weread (+ legado) |
| `scripts/` | bootstrap + cross-cutting helpers |
| `docs/` | persistent handoffs and development contracts |
| `VERSIONS.md` | pinned upstream SHAs |

Upstream component repos still exist (`einklover/m4-firmware` etc.). Prefer committing here for multi-component work; mirror important fixes back to component repos when releasing.

## Non-negotiables

1. **Main thread implements** unless user asks for subagents. Do not silently spawn other agents for implementation.
2. **Firmware changes**: run the smallest relevant host/m4sim test first when possible; use the full gate only at checkpoints. Follow `docs/FAST_FIRMWARE_DEV.md`.
3. **Production flash is APP1-only** via `firmware/scripts/murphy_m4_app1_flash.py` or `flash_app1_once.sh`. Never flash APP0 / bootloader / full erase without explicit user approval.
4. **m4adb**: single global daemon; never `pkill -f m4adb.py`; device I/O only through repository m4adb tooling.
5. **Never flash QEMU profiles to hardware** (`murphy_m4_qemu`, `murphy_m4_qemu_plugin`).
6. **Do not commit** reconstructed `open-m4-sdk` / `builtinFonts` / `.pio` / firmware binaries / plugin `.m4x` binaries.
7. **m4sim is frozen after the 2026-08-13 Network Manager gate.** Do not expand simulator scope unless a future production firmware change proves a simulator correctness gap.
8. **Preserve constrained-device design**: large HTTP/font/catalog work must remain bounded-memory, streaming-first, PSRAM-aware, and must not reintroduce full-body/full-font heap loads.

## First-time setup

```bash
bash scripts/bootstrap_deps.sh
export PATH="$HOME/.local/bin:$HOME/.platformio/penv/bin:$PATH"
export PLATFORMIO_BUILD_CACHE_DIR="$HOME/.cache/murphy-m4/platformio-build-cache"
```

Do not bootstrap on every iteration if dependencies are already present.

## Fast build / test entry points

See `docs/FAST_FIRMWARE_DEV.md` for the full contract. The important rule is **build once, then reuse**.

```bash
# Incremental QEMU-plugin firmware build
cd firmware && pio run -e murphy_m4_qemu_plugin && cd ..

# Reuse the same firmware build for journeys
./m4sim test smoke --plugin-debug --skip-build --ready-seconds 90
./m4sim test network-manager --plugin-debug --skip-build --ready-seconds 90

# Production compile gate
cd firmware && pio run -e murphy_m4
```

Patched QEMU should normally be reused from:

```text
~/.cache/murphy-m4/espressif-qemu-v3/
```

Do not rebuild patched QEMU for ordinary firmware changes.

## Current simulator checkpoint

As of 2026-08-13:

- patched QEMU `murphy-m4` machine boots the plugin-debug firmware
- m4adb control path works
- generic plugin-debug smoke passes
- Network Manager real-Home 3-mode E2E passes
- m4sim is therefore considered **frozen/stable enough for firmware development**

Historical QEMU boot documents may describe earlier blockers; prefer `HANDOFF.md` and `docs/FAST_FIRMWARE_DEV.md` for current state.

## Where to start reading

1. `HANDOFF.md` — current task/status
2. `docs/FAST_FIRMWARE_DEV.md` — fastest correct development loop
3. `README.md` — repository map
4. `simulator/docs/CODEX_FIRMWARE_DEBUG_GUIDE.md` — simulator debugging details
5. historical docs only when investigating old regressions

## Plugins / network pitfalls

- Prefer host/native streaming paths for large JSON/catalogs; avoid monolithic bodies on constrained heap.
- Do not send duplicate `User-Agent` if host transport already sets one.
- Catalog/list row IDs: use stable provider IDs/index tables, not display title as identity.
- Wi-Fi drops on device: use repository m4adb Wi-Fi preparation flow before HTTP install/debug.
- Under QEMU plugin-debug, network availability may come from the existing QEMU/open_eth compatibility helper rather than ESP `WiFi.status()` alone.
