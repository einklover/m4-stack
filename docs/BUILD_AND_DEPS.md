# Build and dependencies

The M4 build is designed for a clean clone. Large reconstructed inputs are ignored by Git and are restored from one pinned archive when needed.

## Reconstructed paths

The bootstrap reconstructs these paths under `firmware/`:

```text
open-m4-sdk/
lib/Epub/
lib/Lua/
lib/expat/
lib/miniz/
lib/picojpeg/
lib/EpdFont/builtinFonts/
src/network/updater_fw.bin
```

The SDK contains the hardware/display libraries discovered through `lib_extra_dirs`; the other directories provide the embedded and application libraries referenced by the M4 configuration. These generated trees are intentionally not committed.

## Automatic flow

`firmware/platformio.ini` attaches `firmware/scripts/bootstrap_m4_deps.py` to the M4 base environment before PlatformIO resolves the reconstructed libraries and embedded updater image. The pre-script checks required sentinel files. If any are missing, it invokes the root script:

```bash
bash scripts/bootstrap_deps.sh
```

That script downloads `einklover/m4-device@f86b134`, validates the expected archive contents, installs the paths above, and applies the required QEMU InputManager patch when needed. A complete dependency set is a no-op and does not download anything.

The normal build is:

```bash
export PATH="$HOME/.local/bin:$HOME/.platformio/penv/bin:$PATH"
cd firmware
pio run -e murphy_m4 -j1
```

## Manual bootstrap

When diagnosing a clean clone or preparing dependencies explicitly, run from the repository root:

```bash
bash scripts/bootstrap_deps.sh
cd firmware
pio run -e murphy_m4 -j1
```

Manual bootstrap is supported; it is not required before a normal M4 build.

## Custom M4 pixel source

The original custom source TTF `标准像素粗.ttf` is intentionally not
distributed because redistribution permission is unclear. Normal firmware
builds do not need it: the tracked
`firmware/src/fontdata/m4_center_kernel_16x16.bin` is already present.

Only regenerate or customize the M4 pixel font when you have a legally usable, distributable Chinese-capable pixel-style TTF. The current generator also enforces the known source-font SHA-256, so a different TTF is rejected until that validation is intentionally updated; the command syntax is:

```bash
python3 firmware/scripts/generate_m4_center_kernel.py --font <path-to-font.ttf>
```

A different TTF would change glyph appearance and output hash and cannot
reproduce the current artifact byte-for-byte. This is separate from
`firmware/lib/EpdFont/builtinFonts/`: that ~159 MiB tree is a separate
bootstrap-reconstructed dependency and is not the source for the custom M4
center-kernel font.

## Prerequisites and caches

The bootstrap requires Python 3, `curl`, and `tar`. The firmware build requires PlatformIO and its downloaded toolchain/packages. Install PlatformIO with your Python environment, then ensure its CLI is on `PATH`:

```bash
python3 -m pip install --user platformio
export PATH="$HOME/.local/bin:$HOME/.platformio/penv/bin:$PATH"
```

PlatformIO downloads the M4 platform from the pinned `pioarduino/platform-espressif32` release `55.03.37`. The M4 configuration also uses its declared ArduinoJson, QRCode, PNGdec, WebSockets, NimBLE-Arduino, and SdFat dependencies. Keep the package cache at `~/.platformio` and use a compiler-object cache for repeated builds:

```bash
export PLATFORMIO_BUILD_CACHE_DIR="$HOME/.cache/murphy-m4/platformio-build-cache"
```

The patched QEMU cache is separate from PlatformIO. Its source and patch pin are recorded in `VERSIONS.md`; do not rebuild it for an ordinary firmware-only edit.

## Clean-clone troubleshooting

If a first build reports missing `open-m4-sdk`, `Epub`, `Lua`, font, or other reconstructed inputs, verify that `curl` and `tar` are available and run `bash scripts/bootstrap_deps.sh` from the repository root. Then rerun the production build.

If bootstrap reports an archive or network failure, keep the error output: it identifies the pinned archive and the missing prerequisite. Do not replace the pin with an unverified local SDK. The dependency contract can be checked without a device:

```bash
python3 firmware/tests/test_m4_dependency_bootstrap_contract.py
```
