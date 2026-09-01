# Vendored FreeInk SDK subset for Murphy M4

Source: `m4-crosspoint-native/freeink-sdk` at commit
`7c4aac5b1a8a2a159c3738081aa5bcdaf0a7aec0`.

License: MIT (see LICENSE, NOTICE).

This tree is a **real copy** of only the libraries required to build the
Murphy M4 target (BoardConfig, FreeInkDisplay, BatteryMonitor, InputManager,
SDCardManager, FrontlightManager, PowerManager). It is not a symlink and is
not referenced via absolute local PlatformIO paths.

Device profile of record for Phase 1: `FREEINK_DEVICE_MURPHY_M4` —
ESP32-S3, SSD1677 800×480 panel (logical portrait UI 480×800), FT6x36 touch,
4-bit SDMMC, dual-channel frontlight.
