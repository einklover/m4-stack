# Murphy M4 Stack

This repository is the Murphy M4 monorepo: production firmware for the ESP32-S3 reader, the host/QEMU validation harness, and reader plugins.

## Requirements

For the firmware path you need:

- Git and Python 3.10 or newer
- `curl` and `tar` for a clean-clone dependency bootstrap
- PlatformIO for the firmware build

For simulator work, also install a C/C++17 toolchain, CMake, and the host packages listed in [the simulator setup guide](docs/M4SIM_CLEAN_SETUP.md).

## Five-minute build

```bash
git clone https://github.com/einklover/m4-stack.git
cd m4-stack
export PATH="$HOME/.local/bin:$HOME/.platformio/penv/bin:$PATH"
cd firmware
pio run -e murphy_m4 -j1
```

The production image is written to `firmware/.pio/build/murphy_m4/firmware.bin`.

On the first M4 build, PlatformIO runs the repository's dependency pre-script. If reconstructed SDK/library sentinels are missing, it fetches the pinned `einklover/m4-device@f86b134` archive through `scripts/bootstrap_deps.sh`; when the sentinels are already present, it does no download. See [Build and dependencies](docs/BUILD_AND_DEPS.md) for the manual path and cache details.

## Simulator

From the repository root, the verified smoke entry point is:

```bash
./m4sim test smoke --plugin-debug
```

The command uses the repository's simulator/QEMU chain and reports protocol-level readiness. See [M4Sim clean setup](docs/M4SIM_CLEAN_SETUP.md) and [the simulator README](simulator/README.md) for setup and capabilities.

## Device safety

Production hardware flashing is APP1-only. Build `murphy_m4`, then use the supported wrapper from `firmware/`:

```bash
cd firmware
bash scripts/flash_app1_once.sh /dev/cu.usbmodemXXXX
```

Never use a QEMU profile on hardware, and do not write APP0, the bootloader, the partition table, NVS, or perform a full erase as a normal recovery step. Read [Device and m4adb](docs/DEVICE_AND_M4ADB.md) first.

## Repository map

```text
firmware/     Murphy M4 production firmware and PlatformIO configuration
simulator/    host models, patched QEMU tools, and E2E journeys
plugins/      fanqie, jjwxc, weread, and related plugin projects
scripts/      repository-wide bootstrap helpers
docs/         stable development contracts and operating guides
```

## Further reading

- [AI quickstart](docs/AI_QUICKSTART.md)
- [Build and dependencies](docs/BUILD_AND_DEPS.md)
- [Device and m4adb](docs/DEVICE_AND_M4ADB.md)
- [Troubleshooting](docs/TROUBLESHOOTING.md)
- [Fast firmware development](docs/FAST_FIRMWARE_DEV.md)
- [Simulator clean setup](docs/M4SIM_CLEAN_SETUP.md)
- [Plugin READMEs](plugins/)
