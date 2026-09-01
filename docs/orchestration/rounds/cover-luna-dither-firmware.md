# Cover Luna — fixed-point edge-retaining halftone

## Scope

Implemented the Python-approved `bilateral + edge retention + single-pixel screen`
algorithm in the exact-size Home cover conversion paths. Scene rendering and cache
contracts are unchanged.

## Implementation

- `firmware/lib/JpegToBmpConverter/CoverDither.h` contains the bounded fixed-point
  processor: tone curve, two 3x3 bilateral passes, coherent opposing-gradient edge
  gate, clipped local edge push, and an 8x8 single-pixel threshold screen.
- JPEG exact 1-bit Home output now retains the target grayscale plane until the
  final processor runs, so dithering is not followed by another conversion step.
- BMP and PNG exact Home output use the same processor. PNG Home output now emits
  an exact 1-bit BMP instead of the legacy 8-bit grayscale intermediate.
- Temporary memory is target-sized; the processor uses no floating point or error
  diffusion rows.

## Verification

- `test_cover_dither.cpp`: PASS with strict host compiler warnings.
- `test_m4_dependency_bootstrap_contract.py`: PASS.
- Production PlatformIO compile was attempted with an isolated core, but the
  repository bootstrap failed before source compilation because the pinned
  `einklover/m4-device@f86b134` archive returns HTTP 404.
- No device flash or QEMU run was performed.

## Follow-up compile

- After the in-tree FreeInk/vendor dependencies were restored, the production
  command `cd firmware && $HOME/.platformio/penv/bin/pio run -e murphy_m4 -j1`
  completed successfully.
- Firmware size report: RAM 108196/327680 (33.0%), Flash 5352037/7143424
  (74.9%).
- The first source compile exposed a `PNGdec` macro collision with the variable
  name `local`; it was renamed to `toneLevel` in `CoverDither.h`.
