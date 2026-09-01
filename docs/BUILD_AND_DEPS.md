# Build and dependencies

The public `m4-stack` checkout is the only repository required for the M4
source and SDK dependencies. `firmware/open-m4-sdk/` and the Epub, Lua,
Expat, miniz, and picojpeg libraries are tracked in this repository. The
validator below is offline and exists to produce a clear error if a checkout
is incomplete:

```bash
bash scripts/bootstrap_deps.sh
```

PlatformIO runs the same validation before it discovers the M4 libraries.
Neither the root script nor the PlatformIO pre-script accesses
the former private device repository or any other sibling repository. The QEMU
`M4_QEMU_PLUGIN_DEBUG` InputManager patch is applied to an older unpatched
in-tree SDK copy before the validator returns; a complete patched checkout is
a download-free no-op.

## Production build

```bash
export PATH="$HOME/.local/bin:$HOME/.platformio/penv/bin:$PATH"
cd firmware
pio run -e murphy_m4 -j1
```

The M4 production and QEMU profiles define `OMIT_FONTS=1` and use the tracked
center-kernel font, so they do not require the generated bitmap-font tree.
The PlatformIO platform/toolchain and declared libraries may still be
downloaded by PlatformIO into its normal user cache.

## Generated builtinFonts

`firmware/lib/EpdFont/builtinFonts/` is a generated production artifact and
must remain untracked. A clean clone contains the complete generator and its
Python dependency declaration, but intentionally contains no TTF. To generate
the seven faces included by `builtinFonts/all.h`, provide a legally usable
local TTF or OTF:

```bash
python3 -m pip install --user -r firmware/lib/EpdFont/scripts/requirements.txt
firmware/lib/EpdFont/scripts/convert-builtin-fonts.sh \
  --font /path/to/compatible-font.ttf \
  --charset gb2312-plus
```

The output is written to:
`firmware/lib/EpdFont/builtinFonts/{notosans_12,13,14,16,18,20,22}_bold.h`
plus `all.h`. For a smaller controlled fixture, use
`--codepoints-file firmware/lib/EpdFont/m4_ui_charset.txt`
with a local copy of that file, or use `--charset empty` for a toolchain
smoke check. Prefer a pixel-style, CJK-capable TTF for Chinese UI coverage.

If a non-`OMIT_FONTS` build is attempted before generation, the M4 pre-script
fails with an actionable message naming the generator command and required
local font. The TTF itself is never copied or published by the repository.

## Legacy SD-card update removal

The former SD-card intermediary updater was intentionally removed. Public M4
builds no longer require an embedded second-stage updater image or any private
repository artifact. Online OTA retains its network download, verification,
and direct OTA-slot write path; dual-boot slot switching and APP1-only flash
scripts are separate and remain supported.

## Regression contract

```bash
python3 firmware/tests/test_m4_dependency_bootstrap_contract.py
python3 firmware/tests/test_m4_self_contained_contract.py
```

The self-contained contract intentionally does not require the generated
159 MiB font output in Git. It checks for the in-repo generator, ignored
output path, actionable documentation, tracked source dependencies, and the
absence of an active private-repository build reference.
