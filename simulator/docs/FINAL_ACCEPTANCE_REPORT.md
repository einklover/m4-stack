# Murphy M4 validation harness — final software-stack acceptance

Acceptance revision before this report: `80dd43c009a368b92d1f0a5dc12912e66cd4e77f`.
The report itself is documentation-only; the expensive QEMU/plugin evidence was
collected from that unchanged code revision.

## Result

**Software-stack acceptance: PASS.**

This means the deterministic, board/controller, cross-repository host, compiled
Murphy firmware and ESP32-S3 QEMU layers all passed their defined gates. It does
**not** claim analog/electrical equivalence for e-paper physics, RF, battery
chemistry or signal integrity; those remain physical-device acceptance items.

## 1. Native / deterministic / sanitizer gate — PASS

The latest acceptance revision passed:

- Murphy board-spec provenance validator;
- live `m4-firmware` GitHub issue coverage: all 9 real issues classified;
- plugin regression-map validator;
- calibration-profile validation;
- deterministic page-turn/EPD/SD/TLS/heap scenarios;
- low-level GPIO/I2C/SPI/SDMMC/SSD1677 board tests;
- battery/frontlight/button/touch/deep-sleep policy tests;
- historical issue mechanism contracts;
- native alternate-backend smoke;
- CTest and Python diagnostic/tooling tests;
- ASan + UBSan low-level tests.

## 2. Hardware-source drift gates — PASS

Two cross-repository invariants were executed before the expensive integration
steps:

1. simulator and `m4-firmware` PageTurnCoordinator copies produced identical
   behavioral fingerprints across production, injected-bug, threshold and
   timestamp-wrap traces;
2. `board/murphy_m4.json` matched the current FreeInk
   `BoardConfig::MURPHY_M4` values for display geometry/pins/SPI, SD power
   polarity, native 4-bit SDMMC, buttons, battery multiplier, touch address/
   transform/power polarity, frontlight and firmware sensor capability.

The schematic W25Q32 marking versus current 16MiB N16R8 firmware layout remains
an intentionally unresolved source discrepancy rather than being silently
rewritten.

## 3. Real FreeInk SSD1677 driver replay — PASS

CI compiled the current `m4-device/main` implementation of
`Ssd1677Driver.cpp`. The production driver was not copied or modified.

A host `EpdBus` implementation forwarded its actual command/data/waitBusy stream
into `SimSpiBus + SimSsd1677Controller`. The following passed:

- Murphy geometry 800×480;
- production 10MHz SPI selection;
- real controller-init stream;
- full 48,000-byte RAM-window semantics;
- first absolute paint;
- subsequent fast differential paint;
- current/BW and old/RED frame provenance;
- update-control values including the current `0xFC` fast sequence;
- one physical commit per completed activation;
- SPI transaction discipline and BUSY handling.

This is stronger than testing a hand-written imitation of the driver sequence:
SDK command changes now cross an independently implemented controller model.

## 4. Real plugin / host regressions — PASS

The cross-repository gate checked out current `m4-device/main`, materialized the
same PlatformIO dependencies and ran focused host regressions for:

- Fanqie Lua journey/state transitions;
- Fanqie memory regression;
- JJWXC Lua journey;
- WeRead progress-shard round trip;
- M4 subactivity lifecycle;
- plugin reader audit;
- architecture fault injection.

The host build required OpenSSL `libcrypto` because `WereadCrypto.cpp` calls the
host OpenSSL MD5 symbol; the validation workflow links that host dependency
explicitly rather than modifying plugin/firmware behavior.

## 5. Actual Murphy firmware binary in ESP32-S3 QEMU — PASS

The integration workflow did not run a toy QEMU application. It:

1. checked out `m4-firmware` `agent/page-turn-shadow-phasea`;
2. verified PageTurnCoordinator behavior sync;
3. reconstructed the same FreeInk SDK/unvendored libraries used by firmware CI;
4. verified the reconstructed FreeInk Murphy BoardConfig against the simulator
   contract;
5. ran `pio run -e murphy_m4` successfully;
6. composed the exact disposable 16MiB Murphy flash image;
7. installed ESP-IDF 5.5.2 and Espressif `qemu-xtensa`;
8. executed the real flash image in ESP32-S3 QEMU;
9. captured and classified the boot log.

QEMU evidence run: `31415860952`.

Final classifier result:

```text
highest_firmware_checkpoint = home_or_reader
failure_class               = none
```

Therefore the real compiled Murphy image progressed through the production boot
chain (setup start, PSRAM/early board work, SD/display/touch/font phases and boot
summary) into the Home/Reader stage without a classified panic/watchdog/board
initialization failure in that run.

This does **not** mean QEMU is an analog SSD1677/SD-card/touch-panel emulator.
QEMU answers the machine-code/ESP32-S3/FreeRTOS/boot integration question, while
the independent host board/controller layer answers Murphy protocol and fault
questions. Physical hardware remains authoritative for electrical/analog
behavior.

## 6. Firmware issues — coverage status

Every current real issue in `einklover/m4-firmware` is classified:

- #1 SSD1677 wipe/analog intermediate appearance — controller semantics modeled;
  final appearance remains physical-trace-required;
- #2 fragmented internal RAM/TLS OOM — reproduced;
- #4 JJWXC memory/prefetch tradeoff — constituent mechanisms modeled;
- #5 cross-chapter empty-path/repeated reader-generation chain — reproduced;
- #9 Native WeRead HTTPS/PSRAM residency panic class — residency contract
  modeled, exact mbedTLS panic belongs to compiled/device execution;
- #17 native UI/catalog field regression — upstream firmware/UI regression;
- #18 runtime TTF UI physical-pixel regression — upstream regression (closed);
- #20 native layout parity/markup regression — upstream regression;
- #24 HTTP/SD atomic commit + TTF/UI field failures — SD publication mechanism
  reproduced and upstream parser/UI tests linked.

`reproduced` is intentionally reserved for entries with a local executable test.
The matrix validator prevents stronger claims without evidence.

## 7. Physical-only acceptance that remains

The following are deliberately outside software-stack PASS and must not be
claimed from host/QEMU results:

- e-paper ghost density and perceived residual image;
- analog/multi-phase LUT gray progression and temperature response;
- Wi-Fi/BLE RF and coexistence;
- SDMMC signal integrity/card electrical marginality;
- USB electrical behavior;
- battery chemistry/charging thermal behavior/absolute ADC calibration;
- regulator/frontlight analog efficiency/noise;
- true deep-sleep current and board leakage;
- ESD/EMI/mechanical/touch-panel physical effects.

## 8. Development workflow delivered

Agents should use, in order:

- `AGENTS.md` — mandatory agent contract;
- `docs/CODEX_FIRMWARE_DEBUG_GUIDE.md` — layer selection and commands;
- `tools/acceptance.py` — one-command offline layered gate;
- `tools/import_device_trace.py` — normalize real Murphy logs;
- `qemu/probe_boot.py` — classify real-firmware QEMU progress;
- `issues/regressions.json` / `issues/plugin_regressions.json` — regression
  ownership and evidence;
- `profiles/murphy_m4.json` — source-attributed calibration evidence.

The intended workflow is now:

```text
plugin/host regression
 -> deterministic race/fault simulation
 -> memory/storage/lifetime contracts
 -> Murphy board/controller model
 -> sanitizers
 -> real Murphy PlatformIO build
 -> real flash image in ESP32-S3 QEMU
 -> physical unit only for final analog/electrical calibration
```

This makes physical flashing the last acceptance/calibration stage for normal
firmware work rather than the primary debugger.
