# Build and dependencies

The M4 build is designed for a clean clone. FreeInk SDK and the related third-party trees are **vendored in this monorepo** under `firmware/`. Builds must not fetch the private `einklover/m4-device` archive.

## Vendored paths

These paths live under `firmware/` and are tracked in git:

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

The SDK contains the hardware/display libraries discovered through `lib_extra_dirs`; the other directories provide the embedded and application libraries referenced by the M4 configuration.

## Automatic flow

`firmware/platformio.ini` attaches `firmware/scripts/bootstrap_m4_deps.py` to the M4 base environment before PlatformIO resolves libraries. The pre-script checks required sentinel files. If any are missing, it invokes:

```bash
bash scripts/bootstrap_deps.sh
```

That script only **verifies** the vendored trees and applies the QEMU InputManager patch when needed. It does **not** download anything. Missing trees are a git checkout / worktree problem — restore them from the repository instead of curling private GitHub.

The normal build is:

```bash
export PATH="$HOME/.local/bin:$HOME/.platformio/penv/bin:$PATH"
cd firmware
pio run -e murphy_m4 -j1
```

## Manual bootstrap

When diagnosing a worktree or confirming sentinels explicitly, run from the repository root:

```bash
bash scripts/bootstrap_deps.sh
cd firmware
pio run -e murphy_m4 -j1
```

Manual bootstrap is supported; it is not required before a normal M4 build when the vendored trees are present.

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
`firmware/lib/EpdFont/builtinFonts/`: that ~159 MiB tree is the vendored UI
font pack and is not the source for the custom M4 center-kernel font.

## Prerequisites and caches

The firmware build requires Python 3 and PlatformIO (plus its downloaded toolchain/packages). `curl`/`tar` are **not** required for M4 dependency bootstrap anymore. Install PlatformIO with your Python environment, then ensure its CLI is on `PATH`:

```bash
python3 -m pip install --user platformio
export PATH="$HOME/.local/bin:$HOME/.platformio/penv/bin:$PATH"
```

PlatformIO downloads the M4 platform from the pinned `pioarduino/platform-espressif32` release `55.03.37`. The M4 configuration also uses its declared ArduinoJson, QRCode, PNGdec, WebSockets, NimBLE-Arduino, and SdFat dependencies. Keep the package cache at `~/.platformio` and use a compiler-object cache for repeated builds.

## Troubleshooting

If a first build reports missing `open-m4-sdk`, `Epub`, `Lua`, font, or other vendored inputs, run `git status` / `git checkout -- firmware/...` to restore the tracked trees. Do not point bootstrap at a private GitHub archive.
