# ESP32-S3 ROMs for Murphy QEMU

## Do we ship ROMs in this repo?

**No.** Espressif's QEMU tree already embeds the ESP32-S3 ROM blobs used by
`qemu-system-xtensa -machine murphy-m4`. The Murphy-patched binary is built by:

```bash
python3 simulator/qemu/build.py -j $(nproc 2>/dev/null || sysctl -n hw.ncpu)
```

Upstream pin: `simulator/qemu/patches/upstream.json`  
Patch series: `simulator/qemu/patches/series-v3`

## If a future machine needs an external ROM

1. Do **not** commit copyrighted ROM binaries.
2. Add a fetcher with SHA-256 verification, e.g. `scripts/fetch_or_prepare_rom.py`.
3. Document the expected path here, for example:

```text
simulator/roms/esp32s3_rev0_rom.bin   # user-provided, gitignored
```

4. Fail with a clear message if the file is missing or the hash mismatches.

## Related firmware images

| Artifact | How to obtain | Commit? |
|----------|---------------|---------|
| Patched `qemu-system-xtensa` | `simulator/qemu/build.py` → `~/.cache/murphy-m4/...` | No (rebuild) |
| `bootloader.bin` / `partitions.bin` / `firmware.bin` | `pio run -e murphy_m4` or `murphy_m4_qemu_plugin` | No (build) |
| 16 MiB flash | `simulator/tools/murphy_flash_image.py` | No (compose) |
| FAT32 SD image | `simulator/qemu/make_sd_image.py` | No (compose) |
