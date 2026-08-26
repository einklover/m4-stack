# M4 Stack Clean Main Rebuild Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce a reproducible, AI-friendly Murphy M4-only repository baseline, verify it from a clean clone, and promote the verified branch tip to GitHub `main` while preserving history.

**Architecture:** Keep large SDK/font/vendor trees reconstructed from the pinned `einklover/m4-device@f86b134` archive rather than committing them. Make the M4 build self-bootstrap before local library discovery, then provide one canonical documentation path for humans/agents and remove host-specific or stale simulator assumptions that break clean clones. Preserve the already hardware-tested buzzer/direct-boot/shutdown-wallpaper behavior unchanged.

**Tech Stack:** ESP32-S3 / Arduino + ESP-IDF through PlatformIO, Python 3 helper scripts and contract tests, Bash bootstrap/flash tools, CMake host simulator, patched Espressif QEMU v3, Git/GitHub.

**Spec:** `docs/superpowers/specs/2026-08-26-main-rebuild-design.md`

## Global Constraints

- Production hardware target is Murphy M4; production build environment is `murphy_m4`.
- QEMU build environments must never be flashed to hardware.
- Ordinary hardware flashing is APP1-only; never write/erase bootloader, partition table, NVS, or original APP0 without explicit human approval.
- Reconstructed dependencies remain pinned to `einklover/m4-device@f86b134` unless the spec is explicitly revised.
- Do not vendor the ~159 MB `firmware/lib/EpdFont/builtinFonts/` tree into Git in this rebuild.
- Existing GPIO46 buzzer sanitizer, direct Home/Reader startup, and default shutdown wallpaper behavior must not regress.
- Hardware validation claims require hardware evidence; simulator/QEMU evidence must be labeled as such.
- No secrets, device captures, local absolute paths, or generated build outputs may be committed.
- Use single-process production builds (`-j1`) for final evidence to avoid the known concurrent `.pio` deletion race.

---

### Task 1: Freeze and re-verify the current tested M4 firmware delta

**Files:**
- Modify: `firmware/src/main.cpp`
- Modify: `firmware/src/activities/boot_sleep/SleepActivity.cpp`
- Modify: `firmware/src/util/CrosslinkDefaultWallpaper.h`
- Add/Test: `firmware/tests/test_m4_buzzer_boot_contract.py`
- Add/Test: `firmware/tests/test_m4_direct_boot_contract.py`
- Add/Test: `firmware/tests/test_m4_shutdown_wallpaper_contract.py`

**Interfaces:**
- Consumes: current uncommitted firmware state already flashed/tested on the M4.
- Produces: one reviewed commit containing only the known buzzer/direct-boot/shutdown-wallpaper firmware changes and their contracts.

- [ ] **Step 1: Inspect the exact current diff and tests; do not edit behavior unless a contract is malformed.**

Run:
```bash
git diff -- firmware/src/main.cpp firmware/src/activities/boot_sleep/SleepActivity.cpp firmware/src/util/CrosslinkDefaultWallpaper.h
sed -n '1,220p' firmware/tests/test_m4_buzzer_boot_contract.py
sed -n '1,220p' firmware/tests/test_m4_direct_boot_contract.py
sed -n '1,220p' firmware/tests/test_m4_shutdown_wallpaper_contract.py
```

- [ ] **Step 2: Run the three focused contracts.**

Run:
```bash
python3 firmware/tests/test_m4_buzzer_boot_contract.py
python3 firmware/tests/test_m4_direct_boot_contract.py
python3 firmware/tests/test_m4_shutdown_wallpaper_contract.py
```
Expected: all print `PASS` and exit 0.

- [ ] **Step 3: Verify whitespace and production build without changing behavior.**

Run:
```bash
git diff --check
cd firmware
PATH="$HOME/.local/bin:$HOME/.platformio/penv/bin:$PATH" \
PLATFORMIO_BUILD_CACHE_DIR="$HOME/.cache/murphy-m4/platformio-build-cache" \
pio run -e murphy_m4 -j1
```
Expected: build SUCCESS.

- [ ] **Step 4: Record firmware artifact evidence.**

Run from `firmware/`:
```bash
shasum -a 256 .pio/build/murphy_m4/firmware.bin
stat -f '%z bytes' .pio/build/murphy_m4/firmware.bin 2>/dev/null || stat -c '%s bytes' .pio/build/murphy_m4/firmware.bin
```

- [ ] **Step 5: Commit only this firmware delta and its three tests.**

```bash
git add firmware/src/main.cpp \
        firmware/src/activities/boot_sleep/SleepActivity.cpp \
        firmware/src/util/CrosslinkDefaultWallpaper.h \
        firmware/tests/test_m4_buzzer_boot_contract.py \
        firmware/tests/test_m4_direct_boot_contract.py \
        firmware/tests/test_m4_shutdown_wallpaper_contract.py
git commit -m "fix(m4): finalize boot and shutdown behavior"
```

---

### Task 2: Make a clean M4 clone self-bootstrap its reconstructed build dependencies

**Files:**
- Add: `firmware/scripts/bootstrap_m4_deps.py`
- Modify: `firmware/platformio.ini`
- Modify: `scripts/bootstrap_deps.sh`
- Modify: `.gitignore`
- Modify: `firmware/.gitignore`
- Add/Test: `firmware/tests/test_m4_dependency_bootstrap_contract.py`

**Interfaces:**
- Produces: `missing_dependencies(firmware_dir: Path) -> list[str]` and `ensure_dependencies(firmware_dir: Path, runner=subprocess.run) -> bool` in `bootstrap_m4_deps.py`.
- Consumes: root `scripts/bootstrap_deps.sh`, pinned archive `einklover/m4-device@f86b134`.
- Later tasks rely on: `cd firmware && pio run -e murphy_m4` succeeding from a clone with reconstructed directories absent.

- [ ] **Step 1: Normalize the interrupted dependency contract and run it RED.**

The contract must require these sentinels:
```python
REQUIRED_SENTINELS = (
    "open-m4-sdk/libs/hardware/BatteryMonitor/library.json",
    "open-m4-sdk/libs/hardware/InputManager/library.json",
    "open-m4-sdk/libs/display/FreeInkDisplay/library.json",
    "open-m4-sdk/libs/hardware/SDCardManager/library.json",
    "open-m4-sdk/libs/hardware/BoardConfig/library.json",
    "open-m4-sdk/libs/hardware/FrontlightManager/library.json",
    "open-m4-sdk/libs/hardware/PowerManager/library.json",
    "src/network/updater_fw.bin",
    "lib/Epub/Epub.h",
    "lib/Lua/src/lua.h",
    "lib/expat/expat.h",
    "lib/miniz/miniz.h",
    "lib/picojpeg/picojpeg.h",
    "lib/EpdFont/builtinFonts/all.h",
)
```
It must verify: missing sentinel => bootstrap runner invoked once; complete tree => no runner/network; the M4 environment wires the helper; X3/X4 environments do not.

Run:
```bash
python3 firmware/tests/test_m4_dependency_bootstrap_contract.py
```
Expected RED: helper/config wiring absent.

- [ ] **Step 2: Implement an importable helper with no SCons dependency at import time.**

`firmware/scripts/bootstrap_m4_deps.py` must expose:
```python
from pathlib import Path
import subprocess

REQUIRED_SENTINELS = (...)

def missing_dependencies(firmware_dir: Path) -> list[str]:
    return [rel for rel in REQUIRED_SENTINELS if not (firmware_dir / rel).is_file()]

def ensure_dependencies(firmware_dir: Path, runner=subprocess.run) -> bool:
    missing = missing_dependencies(firmware_dir)
    if not missing:
        return False
    repo_root = firmware_dir.parent
    bootstrap = repo_root / "scripts" / "bootstrap_deps.sh"
    if not bootstrap.is_file():
        raise RuntimeError(f"missing bootstrap script: {bootstrap}")
    runner(["bash", str(bootstrap)], cwd=repo_root, check=True)
    remaining = missing_dependencies(firmware_dir)
    if remaining:
        raise RuntimeError("bootstrap completed but dependencies are still missing: " + ", ".join(remaining))
    return True
```
The PlatformIO execution tail may import `env` only under an execution guard so unit import remains possible.

- [ ] **Step 3: Wire bootstrap early enough for M4 library discovery and embedded updater input.**

First try `pre:scripts/bootstrap_m4_deps.py` in `[m4_base]`. If PlatformIO attempts to resolve `file://open-m4-sdk/...` before the pre-script runs, replace those M4-only `file://` package dependencies with library discovery through reconstructed local SDK directories (for example `lib_extra_dirs` pointing at `open-m4-sdk/libs/hardware` and `open-m4-sdk/libs/display`) so the pre-script executes before LDF scans them. Do not alter legacy X3/X4 profiles.

This step is complete only when the realistic missing-dependency test in Step 6 passes; a unit contract alone is insufficient.

- [ ] **Step 4: Harden the root bootstrap enough to fail clearly and remain pinned.**

Keep `M4_DEVICE_SHA=f86b134`. Validate all sentinels after copy. Preserve the current QEMU-only `InputManager` patch behavior. Print the exact archive pin and actionable network/tool failure context. Do not download when the helper determines all sentinels already exist.

- [ ] **Step 5: Fix ignore comments to describe reconstructed dependencies accurately.**

Replace stale “absolute-path symlinks” wording. The ignored dependency paths remain ignored and are described as generated/reconstructed from the pinned archive.

- [ ] **Step 6: Prove actual clean-clone ordering, not just helper logic.**

Use a temporary clone/worktree derived from the branch commit content, remove/ensure absence of all reconstructed dependency paths and `.pio`, then run:
```bash
cd firmware
PATH="$HOME/.local/bin:$HOME/.platformio/penv/bin:$PATH" \
pio run -e murphy_m4 -j1
```
Expected evidence in output: automatic bootstrap runs once, reconstructs all sentinels, and the same invocation continues to a successful build. A second invocation with the populated tree must not invoke bootstrap/network again.

- [ ] **Step 7: Run contract and diff checks.**

```bash
python3 firmware/tests/test_m4_dependency_bootstrap_contract.py
git diff --check
```
Expected: PASS / no output from diff check.

- [ ] **Step 8: Commit portability work.**

```bash
git add firmware/scripts/bootstrap_m4_deps.py firmware/platformio.ini \
        scripts/bootstrap_deps.sh .gitignore firmware/.gitignore \
        firmware/tests/test_m4_dependency_bootstrap_contract.py
git commit -m "build(m4): self-bootstrap clean clone dependencies"
```

---

### Task 3: Remove host-specific defaults and make Murphy M4 the obvious production path

**Files:**
- Modify: `firmware/platformio.ini`
- Test: add a focused static contract under `firmware/tests/` if no existing test covers these config invariants.

**Interfaces:**
- Produces: clean, host-independent production configuration.
- Later docs may state `murphy_m4` as the default/primary production target only after this task passes.

- [ ] **Step 1: Write a failing config contract.**

Assert:
- `default_envs` is `murphy_m4` (or omitted only if README always gives explicit environment; prefer `murphy_m4` for this M4-only main).
- no committed private RFC1918 endpoint such as `192.168.0.195` remains in production/plugin-debug defaults.
- QEMU environment names remain distinct from production.

Run the contract and verify RED on the current config.

- [ ] **Step 2: Make the minimal config change.**

Set:
```ini
[platformio]
default_envs = murphy_m4
```
Replace the committed screen-bridge endpoint with an opt-in environment/config value or loopback-safe disabled default; do not bake a developer LAN address into Git.

- [ ] **Step 3: Run config contract and production build.**

```bash
python3 firmware/tests/test_m4_platformio_contract.py
cd firmware && PATH="$HOME/.local/bin:$HOME/.platformio/penv/bin:$PATH" pio run -e murphy_m4 -j1
```
Expected: PASS/SUCCESS.

- [ ] **Step 4: Commit.**

```bash
git add firmware/platformio.ini firmware/tests/test_m4_platformio_contract.py
git commit -m "build(m4): make production defaults portable"
```

---

### Task 4: Create the canonical human/AI onboarding documentation

**Files:**
- Modify: `README.md`
- Modify: `AGENTS.md`
- Modify: `HANDOFF.md`
- Modify: `VERSIONS.md`
- Add: `docs/AI_QUICKSTART.md`
- Add: `docs/BUILD_AND_DEPS.md`
- Add: `docs/DEVICE_AND_M4ADB.md`
- Add: `docs/TROUBLESHOOTING.md`

**Interfaces:**
- Consumes: verified commands/behavior from Tasks 1-3.
- Produces: canonical links used by later simulator/plugin docs.

- [ ] **Step 1: Rewrite README as the verified 5-minute human path.**

It must contain, in this order: project scope (Murphy M4), requirements, clone/build command, dependency auto-bootstrap explanation, simulator entry command/link, APP1-only flash warning, repository map, deeper docs links. Avoid historical QEMU conclusions in README.

- [ ] **Step 2: Reduce AGENTS.md to an AI operating contract.**

Required first five-minute sequence:
```bash
git status --short --branch
git log -5 --oneline
python3 firmware/tests/test_m4_dependency_bootstrap_contract.py
cd firmware && pio run -e murphy_m4 -j1
```
Also state: never discard unknown dirty changes; production vs QEMU profile distinction; one m4adb owner; APP1-only safety; hardware-evidence rule; use targeted tests and caches.

- [ ] **Step 3: Add `docs/AI_QUICKSTART.md`.**

Explain repository map, first files to read, how to choose firmware/simulator/plugin work, how to gather evidence, and a small “before you edit / before you claim done” checklist.

- [ ] **Step 4: Add `docs/BUILD_AND_DEPS.md`.**

Document all reconstructed paths, exact pin `f86b134`, automatic flow, manual `bash scripts/bootstrap_deps.sh`, PlatformIO dependencies/cache, prerequisites (`curl`, `tar`, Python, PlatformIO), and clean-clone troubleshooting.

- [ ] **Step 5: Add `docs/DEVICE_AND_M4ADB.md`.**

Document one-daemon/single-owner rule, status/basic commands from `firmware/scripts/m4adb.py --help`, USB re-enumeration, official APP0 causing custom CDC disappearance, APP1 offset/safety at a conceptual level, and `scripts/flash_app1_once.sh` as the supported flash entry point. Do not document unsafe full-flash instructions as the normal path.

- [ ] **Step 6: Add `docs/TROUBLESHOOTING.md`.**

At minimum include: missing reconstructed libs, PlatformIO not found, `.pio` directory disappearing due concurrent builds, m4adb port ownership, custom CDC absent under official firmware, QEMU TCP/PTY not equal to protocol readiness, stale local network endpoint, and simulator-vs-hardware evidence distinction.

- [ ] **Step 7: Refresh HANDOFF and VERSIONS.**

HANDOFF becomes dated current-state pointers, not duplicated long instructions. VERSIONS records current monorepo branch baseline and dependency/QEMU pins with date 2026-08-26.

- [ ] **Step 8: Validate docs mechanically.**

Run:
```bash
rg -n '/Users/|/Volumes/.*/worktrees|wap-checkpoint|192\.168\.' README.md AGENTS.md HANDOFF.md VERSIONS.md docs/AI_QUICKSTART.md docs/BUILD_AND_DEPS.md docs/DEVICE_AND_M4ADB.md docs/TROUBLESHOOTING.md
git diff --check
```
Expected: no local-only paths/endpoints in canonical docs; diff check clean.

- [ ] **Step 9: Commit.**

```bash
git add README.md AGENTS.md HANDOFF.md VERSIONS.md \
        docs/AI_QUICKSTART.md docs/BUILD_AND_DEPS.md \
        docs/DEVICE_AND_M4ADB.md docs/TROUBLESHOOTING.md
git commit -m "docs: add canonical M4 and AI onboarding"
```

---

### Task 5: Canonicalize simulator/QEMU guidance and remove stale monorepo path assumptions

**Files:**
- Add or Rewrite: `docs/SIMULATOR.md`
- Modify: `simulator/README.md`
- Modify: `simulator/AGENTS.md`
- Modify: `simulator/qemu/README.md`
- Modify: `simulator/qemu/LOCAL_RUNTIME_CHAIN.md`
- Modify only as needed: `simulator/qemu/run_murphy_bin.py`
- Modify only as needed: `simulator/tools/ai_debug.py`
- Modify only as needed: `simulator/docs/AI_DEBUG_INTERFACE.md`
- Modify only as needed: related simulator tests encoding old paths

**Interfaces:**
- Produces: one QEMU v3/m4sim source of truth; scripts resolve the monorepo firmware path without old `wap-checkpoint`/separate-repo defaults.

- [ ] **Step 1: Write/identify a failing test for old path assumptions before changing runtime scripts.**

Search:
```bash
rg -n 'wap-checkpoint|murphy-m4-simulator|run_plugin_debug|build_patched_qemu_v2|../firmware' simulator docs
```
For runtime code with hardcoded old locations, add/update tests so monorepo-root-relative discovery is required.

- [ ] **Step 2: Make runtime path resolution repository-relative.**

Python tools should derive the repo root from `Path(__file__).resolve()` and use `<repo>/firmware`, while still allowing an explicit CLI override where already supported. Do not silently depend on sibling repositories.

- [ ] **Step 3: Establish v3 as canonical.**

`docs/SIMULATOR.md` must explain:
- host simulator vs patched QEMU roles;
- `./m4sim info`, build, run, targeted tests and `--skip-build` workflow using actual CLI help;
- patched QEMU v3 as source of truth;
- readiness requires m4adb `ping` containing both `protocol` and `firmware`;
- QEMU profiles are not hardware firmware;
- known emulation limitations are current and linked to historical evidence rather than restated as old “entry hang” conclusions.

- [ ] **Step 4: Turn old handoffs into explicitly historical references, not competing instructions.**

Do not delete useful stage evidence. Add a prominent historical/stale marker and point to `docs/SIMULATOR.md` where an old document would otherwise mislead a new agent.

- [ ] **Step 5: Run simulator tests/smoke using the documented command.**

Choose the smallest reliable current smoke path based on `./m4sim --help`/existing CI, then execute it. Reuse QEMU/build caches and `--skip-build` after one build where applicable.

- [ ] **Step 6: Scan for remaining stale executable defaults.**

```bash
rg -n 'wap-checkpoint|murphy-m4-simulator|build_patched_qemu_v2|192\.168\.' simulator docs/SIMULATOR.md
```
Any remaining hits must be either historical text clearly labeled as such or intentional test fixtures, not active defaults.

- [ ] **Step 7: Commit simulator cleanup.**

```bash
git add docs/SIMULATOR.md simulator
git commit -m "docs(sim): canonicalize M4 simulator workflow"
```

---

### Task 6: Consolidate plugin development/package/install guidance

**Files:**
- Add: `docs/PLUGINS.md`
- Modify as needed: `plugins/*/README.md`
- Test/inspect: `plugins/*/tools/package.py`

**Interfaces:**
- Produces: one plugin overview linking each plugin’s own README and actual package scripts/outputs.

- [ ] **Step 1: Inventory actual plugins and packaging commands.**

Run:
```bash
find plugins -maxdepth 2 -name README.md -print
find plugins -path '*/tools/package.py' -print
```
Inspect package script help/source before documenting command/output names.

- [ ] **Step 2: Write `docs/PLUGINS.md`.**

Cover current plugin directories, package command pattern, `.m4x` artifacts, device install flow through `app_inbox`, provider/plugin debugging entry points, and which simulator/QEMU tests can exercise plugins. Distinguish screen-bridge/Android components if they are not part of the normal reader plugin release.

- [ ] **Step 3: Correct stale plugin README paths only where verified.**

In particular, remove nonexistent `publish/...` and `fengyan-m4-device-rc1/...` examples from the JJWXC README and replace them with actual monorepo commands/paths.

- [ ] **Step 4: Package representative plugins without publishing.**

Use their existing `tools/package.py` commands to create local ignored artifacts, confirm success, then leave generated `.m4x` files untracked/ignored.

- [ ] **Step 5: Commit plugin docs.**

```bash
git add docs/PLUGINS.md plugins/*/README.md
git commit -m "docs(plugins): add reproducible development guide"
```

---

### Task 7: Final clean-clone verification and promotion preparation

**Files:**
- Modify only if verification uncovers a defect: files from Tasks 2-6.
- Add: optionally `docs/RELEASE_CHECKLIST.md` only if it removes duplicated promotion steps; otherwise keep evidence in commit/terminal report.

**Interfaces:**
- Produces: verified branch tip suitable for remote `main` promotion.

- [ ] **Step 1: Run all focused repository contracts from the rebuilt branch.**

```bash
python3 firmware/tests/test_m4_buzzer_boot_contract.py
python3 firmware/tests/test_m4_direct_boot_contract.py
python3 firmware/tests/test_m4_shutdown_wallpaper_contract.py
python3 firmware/tests/test_m4_dependency_bootstrap_contract.py
python3 firmware/tests/test_m4_platformio_contract.py
```
Expected: all PASS.

- [ ] **Step 2: Run final production build single-threaded.**

```bash
cd firmware
PATH="$HOME/.local/bin:$HOME/.platformio/penv/bin:$PATH" \
PLATFORMIO_BUILD_CACHE_DIR="$HOME/.cache/murphy-m4/platformio-build-cache" \
pio run -e murphy_m4 -j1
```
Record size/SHA-256.

- [ ] **Step 3: Run the canonical simulator minimum validation documented in Task 5.**

Expected: documented smoke/test passes. Record whether evidence is host simulator or QEMU.

- [ ] **Step 4: Create a completely separate temporary clone from the branch tip and prove onboarding.**

The clone must start without ignored reconstructed dependencies or `.pio`. Run the README build path verbatim and confirm automatic dependency reconstruction and successful `murphy_m4` production build. Run a second build and confirm bootstrap is a no-op.

- [ ] **Step 5: Repository hygiene audit.**

```bash
git status --short
git diff --check
rg -n '/Users/[^ )`]+|/Volumes/.*/worktrees|192\.168\.' --glob '!docs/history/**' --glob '!docs/emulator-stages/**' .
git ls-files -s | awk '$1 == 120000 {print}'
```
Investigate every local-path/private-endpoint hit and every tracked symlink. Generated dependency/build/package artifacts must remain untracked/ignored.

- [ ] **Step 6: Confirm remote main has not moved since rebuild base/promotion check.**

```bash
git fetch origin
git rev-parse origin/main
git log --oneline --decorate -5
```
If remote `main` moved unexpectedly, stop promotion and reconcile; do not blindly overwrite somebody else’s new commits.

- [ ] **Step 7: Create a safety tag for the pre-rebuild main tip and push rebuild branch.**

Assuming old main remains `e6da618095efdd9a6be431f82b1820e66c286ad4`:
```bash
git tag -a pre-main-rebuild-20260826 e6da618095efdd9a6be431f82b1820e66c286ad4 -m "main before M4 clean rebuild"
git push origin pre-main-rebuild-20260826
git push -u origin main-rebuild-20260826
```

- [ ] **Step 8: Promote the verified branch tip to remote `main` with lease protection.**

Only after all previous evidence is green and `origin/main` still matches the expected old tip:
```bash
git push origin main-rebuild-20260826:main --force-with-lease=main:e6da618095efdd9a6be431f82b1820e66c286ad4
```
Then:
```bash
git fetch origin
test "$(git rev-parse origin/main)" = "$(git rev-parse main-rebuild-20260826)"
```
Expected: equality succeeds.

- [ ] **Step 9: Final promotion report.**

Report branch tip, old-main safety tag, production firmware SHA/size, clean-clone build evidence, simulator/QEMU evidence, and any explicitly parked non-blocking limitations. Do not claim hardware revalidation unless a device test was actually performed during this rebuild.
