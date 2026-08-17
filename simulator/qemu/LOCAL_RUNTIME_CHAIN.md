# Local successful BIN runtime chain (source of truth)

Captured from a machine that already boots Murphy firmware under QEMU.
**Do not invent a second chain for CI.** `m4sim` and workflows must call this.

## What actually works locally

| Piece | Path / value |
|-------|----------------|
| QEMU binary | `$HOME/.cache/murphy-m4/espressif-qemu-v3/build-murphy-v3/qemu-system-xtensa` |
| Build entry | `python3 simulator/qemu/build.py` → `build_patched_qemu_v3.py` |
| Upstream | `patches/upstream.json` → espressif/qemu `@ febae182…` |
| Patch series | `patches/series-v3` |
| Machine | `murphy-m4` (not bare `esp32s3` for board work) |
| PSRAM | `-m 8M` + `ssi_psram is_octal=true` |
| Flash | 16 MiB raw MTD via `murphy_flash_image.py` |
| SD | raw FAT32 via `make_sd_image.py` |
| Serial | `-serial pty` → `/dev/ttysNNN` (macOS) or `/dev/pts/N` (Linux) |
| Bridge | `firmware/scripts/m4adb.py --port <pty> --no-daemon ping` |
| Ready | JSON with `"protocol"` **and** `"firmware"` (not TCP-open alone) |
| Frame | `murphy-ssd1677` `frame-file=` → `ssd1677-frame.pbm` |
| Interactive path | `run_plugin_debug.py` (`murphy_m4_qemu_plugin` firmware) |
| Production path | `run_production_bin.py` (16 MiB flash, probe boot) |

## Why a clean GitHub clone used to fail

1. **Stock Espressif QEMU** (`~/.espressif/tools/qemu-xtensa/...`) has **no**
   `murphy-m4` machine, SSD1677 frame export, or Murphy GPIO/SD wiring.
2. Patched binary lived only under **`~/.cache/murphy-m4/`** (not in git).
3. Launch required **implicit** knowledge: which of v1/v2/v3 builders, which
   `QEMU_XTENSA`, which script (`run_plugin_debug` vs `run_murphy_bin`).
4. CI historically mixed **v2** builders while local plugin debug used **v3**.
5. Readiness was sometimes “sleep N seconds” or port-open, not m4adb protocol.

## Not committed (by design)

- QEMU `build/` trees and the resulting binary
- Python venvs / pip caches
- Temporary flash/SD images under `/tmp/m4-plugin-debug` or `/tmp/m4sim`
- Device firmware blobs and copyrighted fonts/ROMs
- Local absolute endpoints (e.g. phone bridge IP in platformio flags)
