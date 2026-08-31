# Round 9 Luna — design-sheet icons

Date: 2026-09-01
Sheet: `docs/orchestration/assets/icon-design-sheet-round9.png` (1448×1086, RGB)
Branch: `agent/home-luna-audit`

## Result

The three plugin `icon_home.bmp` resources now use distinct generic reading glyphs from the sheet. `HomeSceneAssetDecoder` owns the compiled 62×64 1-bit builtin bitmaps, and `AppListActivity` uses the same 1-bit draw loop for matched builtins before falling back to the legacy `UIIcon` glyphs for unmatched builtins. Plugin manifests were unchanged.

The pre-existing dirty changes in `firmware/src/activities/home/HomeSceneAssetDecoder.cpp` and `firmware/tests/native_app/test_home_lifecycle_uaf.cpp` were preserved; no reset, clean, or revert was used.

## Crop map

Coordinates are `(x0, y0, x1, y1)` in the source sheet. Each saved preview is a 4× nearest-neighbour view of a 62×64 thresholded crop; the ink ratio is black pixels / 3968 visible pixels.

| id | sheet cell | glyph / rationale | saved crop | ink |
|---|---|---|---|---:|
| `builtin.files` | r1c1 | folder | [files preview](../assets/round-9-crops/files-preview.png) | 663 (16.71%) |
| `builtin.history` | r1c2 | document + clock | [history preview](../assets/round-9-crops/history-preview.png) | 680 (17.14%) |
| `builtin.bookmarks` | r1c3 | document + bookmark | [bookmarks preview](../assets/round-9-crops/bookmarks-preview.png) | 806 (20.31%) |
| `builtin.network` | r2c1 | Wi-Fi | [network preview](../assets/round-9-crops/network-preview.png) | 397 (10.01%) |
| `builtin.settings` | r2c2 | gear | [settings preview](../assets/round-9-crops/settings-preview.png) | 634 (15.98%) |
| `com.jjwxc.client` | r2c3 | reader with book; distinct book/text glyph | [jjwxc preview](../assets/round-9-crops/jjwxc-preview.png) | 840 (21.17%) |
| `com.weread.client` | r3c2 | book in a chat bubble; distinct book/text glyph | [weread preview](../assets/round-9-crops/weread-preview.png) | 529 (13.33%) |
| `com.fanqie.client` | r3c3 | open book; distinct book/text glyph | [fanqie preview](../assets/round-9-crops/fanqie-preview.png) | 651 (16.41%) |

Saved source crops are the corresponding `*-source.png` files in the same directory. Tight source crop boxes were: files `(139,113,308,267)`, history `(482,95,659,280)`, bookmarks `(827,95,987,270)`, network `(127,439,316,582)`, settings `(471,420,640,589)`, jjwxc `(827,420,979,592)`, weread `(460,728,659,903)`, and fanqie `(805,746,1005,900)`.

The plugin mapping is intentionally generic rather than a claimed brand logo. The sheet also has tomato and other reading-adjacent cells; those remain open alternatives if product review prefers them.

## Code and assets

- Replaced the three plugin BMPs with valid 574-byte, 62×64, 1-bpp BMPs using palette index 0 = black and 1 = white; each manifest still declares `icon_home.bmp`.
- Added five compiled renderer-native 1-bit arrays and `builtinSheetIcon()` for files/history/bookmarks/network/settings.
- `decodeBuiltinFilesIconForPublication()` now publishes the sheet folder bitmap directly into the existing 62×64 Home asset slot.
- `AppListActivity::drawItemIcon()` now draws plugin and matched builtin assets through one 1-bit MSB-first loop. OPDS, JianGuo, Data Capsule, and other unmatched builtins retain their existing `UIIcon` fallback.
- No Scene framework or theme runtime changes were made.

## Commands and results

- `python3 firmware/tests/test_m4_dependency_bootstrap_contract.py` — PASS.
- `python3 firmware/tests/test_plugin_home_icon_resource.py` — PASS.
- `/opt/anaconda3/bin/pytest -q firmware/tests/test_plugin_home_icon_resource.py firmware/tests/test_home_app_icon_62x64.py firmware/tests/test_home_app_drawer.py firmware/tests/test_app_drawer_3col.py firmware/tests/test_home_dock_publication.py` — **19 passed**.
- `/opt/homebrew/bin/g++-14 -std=c++17 -I firmware/src -o /tmp/round9_home_decoder_hardening firmware/tests/native_app/test_home_decoder_hardening.cpp firmware/src/activities/home/HomeSceneAssetDecoder.cpp firmware/src/ui/pages/HomeSceneModel.cpp`, then `/tmp/round9_home_decoder_hardening` — PASS.
- Same host build/run pattern for `test_home_lifecycle_uaf.cpp` — PASS.
- Dedicated host helper check for all five `builtinSheetIcon()` ids — PASS.
- `PLATFORMIO_HOME_DIR=/tmp/pio_home2 /Users/zhouxinlai/.platformio/penv/bin/pio run -e murphy_m4 -j1` — SUCCESS; flash 5,348,961 bytes, 74.9% of app partition.
- `PLATFORMIO_HOME_DIR=/tmp/pio_home2 /Users/zhouxinlai/.platformio/penv/bin/pio run -e murphy_m4_qemu_plugin -j1` — SUCCESS; flash 5,358,493 bytes, 75.0% of app partition.
- `git diff --check` — PASS.

## QEMU evidence

The fresh plugin image was booted with:

```text
PLATFORMIO_HOME_DIR=/tmp/pio_home2 ./m4sim run --plugin-debug --skip-build --no-hostfwd --ready-seconds 20
```

The capture session reported `activity: Home`, `screen_w: 480`, `screen_h: 800`, and `sd_ok: true`. The SD fixture was stopped before updating the three plugin BMPs with `mcopy`; it was then booted once for the final frames. The raw framebuffer is physical 800×480; the `*-portrait.png` files are rotated 270° into logical 480×800.

- Home raw: `tmp-home-screenshots/round-9-luna/02-home.png`
- Home portrait: `tmp-home-screenshots/round-9-luna/02-home-portrait.png`
- AppList raw: `tmp-home-screenshots/round-9-luna/03-applist.png`
- AppList portrait: `tmp-home-screenshots/round-9-luna/03-applist-portrait.png`
- [Home portrait URL](http://179.255.101.123/share/mdBCdD-zIJi5BRcYIDJ9JQ/02-home-portrait.png)
- [AppList portrait URL](http://179.255.101.123/share/qIDgXPgTTeaLZ26k_lduIQ/03-applist-portrait.png)

QEMU is simulator evidence only; no device flash was performed.

## SHA-256

```text
e321b47c1f45a63a27efe31849464ed6972f3847716b3d40f9e7c78eacb9df55  docs/orchestration/assets/icon-design-sheet-round9.png
361c2984f36dbe60d9bed8fb018f4850c3493ea0ee4b30d492a1489ae520f966  plugins/m4-fanqie-plugin/icon_home.bmp
aa7c6164c3a4e14f9bac796125ab2f99c320344da9d32a663321f0e425eac6a7  plugins/m4-weread-plugin/icon_home.bmp
236245baf8b7604d642c859c878b932312dc68a5e5ff965ad4c17072e11dd99a  plugins/m4-jjwxc-plugin/icon_home.bmp
f547e79b94c254fb5c203ab81f35cfe9e9d12dccd3901f99b613081675a2d820  tmp-home-screenshots/round-9-luna/02-home-portrait.png
a07386d56740192a4a9fb74d91c620c70cf3f86780650a9306fbfac4e37cfe32  tmp-home-screenshots/round-9-luna/03-applist-portrait.png
```

## Open questions

- The three plugin cells are generic reading symbols, not official logos. Current mapping is open book → Fanqie, book-in-chat → WeRead, and reader-with-book → JJWXC; human review may choose the tomato cell or another reading glyph later.
- Bookmarks is wired when the conditional `builtin.bookmarks` drawer item exists. OPDS, JianGuo, and Data Capsule remain legacy gray glyphs because the sheet has no unambiguous transfer/cloud symbols.
- No hardware result is claimed; the evidence above is from patched QEMU only.
