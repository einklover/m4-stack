# Murphy M4 AI operating contract

This monorepo is the source of truth for Murphy M4 firmware, simulator, and plugins. Work in the current worktree and preserve the repository's boundaries.

## First five minutes

Run these checks before editing:

```bash
git status --short --branch
git log -5 --oneline
python3 firmware/tests/test_m4_dependency_bootstrap_contract.py
cd firmware && pio run -e murphy_m4 -j1
```

Read `HANDOFF.md`, `docs/FAST_FIRMWARE_DEV.md`, and the task-specific docs before choosing a broader test. Inspect the current branch and dirty state; never discard, reset, overwrite, or clean unknown user changes.

## Build and test contract

- `murphy_m4` is the production hardware environment and the default PlatformIO environment.
- `murphy_m4_qemu` and `murphy_m4_qemu_plugin` are simulator-only profiles. Never flash either profile to a device.
- Use the smallest relevant host, contract, simulator, or plugin test first. Reuse PlatformIO and patched-QEMU caches; build once and use `--skip-build` for subsequent journeys where supported.
- A production compile is not hardware evidence. A host model or QEMU pass is not hardware evidence. Claim device behavior only with a real-device result and record the command and artifact.
- Do not commit reconstructed SDK/library trees, `.pio`, firmware binaries, plugin `.m4x` packages, credentials, or device captures.

## Device safety

- Production flashing is APP1-only through `firmware/scripts/flash_app1_once.sh` or the checked helper `firmware/scripts/murphy_m4_app1_flash.py`.
- Do not write APP0, the bootloader, partition table, NVS, or full flash without explicit human approval. The APP1 application offset is part of the production partition contract.
- Keep one global `m4adb` daemon/serial owner. Use repository `m4adb` tooling for device I/O; do not start competing owners and do not use `pkill -f m4adb.py`.
- USB re-enumeration after reset or slot changes is expected. Rediscover the port and reconnect the existing owner instead of launching another daemon.

## Evidence and scope

Keep firmware changes bounded-memory and streaming-first. Do not add private network endpoints to canonical configuration or docs. Update the relevant task/issue evidence when the task authorizes it; keep `HANDOFF.md` as a short pointer, not a history log. Do not flash hardware, publish, push, or modify GitHub unless the current task explicitly authorizes it.
