# orch: merge luna-cover edge-retaining halftone

- Source: `agent/home-luna-cover` tip `340bff5` (`feat(m4): add edge-retaining cover halftone`)
- Integration tip after integrate: `50967cd` (cherry-pick of `340bff5` onto `59fae22`)
- Why cherry-pick instead of `merge --no-ff`: cover lane also carried parallel vendor commits (`6b5c278`/`85fa513`) forked from `9ebda92`, which would conflict with and regress integration's later R10b–R14 + vendor (`4cc506a`/`a2bea72`) history. Only the dither payload was needed.

## Verification on integration

- `test_cover_dither.cpp` host: PASS (`g++-14 -std=c++17 -Wall -Wextra -Werror`)
- `test_m4_dependency_bootstrap_contract.py`: PASS
- `pio run -e murphy_m4 -j1`: SUCCESS (RAM 33.0%, Flash 74.9%)
- Not flashed in this merge turn.

## Files brought in

- `firmware/lib/JpegToBmpConverter/CoverDither.h`
- `firmware/lib/JpegToBmpConverter/JpegToBmpConverter.cpp`
- `firmware/src/util/M4ProviderCoverCache.cpp`
- `firmware/tests/native_app/test_cover_dither.cpp`
- `docs/orchestration/rounds/cover-luna-dither-firmware.md`
