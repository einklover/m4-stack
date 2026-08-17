# Stage 13 — full-chain TTF / TXT / plugin runtime E2E

Branch: `agent/m4-emulator-stage13-e2e-ttf-txt-plugins`

Parent: Stage 12 cumulative display runtime.

## Goal

Exercise a real Murphy M4 user path through the emulated hardware rather than stopping at boot:

1. build an ESP32-S3 Murphy M4 binary from the normal Octal/OPI hardware profile;
2. boot it through ROM/bootloader in `-machine murphy-m4`;
3. mount a FAT32 SD image through native SDMMC;
4. discover and parse `/FONT/M4E2E.ttf` with the production runtime TrueType implementation;
5. load `/books/e2e.txt` through the production `Txt` class and enter the real `TxtReaderActivity`;
6. render through the normal renderer -> GP-SPI2 -> SSD1677 model and capture PBM frames;
7. use the existing M4SerialDebugBridge/m4adb protocol to install, launch and inspect Fanqie, JJWXC and WeRead packages;
8. collect structured UI JSON, serial traces, screenshots and package results.

## Test-only control surface

The workflow patches only its ephemeral CI worktree before compilation. It does not commit these changes to production source:

- route Arduino `Serial` to UART0 so QEMU can expose a PTY;
- authorize M4SerialDebugBridge automatically;
- select the generated `M4E2E.ttf` face;
- enter one known local TXT after normal boot.

TTF parsing/rasterization, SD access, TXT decoding/paging, activities, plugin installer/registry/runtime and display code are the same production implementations. This test build is intentionally identified as an E2E control build and is not a claim that the exact shipping binary auto-opens the fixture.

## Assets

CI creates all fixtures at run time:

- a TrueType `glyf` subset generated from WenQuanYi Zen Hei and firmware/E2E characters;
- a UTF-8 Chinese TXT with two chapters;
- a 64 MiB FAT32 raw SD image;
- plugin sources checked out from the pinned Fanqie/JJWXC/WeRead checkpoint branches.

No font binary or book content is committed to the repository.

## Pass criteria

- QEMU PTY debug bridge becomes ready;
- serial contains `[E2E-TTF] load=1`;
- serial contains `[E2E-TXT] load=1 read=1` and `activity=TxtReader opened`;
- m4adb status/UI/screenshot succeed on the TXT reader;
- each of Fanqie, JJWXC and WeRead installs and launches through the real plugin host;
- all logs and PBM frames are uploaded as `murphy-full-e2e-*` artifacts.

Network-backed content is a separate fidelity boundary. A launched plugin can legitimately report a Wi-Fi/network error until ESP32-S3 Wi-Fi is modeled; Stage 13 distinguishes successful package/runtime launch from provider-network success instead of masking that boundary.
