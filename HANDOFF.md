# Murphy M4 session entry

Last updated: **2026-08-26**

This is a pointer for a new session. Detailed goals, progress, measurements, and acceptance evidence belong in the active work item or task report.

## Start here

1. `AGENTS.md` — AI operating contract and safety rules
2. `HANDOFF.md` — current branch and stable entry points
3. `docs/FAST_FIRMWARE_DEV.md` — build/test/cache workflow
4. the active task or issue named by the work request
5. task-specific code and docs

## Current branch

```text
main-rebuild-20260826
```

The current baseline is the Task 4 documentation point at `bc3038955108eb7573398894eeb63f522b3d0b8b` before the documentation commit.

## Stable entry points

- Production build: `cd firmware && pio run -e murphy_m4 -j1`
- Dependency contract: `python3 firmware/tests/test_m4_dependency_bootstrap_contract.py`
- Manual dependency bootstrap: `bash scripts/bootstrap_deps.sh`
- Simulator smoke: `./m4sim test smoke --plugin-debug`
- Device flash: `firmware/scripts/flash_app1_once.sh` (APP1-only)
- Device control: `python3 firmware/scripts/m4adb.py --help`

The production profile and both QEMU profiles are intentionally distinct. Use `murphy_m4` for hardware and `murphy_m4_qemu` / `murphy_m4_qemu_plugin` only through the simulator workflow. Treat QEMU and host-test results as simulator evidence, not hardware validation.

## Canonical guides

- Human entry: `README.md`
- AI workflow: `docs/AI_QUICKSTART.md`
- Build/dependency behavior: `docs/BUILD_AND_DEPS.md`
- Device and serial lifecycle: `docs/DEVICE_AND_M4ADB.md`
- Known failures: `docs/TROUBLESHOOTING.md`
- Fast loop: `docs/FAST_FIRMWARE_DEV.md`
