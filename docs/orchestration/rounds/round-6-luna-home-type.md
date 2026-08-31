# Round 6 Luna Home Type Rebalance

Date: 2026-08-31

## Scope

Compared the original Home mockup with the current loaded-data simulator frame. The old Home chrome was visually oversized and inconsistent: the header, hero title, section labels, recent titles, and dock labels did not share the intended hierarchy, while the progress text was too small. The right-aligned `All`/`More` controls also started too far right inside their slots.

## Before and after

| Home role | Before | After |
|---|---|---|
| Header logo | `ui_16_bold` | `ui_22_bold` |
| Continue-reading label | `ui_14_regular` | `ui_16_regular` |
| Hero title | `ui_20_bold` | `ui_24_bold` |
| Author and source | `ui_14_regular` | `ui_16_regular` |
| Progress text | `ui_12_regular` | `ui_16_regular` |
| Recent and Apps section headers | `ui_16_bold` | `ui_20_bold` |
| All and More controls | `ui_14_regular`, right aligned | `ui_16_regular`, left aligned |
| Recent book titles | `ui_14_regular` | `ui_16_regular` |
| App dock labels | `ui_12_regular` | `ui_16_regular` |

Key geometry remains locked to the Home reference: the hero card is `[22,84,434,230]`, hero title is `[209,142,220,52]`, author/source are at y=198/220, progress text is `[209,253,50,18]`, recent/app headers are at y=347/601, and cover/icon geometry is unchanged. `All` and `More` retain `[401,347,72,20]` and `[401,601,72,20]`; their alignment now starts at the measured x≈401.

The scene runtime mapping was updated only in `GfxSceneRenderer::runtimeFontId`: 16px, 20px, and 22px scene roles use the fixed system-small face so they cannot inherit the 26px reader face in the QEMU `OMIT_FONTS` build; the 24px hero title uses the fixed 24px hub face. This makes the rendered Home hierarchy match the mockup while keeping the authored HomeRef aliases and bindings intact.

## Build and host validation

- `python3 firmware/tools/compile_home_theme.py --theme themes/murphy-default/theme.json --out /tmp/round-6-murphy-default.m4theme --emit-header firmware/src/generated/murphy_default_m4theme.h`
  - PASS; 48,728-byte package, CRC32 `74d5927d`.
- `python3 firmware/tests/test_m4_dependency_bootstrap_contract.py`
  - PASS.
- `/opt/anaconda3/bin/pytest firmware/tests/test_home_typography_polish.py firmware/tests/test_home_font_hierarchy.py firmware/tests/test_murphy_default_exact_geometry.py firmware/tests/test_home_theme_compiler.py -v`
  - PASS; 60 tests.
- `PLATFORMIO_HOME_DIR=/tmp/pio_home2 /Users/zhouxinlai/.platformio/penv/bin/pio run -e murphy_m4_qemu_plugin -j1` from `firmware/`
  - PASS; QEMU firmware image rebuilt successfully.

## Loaded-data QEMU proof

QEMU was run with `--plugin-debug --skip-build --no-hostfwd --ready-seconds 20` and guest networking enabled. The SD image was seeded with:

```text
python3 simulator/tools/seed_home_recents.py --sd /tmp/m4sim-round6-luna/artifacts/murphy-sd.img
```

The seeded records supplied the Home hero and three recents. Tiny Fanqie and WeRead packages were installed only into the simulator; two Fanqie books were opened through the guest network. The final `m4adb ui` response reported `activity: Home`, `sd_ok: true`, `screen_w: 480`, `screen_h: 800`, and Wi-Fi connected. The final frame visibly contains the plugin hero title/author/progress, recent titles, and three dock labels.

- Raw m4adb proof: `/tmp/m4sim-round6-luna/artifacts/qemu-home-after.pbm`
- Final RGB proof: `tmp-home-screenshots/round-6-type/qemu-home-after.png`
- Final RGB dimensions/mode: 480×800, RGB
- Final screenshot SHA-256: `dac99e4eed9a486a2e3b2afaedec8e1011fe843af5833eb2eab1705fef38dffb`

No hardware was flashed, no push or merge was performed, and the pre-existing dirty `HomeSceneAssetDecoder.cpp` and `test_home_lifecycle_uaf.cpp` files were left untouched. QEMU was stopped after capture.
