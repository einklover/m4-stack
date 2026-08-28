# Troubleshooting

Use the smallest diagnostic that distinguishes the failure. Preserve logs and unknown worktree changes while investigating.

## Missing reconstructed libraries

Symptoms include missing `open-m4-sdk`, `Epub`, `Lua`, or font headers during an M4 build. Run `bash scripts/bootstrap_deps.sh` from the repository root; it validates the tracked source dependencies offline and reports the missing path.

## PlatformIO not found

Install PlatformIO into the active Python environment and add its user and PlatformIO virtual-environment bin directories to `PATH`:

```bash
python3 -m pip install --user platformio
export PATH="$HOME/.local/bin:$HOME/.platformio/penv/bin:$PATH"
```

Confirm from `firmware/` with `pio run -e murphy_m4 -j1`.

## `.pio` disappears during a build

`.pio/` is generated and ignored, but it should not vanish under a single build. Concurrent PlatformIO processes or cleanup commands can race over the same build directory. Stop parallel builds, rerun one process with `-j1`, and preserve any unrelated dirty files. Reuse the compiler cache rather than launching competing builds.

## m4adb port ownership

Only one daemon/serial owner may use a device. Check candidates with `python3 firmware/scripts/m4adb.py devices` and diagnose with `doctor`. Reuse the normal daemon; if the known daemon is stale, use `daemon_stop` and start one replacement. Do not use broad process-kill commands or open the port with another serial monitor.

## Custom CDC is absent under official firmware

The custom debug bridge is runtime-gated and is not exposed by the official factory APP0. The USB port can therefore disappear when APP0 is active. Boot the authorized custom APP1 image and let USB re-enumerate before retrying `m4adb`.

## QEMU transport is not protocol readiness

A QEMU TCP socket or PTY can exist before firmware has initialized the debug bridge. Treat transport availability as a low-level signal only. Use the verified root entry `./m4sim test smoke --plugin-debug`, or wait for a real `m4adb ping` response containing the expected protocol data.

## Stale local network endpoint

Device IP addresses and local network routes can change. Do not hard-code an old endpoint in code or docs. Use `python3 firmware/scripts/m4adb.py wifi_status` to inspect current state, and `wifi_prepare` when the saved network must be activated through the USB session. USB remains the fallback for device control and installation.

## Simulator versus hardware evidence

Host tests validate modeled logic; QEMU validates the supported emulated boot/protocol path; a production compile validates buildability. None of these proves physical USB, RF, power, display, or board behavior. Mark a claim as hardware-validated only after a real-device command or observation, with the image/profile, port lifecycle, and result recorded.
