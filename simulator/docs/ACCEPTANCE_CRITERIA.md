# Final acceptance criteria

The Murphy M4 validation harness is considered **software-stack complete** when
all applicable automated gates below pass on the same branch revision. This is
not a claim of analog/physical equivalence.

## A. Hardware specification / provenance

- `board/murphy_m4.json` validates.
- Current FreeInk `BoardConfig::MURPHY_M4` matches simulator pins, polarity,
  geometry, touch transform, frontlight and SDMMC configuration.
- Known evidence conflicts remain explicit (notably W25Q32 schematic marking vs
  current 16MiB executable firmware layout).
- Schematic-present but firmware-disabled devices remain distinguishable.

## B. Deterministic reader/fault layer

- all default scenarios pass;
- intentionally buggy scenarios are caught by invariants;
- page intent/provenance/catch-up rules hold;
- heap stress/fuzz and fragmentation/TLS scenarios pass;
- schedule-fuzz passes for the chosen release seed budget.

## C. Board/controller layer

- GPIO ownership/electrical contention tests pass;
- active-low power gates pass;
- 4-bit SDMMC power-loss and card lifetime tests pass;
- FT6x36 frame/coordinate/input semantics pass;
- battery and frontlight production policy tests pass;
- deep-sleep/wake/held-rail semantics pass;
- SSD1677 command/RAM/window/BUSY/activation model passes;
- current real FreeInk `Ssd1677Driver.cpp` replays successfully against the
  controller model.

## D. Historical issue layer

- every real `m4-firmware` GitHub issue is classified in
  `issues/regressions.json`;
- every `reproduced` claim points to a local executable test;
- plugin/provider regressions are either executed in m4-device/plugin host tests
  or explicitly mapped to a local cross-cutting contract;
- issue #2/#5/#24 mechanism regressions pass;
- issues requiring physical evidence remain labeled `device_trace_required`.

## E. Cross-repository source drift

- PageTurnCoordinator simulator and firmware copies produce identical behavioral
  fingerprints;
- simulator board profile matches the current SDK BoardConfig;
- focused Fanqie/JJWXC/WeRead/m4-device host regressions pass.

## F. Real compiled firmware

- `pio run -e murphy_m4` succeeds;
- exact 16MiB flash composition succeeds;
- ESP32-S3 QEMU executes at least the ROM + second-stage bootloader from that
  actual image;
- the captured QEMU log is classified by `qemu/probe_boot.py`;
- any later board-init failure is recorded as a truthful unsupported/failing
  boundary rather than patched out of production firmware.

Reaching Home in QEMU is a progressive target, not a precondition for claiming
that the deterministic/controller/compiled-software layers are correct. When a
real firmware image stops on an unsupported QEMU peripheral, that boundary
becomes the next device-model task.

## G. Sanitizers / host correctness

- ASan/UBSan pass for board/peripheral/issue-contract host code;
- native CTest and Python tooling tests pass.

## H. Documentation / agent usability

- `AGENTS.md` provides mandatory agent rules;
- `docs/CODEX_FIRMWARE_DEBUG_GUIDE.md` provides commands and layer selection;
- `tools/acceptance.py` provides one-command offline acceptance;
- field logs can be normalized with `tools/import_device_trace.py`;
- QEMU logs can be classified with `qemu/probe_boot.py`.

## Physical-only acceptance (never claim from simulator alone)

These remain real-device measurements even after every software gate is green:

- SSD1677/e-paper ghost density and perceived residual image;
- analog/multi-phase LUT gray progression and visual wipe continuity;
- temperature-dependent electrophoretic response;
- Wi-Fi/BLE RF performance and coexistence;
- SDMMC signal integrity and card-specific electrical marginality;
- USB electrical behavior;
- battery chemistry, charging thermal behavior and absolute ADC calibration;
- regulator/frontlight analog efficiency/noise;
- sleep current and board leakage;
- ESD/EMI/mechanical/touch-panel physical behavior.

The intended development loop is therefore:

```text
host/plugin tests -> deterministic faults -> board/controller -> sanitizers
-> real firmware build -> QEMU compiled-software boundary -> physical acceptance
```

A physical flash should be the final calibration/acceptance step, not the first
place a normal firmware bug is discovered.
