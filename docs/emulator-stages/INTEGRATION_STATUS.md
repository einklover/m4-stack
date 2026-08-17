# Murphy M4 binary-emulator integration gate

Branch: `agent/m4-emulator-integration`

This branch is not a substitute for the individual Stage branches. It exists to
compile and test the **cumulative** hardware/QEMU work against `main`, because
chained pull requests do not always schedule GitHub Actions consistently.

## Included stages at creation

- Stage 1 — patchless production-flash runner
- Stage 2 — hardware knowledge base / executable board contract / raw SD image
- Stage 3 — native Espressif QEMU SDMMC card attachment
- Stage 4 — SENS ADC oneshot completion below the guest
- Stage 5 — Octal/MSPI audit and ESP32-S3 SPI transfer-index correction
- Stage 6 — functional digital GPIO foundation
- Stage 7 — ESP32-S3 I2C controller realization
- Stage 8 — dedicated `murphy-m4` machine and FT6x36-compatible touch device

## Gate

The `.github/workflows/m4-qemu-patchset.yml` workflow must, at minimum:

1. run production-runner/patchset tests;
2. clone the exact QEMU SHA in `simulator/qemu/patches/upstream.json`;
3. apply every ordered transform/diff;
4. pass `git diff --check`;
5. configure and compile `xtensa-softmmu`;
6. run `qemu-system-xtensa --version`.

Later stages advance this branch only after their own Stage branch and handoff
are saved. A successful integration build proves patch application and
compilation; it does not by itself prove private production-flash runtime
behavior.
