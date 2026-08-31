# Round 7 — Luna full QEMU UX report

Date: 2026-08-31
Workspace: `m4-home-luna-audit`
Baseline: `e4e3f85` (`round-6(luna): rebalance Home type sizes/positions vs mockup`)

## Result

The Home recent-card and Settings navigation defects are fixed and re-proven on the final `murphy_m4_qemu_plugin` image. Home now opens each of the three seeded recent books, Settings opens the display/read subpage, AppList/dock navigation works, and the Fanqie, WeRead, and JJWXC plugin surfaces were exercised. Plugin book/detail footers now use the supported plain `返回` glyph instead of rendering `« 返回` as `?返回`.

QEMU was stopped after the final capture. No hardware was flashed and nothing was pushed to `origin`.

## Root cause and fix

Scene actions were serialized on a `repeat` command prefix, but the runtime only read action metadata from each child node. Repeated cover/title nodes therefore rendered without an action. When a seeded recent item had no cover bitmap, the runtime also discarded the cover event entirely, removing its hit geometry.

The fix:

- propagates effective action and argument metadata through groups and repeats;
- keeps actionable repeated cover/icon geometry alive while the asset is unavailable, allowing the renderer placeholder to remain tappable;
- adds `open_current_book` to the repeated Home cover/title nodes;
- carries the recent item index through `HomeSceneModel` and opens that recent path in `HomeActivity`;
- changes plugin book/app/login error and loading footers to the font-safe `返回` label.

The existing dirty files `firmware/src/activities/home/HomeSceneAssetDecoder.cpp` and `firmware/tests/native_app/test_home_lifecycle_uaf.cpp` were preserved and left unstaged.

## Final QEMU evidence

All coordinates are logical 480×800 coordinates. Each final interaction was performed as `m4adb tap x y`, followed by `m4adb ui` and a PBM/PNG screenshot.

| Journey | Tap | `m4adb ui` result | Screenshot |
|---|---:|---|---|
| Home recent 1 | `(92,440)` | `Reader` → `NativeProviderBook`, Fanqie `lizhi1` | [41-home-cover-1-final-tap.png](../../../tmp-home-screenshots/round-7-ux/41-home-cover-1-final-tap.png) |
| Home recent 2 | `(235,440)` | `Reader` → `NativeProviderBook`, Fanqie `santi1` | [42-home-cover-2-final-tap.png](../../../tmp-home-screenshots/round-7-ux/42-home-cover-2-final-tap.png) |
| Home recent 3 | `(378,440)` | `Reader` → `NativeProviderBook`, Fanqie `pingfan1` | [43-home-cover-3-final-tap.png](../../../tmp-home-screenshots/round-7-ux/43-home-cover-3-final-tap.png) |
| Home → AppList | `(430,610)` | `Home` → `AppList` | [44-app-list-final.png](../../../tmp-home-screenshots/round-7-ux/44-app-list-final.png) |
| AppList → Settings | `(90,245)` | `AppList` → `Settings` | [45-settings-hub-final.png](../../../tmp-home-screenshots/round-7-ux/45-settings-hub-final.png) |
| Settings display/read | `(240,120)` | remains `Settings`; rendered page changes to `显示与阅读` | [46-settings-display-final.png](../../../tmp-home-screenshots/round-7-ux/46-settings-display-final.png) |

The Settings activity intentionally keeps its activity name while changing the scene page; the before/after frames are the proof of the page transition. The final display page has three full-width footer targets: `返回`, `主页`, and `历史`.

Before the fix, the equivalent Home taps at `(87,500)`, `(235,480)`, and `(378,480)` stayed on `Home`; Settings `(240,120)` stayed on the hub. The unchanged failing captures are [03](../../../tmp-home-screenshots/round-7-ux/03-after-mini-1-tap-failing.png), [07](../../../tmp-home-screenshots/round-7-ux/07-after-mini-2-tap-failing.png), [08](../../../tmp-home-screenshots/round-7-ux/08-after-mini-3-tap-failing.png), and [06](../../../tmp-home-screenshots/round-7-ux/06-settings-hub-tap-failing.png).

## Plugin UX pass

- Fanqie: AppList launch, category/list screen, a 24-book result set, book detail, start reading, network error/retry, footer refresh, and footer return were exercised. [50-fanqie-home-final.png](../../../tmp-home-screenshots/round-7-ux/50-fanqie-home-final.png) shows the spaced category/list layout and full-width `返回`/`刷新` targets; [51-fanqie-detail-final.png](../../../tmp-home-screenshots/round-7-ux/51-fanqie-detail-final.png) shows the corrected detail footer. The final footer taps were `(350,770)` → retained Fanqie home with `discovery_phase:3`, then `(120,770)` → `Home`.
- JJWXC: AppList launch, category discovery, 24-book list, detail, and reading were exercised. [47-jjwxc-home-final.png](../../../tmp-home-screenshots/round-7-ux/47-jjwxc-home-final.png), [48-jjwxc-detail-final.png](../../../tmp-home-screenshots/round-7-ux/48-jjwxc-detail-final.png), and [49-jjwxc-reader-final.png](../../../tmp-home-screenshots/round-7-ux/49-jjwxc-reader-final.png) cover the journey. The detail view has a legible `返回` footer and a large `开始阅读` target.
- WeRead: AppList launch reaches the guest QR-login flow with a live countdown, captured in [52-weread-login-final.png](../../../tmp-home-screenshots/round-7-ux/52-weread-login-final.png). No account was available, so the authenticated book list could not be opened; this is recorded as a product-access gap, not treated as a tap failure.
- AppList/dock: [40-home-final-reboot.png](../../../tmp-home-screenshots/round-7-ux/40-home-final-reboot.png) shows the seeded Home dock with Files, WeRead, Fanqie, and JJWXC; [44-app-list-final.png](../../../tmp-home-screenshots/round-7-ux/44-app-list-final.png) shows the full seven-app grid. The actual WeRead tile tap `(385,245)` reached the QR-login state.

The simulator was run with guest networking enabled. Final ping reported `wifi_connected:true`, SSID `qemu-openeth`, IP `10.0.2.15`. Home was seeded with:

```text
python3 simulator/tools/seed_home_recents.py --sd /tmp/m4sim-round7-ux/artifacts/murphy-sd.img
```

No `--no-net` option was used. The fixture recents do not include cover bitmap paths, so the final Home cards intentionally show the renderer's clean placeholder covers while remaining fully tappable. Fanqie/JJWXC network-backed data loaded; QEMU's outbound chapter/catalog requests intermittently returned `http_ESP_ERR_HTTP_FETCH_HEADER` or `catalog_empty`, and the UI exposed retry rather than hanging. This is a simulator/network limitation, not hardware evidence.

## Verification

Passed:

```text
python3 firmware/tests/test_m4_dependency_bootstrap_contract.py
g++-14 ... test_ui_scene_runtime.cpp && /tmp/round7_test_ui_scene_runtime
g++-14 ... test_home_scene_model.cpp HomeSceneModel.cpp && /tmp/round7_test_home_scene_model
PLATFORMIO_HOME_DIR=/tmp/pio_home2 pio run -e murphy_m4_qemu_plugin -j1
PLATFORMIO_HOME_DIR=/tmp/pio_home2 pio run -e murphy_m4 -j1
```

Final image usage: QEMU plugin build RAM 32.8%, flash 75.0%; production build RAM 33.0%, flash 74.8%.

## Changed files

- `firmware/src/ui/scene/UiSceneRuntime.h`
- `themes/murphy-default/theme.json`
- `firmware/src/generated/murphy_default_m4theme.h`
- `firmware/src/ui/pages/HomeSceneModel.cpp`
- `firmware/src/activities/home/HomeActivity.cpp`
- `firmware/src/activities/apps/NativeAppActivity.cpp`
- `firmware/src/activities/apps/NativeProviderBookActivity.cpp`
- `firmware/src/activities/apps/NativeProviderLoginActivity.cpp`
- `firmware/tests/native_app/test_ui_scene_runtime.cpp`
- `firmware/tests/native_app/test_home_scene_model.cpp`
- `docs/M4_AGENT_LESSONS.md`
- `tmp-home-screenshots/round-7-ux/`
