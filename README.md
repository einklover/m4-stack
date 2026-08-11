# m4-stack — Murphy M4 agent-ready monorepo

Single tree for **firmware + host simulator + plugins**, pinned for AI / Codex / human onboarding.

- **GitHub**: https://github.com/einklover/m4-stack  
- **Component upstreams**: `m4-firmware`, `murphy-m4-simulator`, `m4-*-plugin` (see `VERSIONS.md`)

## Quick start

```bash
git clone https://github.com/einklover/m4-stack.git
cd m4-stack

# 1) Fetch FreeInk SDK + vendored libs (~from m4-device; needs network)
bash scripts/bootstrap_deps.sh

# 2) Firmware (PlatformIO)
export PATH="${HOME}/.platformio/penv/bin:$PATH"
cd firmware && pio run -e murphy_m4

# 3) Host simulator
cd ../simulator
cmake -B build && cmake --build build -j
ctest --test-dir build --output-on-failure
```

Requirements:

- macOS or Linux  
- Python 3.10+  
- [PlatformIO](https://platformio.org/) (`~/.platformio/penv`) for firmware  
- CMake + C++17 compiler for simulator  
- Optional: Espressif `qemu-xtensa` for full-flash experiments  

## Layout

```text
firmware/     ESP32-S3 Murphy reader (PlatformIO env murphy_m4)
simulator/    Deterministic host sim + QEMU image tools
plugins/      fanqie / jjwxc / weread (+ legado stub)
scripts/      bootstrap_deps.sh
docs/         Deep handoffs (QEMU, animations, …)
AGENTS.md     Rules for coding agents
HANDOFF.md    Current status for handoff
VERSIONS.md   Pinned SHAs
```

## Device (optional)

APP1-only flash (never full-chip erase without explicit approval):

```bash
# from firmware/, with device on /dev/cu.usbmodem*
bash scripts/flash_app1_once.sh /dev/cu.usbmodem101
```

m4adb: keep **one** daemon; see `AGENTS.md`.

## QEMU note

Full Murphy flash boot in QEMU is **not** production-complete yet (PSRAM/entry hang).  
Details: `docs/QEMU_BOOT_HANDOFF.md`. Use `pio run -e murphy_m4_qemu` only for experiments — **do not flash that env to hardware**.

## License

Same as component projects (see each subtree `LICENSE` where present).
