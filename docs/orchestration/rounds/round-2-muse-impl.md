# Round 2 — Muse impl (Lane A)

## Files changed
- `plugins/m4-weread-plugin/icon_home.bmp` — regenerated 62x64 1-bit BMP (574 bytes, stride 8, 512 pixel bytes) from `docs/orchestration/assets/mockup-icon-weread.png`
- `plugins/m4-fanqie-plugin/icon_home.bmp` — same, from `mockup-icon-fanqie.png`
- `plugins/m4-jjwxc-plugin/icon_home.bmp` — same, from `mockup-icon-jjwxc.png`
- `firmware/src/activities/home/HomeSceneAssetDecoder.h` — added `decodeBuiltinFilesIconForPublication` declaration
- `firmware/src/activities/home/HomeSceneAssetDecoder.cpp` — implemented `decodeBuiltinFilesIconForPublication` with compiled 512-byte `kBuiltinFilesIcon` array (62x64, stride 8, ink on white). Array generated from `mockup-icon-files.png` with identical pipeline as plugin icons. Handles empty install path (no SD) and works on host without filesystem.
- `firmware/src/activities/home/HomeActivity.cpp` — changed `$apps` publication and asset decode to meet dock contract:
  - `publishHomeSceneFromBackendCtx`: always `addApp("builtin.files","文件管理","builtin.files")` at slot 0, then prefer `com.weread.client` → `com.fanqie.client` → `com.jjwxc.client` if found in `M4xRegistry::load()`, then fill remaining from registry in load order without duplicating preferred or builtin. Preserves `kMaxAppItems=4`. Keeps Fengyan fallback dead (hasApps always true after builtin).
  - `publishHomeSceneWithAssetsCtx`: decodes in snapshot order by extracting `appId` via `snapshot.textView`. If `builtin.files`, calls `decodeBuiltinFilesIconForPublication`; else lookup registry entry by id and call `decodeAppIconForPublication`. Ensures slot 0 builtin icon decodes even with empty install path.

No changes to `AppListActivity`, `main.cpp`, `I18n.h`, tests, or other plugins. `plugins/*/manifest.json` left untouched (already declare `icon:"icon_home.bmp"` and `files` includes it).

## How icons were converted
- Tool: `/usr/bin/python3` + PIL (Homebrew `python3` has no PIL). Reused for all 4 icons.
- Pipeline per PNG (`docs/orchestration/assets/mockup-icon-{files,weread,fanqie,jjwxc}.png`, 77–81×120 RGB):
  1. Find bbox where `L < 200` (grayscale) with 2px padding; fallback to full image. Tightens the icon region so faint light icons (weread min 48, fanqie min 59) become enlarged instead of diluted.
  2. Crop to bbox, compute `scale = min(62/w, 64/h)`, resize cropped to `new_w×new_h` via `Image.NEAREST` (preserves hard 1-bit edges; `LANCZOS` lightens dark lines from 48→115).
  3. Paste centered onto 62×64 white `RGB(255,255,255)` canvas.
  4. Convert to `L`, threshold `128` (`lum <128 → black`, else white). `1=black ink on white` e-ink friendly. Built raw as `bytearray(512)` where bit `0` = black (palette entry 0), bit `1` = white (palette entry 1), MSB first, `stride=8`.
  5. Write Windows BMP `574 bytes`: file header `14` + DIB `40` (`w=62,h=64` positive bottom-up, `planes=1,bpp=1,comp=0`) + palette `8` (`00 00 00 00 / FF FF FF 00`) + pixel data `512` bottom-up rows reversed. Verified `bfOffBits=62`, `bpp=1`, `abs(w)=62,abs(h)=64`.
- Results: files `160` black pixels, weread `85`, fanqie `60`, jjwxc `88` (out of 3968 effective). Placeholder BMPs had ~312 black and were uniform; new BMPs have distinct icon shapes and are not empty. Threshold `128` and NEAREST are deterministic; Atkinson would also satisfy spec but threshold gives cleaner line art.

## How builtin files is published
- Id `builtin.files`, name `文件管理`, icon field `builtin.files` (arbitrary non-empty; decoder ignores it for builtin). Always slot 0.
- Registry still loaded for plugins; preferred order is hard-coded `kPreferredIds[3]` as above, but actual `name`/`icon` taken from registry entry (so pack name/version stay authoritative). Fill loop iterates registry in `load()` order, skipping `builtin.files` and any preferred already added, until `addApp` fails (full).
- Decoding: `decodeBuiltinFilesIconForPublication` validates `homePublicationSlotForKey`, checks `62×64×8/512`, copies `kBuiltinFilesIcon` into `pub.arena+offset`, then `homeAddAssetToPublication`. No `SdMan` or `Bitmap` call, so QEMU/device can load it without SD and host stub passes.

## Commands run
```bash
/usr/bin/python3 firmware/tests/test_plugin_home_icon_resource.py  # PASS 3/3
/usr/bin/python3 firmware/tests/test_home_app_icon_62x64.py          # PASS 3/3
python3 -m pytest firmware/tests/test_plugin_home_icon_resource.py firmware/tests/test_home_app_icon_62x64.py -v  # 6 passed
# Host compile sanity for builtin (g++-14):
/opt/homebrew/bin/g++-14 -std=c++17 -I firmware/src -I firmware/src/avr -o /tmp/test_builtin2 /tmp/test_builtin2.cpp firmware/src/activities/home/HomeSceneAssetDecoder.cpp firmware/src/ui/pages/HomeSceneModel.cpp && /tmp/test_builtin2
# white 3808 black 160, builtin decode OK
```
- BMP verification: `struct` header check `62×64×1`, `574` bytes, `bfOff=62`, pixel data `512`, reconstructed top-down matches raw.

## What you did NOT verify (QEMU is coordinator)
- Full `murphy_m4_qemu` / `murphy_m4_qemu_plugin` PlatformIO builds (forbidden per task: `Do not run pio`).
- Fresh QEMU framebuffer pixel/perceptual diff vs authoritative `home-mockup.jpg` / `AUTHORITATIVE_HOME_TARGET_480x800.png`.
- Device flashing or `m4adb` plugin install journeys.
- Drawer (`AppListActivity`, `main.cpp`) routing for `builtin.files` → file manager (Lane B ownership).

