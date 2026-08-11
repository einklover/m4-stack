# Murphy M4 Stack — Agent rules

This monorepo is the **default cwd** for new AI / Codex work on Murphy M4.

## Layout

| Path | What |
|------|------|
| `firmware/` | m4-firmware (ESP32-S3 / PlatformIO `murphy_m4`) |
| `simulator/` | murphy-m4-simulator (host CMake + QEMU helpers) |
| `plugins/` | fanqie / jjwxc / weread (+ legado stub) |
| `scripts/` | bootstrap + cross-cutting helpers |
| `docs/` | deep handoffs (QEMU, page-turn, …) |
| `VERSIONS.md` | pinned upstream SHAs |

Upstream component repos still exist (`einklover/m4-firmware` etc.). Prefer committing here for multi-component work; mirror important fixes back to component repos when releasing.

## Non-negotiables

1. **Main thread implements** unless user asks for subagents. Do not silently spawn Sol/DeepSeek for implementation.
2. **Firmware changes**: run simulator/host tests first when possible; device flash is APP1-only via `firmware/scripts/murphy_m4_app1_flash.py` or `flash_app1_once.sh`. Never flash APP0 / bootloader / full erase without explicit user OK.
3. **m4adb**: single global daemon; never `pkill -f m4adb.py`; device I/O only through `scripts/m4adb.py` (daemon socket). See historical iron rules in component docs if present.
4. **Do not flash `murphy_m4_qemu` builds to hardware** (QEMU-only PSRAM profile).
5. **Do not commit** reconstructed `open-m4-sdk` / `builtinFonts` / `.pio` / `firmware.bin` / plugin `.m4x` binaries.

## First-time setup (every clone)

```bash
bash scripts/bootstrap_deps.sh   # needs network → einklover/m4-device
```

## Build / test

```bash
# Firmware (needs PlatformIO + penv with esptool)
export PATH="${HOME}/.platformio/penv/bin:$PATH"
cd firmware && pio run -e murphy_m4

# Host simulator
cd simulator && cmake -B build && cmake --build build -j
ctest --test-dir build --output-on-failure

# QEMU (optional, Stage2 incomplete — see docs/QEMU_BOOT_HANDOFF.md)
cd firmware && pio run -e murphy_m4_qemu
cd ../simulator
python3 tools/murphy_flash_image.py --build-dir ../firmware/.pio/build/murphy_m4_qemu -o /tmp/murphy-qemu.bin
python3 qemu/run_murphy_bin.py /tmp/murphy-qemu.bin --seconds 40 --serial-file /tmp/mq.log --probe
```

## Where to start reading

1. `README.md` — 30-second map  
2. `HANDOFF.md` — current status  
3. `docs/QEMU_BOOT_HANDOFF.md` — if task is QEMU boot  
4. `simulator/docs/CODEX_FIRMWARE_DEBUG_GUIDE.md` — firmware debug with sim  

## Plugins / network pitfalls (short)

- Prefer host `dl.jsonGet` for large JSON; Lua heap is ~512KB.  
- Do not send duplicate `User-Agent` if host already sets one.  
- Catalog/list row IDs: use your table by index, not list callback title-as-id.  
- Wi-Fi drops on device: `m4adb wifi_prepare` before install_http.
