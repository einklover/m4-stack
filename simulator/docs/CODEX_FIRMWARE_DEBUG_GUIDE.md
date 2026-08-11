# Murphy M4 firmware debugging guide for Codex and other agents

This guide turns the simulator into a development workflow, not a separate demo.
The goal is to catch the cheapest truthful failure first, then escalate only when
more hardware fidelity is required.

Start with the graded interface unless you need to invoke an underlying tool
directly:

```bash
python3 tools/ai_debug.py --list --json
python3 tools/ai_debug.py 0
python3 tools/ai_debug.py 4          # exact level
python3 tools/ai_debug.py 4 --through
```

`build/ai-debug/summary.json` is the machine-readable source of truth. See
`AI_DEBUG_INTERFACE.md` for level selection, output schema and safety boundary.

## 1. Know the stack

```text
plugin/native app logic
        |
reader/provider policy            m4-device/plugin host tests
        |
page/index/frame decisions         m4_simulator deterministic scheduler
        |
memory/TLS/storage contracts       m4_issue_contracts + SimHeap/SimTls
        |
board wiring/controller protocol   MurphyBoard + GPIO/I2C/SPI/SDMMC/SSD1677
        |
compiled ESP32-S3 firmware         Espressif QEMU + full 16MiB flash image
        |
analog/RF/physical display         real Murphy M4
```

A lower box may prove a contract, but it cannot manufacture evidence belonging
to a higher box. For example, `SimSsd1677Controller` can prove that firmware
writes the wrong RED baseline or activates while BUSY; it cannot predict the
actual amount of gray ghosting on an electrophoretic panel.

## 2. Fast local gate before firmware edits

```bash
python3 tools/validate_board_spec.py
python3 tools/validate_issue_matrix.py
python3 -m json.tool profiles/murphy_m4.json >/dev/null

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/m4_simulator
./build/m4_board_tests
./build/m4_issue_contracts
ctest --test-dir build --output-on-failure
python3 -m unittest discover -s tests -p 'test_*.py' -v
```

If a change touches allocator/controller/lifetime code, also run ASan/UBSan:

```bash
cmake -S . -B build-sanitize \
  -DM4SIM_SANITIZE=ON -DM4SIM_BUILD_NATIVE_SMOKE=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build-sanitize -j --target m4_board_tests m4_issue_contracts
./build-sanitize/m4_board_tests
./build-sanitize/m4_issue_contracts
```

## 3. Page-turn / indexing / display race work

The deterministic simulator is the first tool because a real EPD makes timing
bugs slow and non-repeatable.

List tests:

```bash
./build/m4_simulator --list
```

Run a single scenario with timeline:

```bash
./build/m4_simulator --seed 1 single_tap_slow_index -v
./build/m4_simulator --seed 1 bug_divergence -v
./build/m4_simulator --seed 1 slow_sd_tap -v
```

Fuzz scheduling:

```bash
./build/m4_simulator --seeds 1:1000
```

Required invariants:

- one valid page intent never needs a second tap to wake it;
- target page, pending rendered frame and physical committed page are different
  concepts and must not be collapsed;
- physical bookkeeping follows committed frame provenance, not live target;
- when the final target differs from the physical page, catch-up eventually
  drives it once indexing/panel state permits;
- SD/network slowness may defer completion but must not erase input intent.

## 4. Internal RAM / PSRAM / TLS work

Use these first:

```bash
./build/m4_simulator --seed 1 tls_fragmented_heap -v
./build/m4_simulator --seed 1 tls_keepalive -v
./build/m4_simulator --seed 1 framebuffer_contract -v
./build/m4_issue_contracts
```

Current Murphy policy:

- framebuffer: PSRAM;
- large HTTP body/decode/catalog payload: PSRAM;
- TTF cmap/cache: PSRAM where supported;
- TLS worker stack and ESP networking control objects that require internal RAM:
  internal RAM;
- total free bytes do **not** imply a large contiguous internal allocation is
  possible: ESP32-S3 internal banks remain independent in `SimHeap`.

When a device OOM is reported, import the log instead of guessing:

```bash
python3 tools/import_device_trace.py serial.log -o trace.json
```

Update `profiles/murphy_m4.json` only with source attribution.

## 5. SD / chapter persistence work

`m4_issue_contracts` exercises the storage publication policy from issue #24:

```text
write temporary generation
        |
try rename fast path
        |
rename fails on card/filesystem?
        v
bounded <=2KiB streaming copy
        |
sync + final byte-size verification
        |
ONLY THEN replace authoritative generation
```

The previous catalog/chapter remains valid on read/write/sync/size failure.
Never expose a partially committed path to `TxtReaderActivity`.

Cross-chapter reader lifetime is also checked:

```text
reader open N
  -> store switch intent N+1
  -> close/release reader generation N
  -> obtain and fully commit N+1 content
  -> open generation N+1
```

An empty `relPath` is an unavailable state, not a reader path.

## 6. Board / driver work

Validate the board contract first:

```bash
python3 tools/validate_board_spec.py
./build/m4_board_tests
```

Current executable pin profile comes from the production-probed FreeInk
`BoardConfig::MURPHY_M4`, while the schematic records physical topology:

```text
EPD:     SCLK4 MOSI3 CS5 DC6 RST7 BUSY8
SDMMC:   PWR10(active-low) CLK16 CMD15 D0=17 D1=18 D2=11 D3=14
keys:    UP1 DOWN2 LOCK/POWER0 active-low
battery: ADC9
I2C0:    SDA13 SCL12
Touch:   IRQ44 PWR45(active-low), addr 0x2e
buzzer:  GPIO46
front:   warm47 cool48
```

Do not duplicate these numbers in new model code; consume `MurphyM4Spec.h` or
`board/murphy_m4.json`.

### SSD1677

`SimSsd1677Controller` consumes driver-level command/data streams:

- `0x24` current/BW RAM;
- `0x26` old/RED RAM;
- RAM window/counters;
- `0x21/0x22` update controls;
- `0x32` LUT;
- `0x20` MASTER_ACTIVATION;
- active-high BUSY;
- one digital physical commit per activation.

A second MASTER_ACTIVATION while BUSY is an explicit protocol failure.

For LUT/ghosting appearance, use this controller model to validate digital
sequence first, then a physical panel for analog acceptance.

## 7. Build the real Murphy firmware

In `einklover/m4-firmware`:

```bash
pio run -e murphy_m4
```

Do not use a generic full-flash command on a physical unit. The firmware target
uses the factory APP1 slot at `0x6e0000`, preserving factory rollback layout.

## 8. Execute the real binary in QEMU

Compose a disposable full flash from the PlatformIO build:

```bash
python3 tools/murphy_flash_image.py \
  --build-dir ../m4-firmware/.pio/build/murphy_m4 \
  -o /tmp/murphy-qemu.bin
```

Blank-QEMU mode mirrors the byte-identical application into APP0 and APP1 so an
erased OTA-data partition can boot deterministically. If an exact 16MiB device
flash dump is supplied as `--base-flash`, the composer preserves factory/NVS/
OTA/APP0/SPIFFS and overlays only APP1.

Install Espressif QEMU through the ESP-IDF tool manager, export ESP-IDF, then:

```bash
python3 qemu/run_full_flash.py /tmp/murphy-qemu.bin
```

GDB:

```bash
python3 qemu/run_full_flash.py /tmp/murphy-qemu.bin --gdb
# second terminal: idf.py -C qemu -B build-fullflash gdb
```

Classify a captured boot log:

```bash
python3 qemu/probe_boot.py qemu.log -o qemu-probe.json
```

The correct response to a new QEMU boundary is:

1. identify the exact MMIO/peripheral/driver wait;
2. confirm it against BoardConfig/schematic/driver source;
3. add a low-level protocol test;
4. implement the missing board/QEMU device behavior;
5. rerun the **same firmware image**.

Do not patch the firmware to skip initialization merely so QEMU reaches Home.
A dedicated test build may add diagnostics, but production behavior remains the
reference.

## 9. Plugin/provider work

Business logic stays in the plugin and `m4-device` host simulator. Examples
already cataloged in `issues/plugin_regressions.json` include:

- Fanqie JSON projection/internal-RAM OOM;
- cancel-during-loading `booklist_page=nil` crash;
- in-flight fetch vs loading-screen tap scene divergence;
- explicit native-reader open handoff after body completion;
- JJWXC booklist OOM and cross-chapter prefetch/open chain;
- per-book progress sharding/large-file limits.

Use hardware simulator tests only for the cross-cutting mechanism (memory,
intent, persistence, I/O failure). Do not rewrite provider parsing in M4Sim.

## 10. Adding a field issue

1. Capture exact report/log/build/ref.
2. Add an entry to `issues/regressions.json`.
3. Choose status honestly:
   - `reproduced` = local executable failure/regression exists;
   - `modeled` = mechanism modeled, exact field execution still elsewhere;
   - `device_trace_required` = evidence missing or analog/physical;
   - `fixed_upstream_regression` = app/firmware repo already owns focused test.
4. Add the smallest truthful failing test.
5. Fix production code.
6. Run fast gate + firmware build.
7. Run QEMU for CPU/FreeRTOS/heap/boot-sensitive work.
8. Run real-device acceptance for physical claims.

The validator rejects `reproduced` claims with no local executable test.

## 11. Final acceptance checklist

A firmware change is ready for device testing only when applicable gates pass:

```text
[ ] board spec validator
[ ] issue matrix validator
[ ] deterministic scenarios
[ ] schedule fuzz for race-sensitive change
[ ] board protocol tests
[ ] issue contracts
[ ] Python tooling tests
[ ] ASan/UBSan for low-level host code
[ ] m4-device/plugin host tests for plugin changes
[ ] pio run -e murphy_m4
[ ] real firmware full-flash QEMU boot for CPU/boot/resource changes
[ ] imported/archived QEMU classification
[ ] physical M4 for EPD appearance/RF/power/signal integrity
```

The purpose of the stack is to make real-device flashing the final physical
acceptance step, not the first debugging step.
