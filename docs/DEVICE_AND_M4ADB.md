# Device and m4adb

This guide covers the production Murphy M4 USB debug path. Device operations are stateful: serial ownership, USB re-enumeration, and the active OTA slot all matter.

## One daemon, one serial owner

Run one repository `m4adb` daemon for a device. Normal commands reuse that owner through its Unix socket. Do not run competing daemons, open the same serial device from ad-hoc tools, or kill processes with a broad `pkill`; use the repository commands and stop the known daemon when necessary.

The bridge requires the device's Developer Options USB serial debugging/control setting. Without that runtime authorization, the custom debug bridge is not available.

## Basic commands

The audited command surface is `firmware/scripts/m4adb.py --help`. From the repository root:

```bash
python3 firmware/scripts/m4adb.py devices
python3 firmware/scripts/m4adb.py doctor
python3 firmware/scripts/m4adb.py ping
python3 firmware/scripts/m4adb.py status
python3 firmware/scripts/m4adb.py ui
python3 firmware/scripts/m4adb.py logs
```

Use `--port PORT` when automatic detection is ambiguous. The daemon lifecycle commands are also available:

```bash
python3 firmware/scripts/m4adb.py daemon
python3 firmware/scripts/m4adb.py daemon_stop
```

`--no-daemon` exists for fault isolation and may reset the device; it is not the normal workflow. `--mock` is for host-only use without hardware.

## USB re-enumeration

Reset, flashing, and changing the active OTA slot can make the USB device disappear briefly and return with a different path. Run `devices` again, allow the one daemon to reconnect, and retry `ping` or `status`. A missing old path is not evidence that a second daemon should be started.

The official factory APP0 does not expose the custom USB CDC debug bridge. Therefore the custom CDC port can be absent while official firmware is running. Enable the device's USB serial debugging option and boot the custom APP1 image before diagnosing the bridge.

## APP1-only production flashing

The production PlatformIO image is an APP1 payload. At a conceptual level, the supported flash helper preflights the factory dual-OTA partition shape, writes only the application at offset `0x6e0000`, verifies the write, and selects OTA slot 1. It does not need to rewrite the bootloader, partition table, NVS, or factory APP0.

Use the wrapper from `firmware/`:

```bash
cd firmware
bash scripts/flash_app1_once.sh /dev/cu.usbmodemXXXX
```

This wrapper handles the one-shot write, slot selection, reboot wait, and daemon restart. Use a `murphy_m4` build artifact only. Do not use generic PlatformIO upload, a QEMU profile, a full-chip erase, or an unsafe full-flash command as the normal path.
