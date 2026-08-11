# Murphy patches for Espressif QEMU

These patches exist to make **unmodified production Murphy M4 binaries** observe
missing ESP32-S3/board hardware behavior. They must not become a second guest
firmware compatibility layer.

## Reproducibility

`upstream.json` pins an exact `espressif/qemu` commit. `series` defines patch
order. Build with:

```bash
python3 simulator/qemu/build_patched_qemu.py
export QEMU_XTENSA="$HOME/.cache/murphy-m4/espressif-qemu/build-murphy/qemu-system-xtensa"
```

Before changing the upstream SHA:

1. rebase every patch;
2. run `git apply --check` through the builder;
3. rebuild QEMU;
4. rerun the production-flash acceptance command;
5. record the new boot checkpoint in the stage handoff.

## Patch 0001 — SENS ADC oneshot completion

ESP-IDF v5.5's S3 ADC self-calibration starts an RTC/SENS oneshot and polls the
SENS `meas*_done_sar` bit. Upstream QEMU currently leaves the SENS range in the
generic unsupported I/O window, whose reads return zero. That makes the
constructor wait forever before `app_main`/Arduino setup.

The first patch implements only the digital contract needed to stop that hang:
SENS register storage plus START→DONE and deterministic internal-ground data.
It deliberately does **not** claim analog ADC accuracy. Battery-voltage and
external-channel analog input belong to the Murphy board model and can be added
as QOM properties later.

## Rules for future patches

- Prefer an existing upstream QEMU device before adding a replacement. Stage 3
  demonstrated this for SDMMC.
- Keep SoC fixes generic ESP32-S3 behavior when possible; keep Murphy-specific
  GPIO/peripheral wiring in a board machine/device layer.
- Every bypass removed from `M4_QEMU_BUILD` should have a corresponding
  production-bin acceptance assertion before deleting the bootstrap path.
- Do not patch the guest binary or rewrite its hash to hide emulator failures.
