# Round 10 — Luna routes and header inset

Date: 2026-09-01

Branch: `agent/home-luna-audit`
Base: integration `172067e`

## Result

- `文件管理` now opens the local `MyLibrary` path browser.
- `网络管理` now opens the existing Settings `网络与同步` L2, rather than
  Settings root, a Wi-Fi-only picker, or the transfer server.
- `CrossPointWebServer` remains reachable through the explicit
  `wifi_transfer` USB/debug hook and the existing `文件传输` entry.
- Fengyan non-Home headers use a 12 px safe top inset. The Home reference
  header path remains on its separate `HomeHeader*` geometry.

## Root cause

`builtin.files` was named as a file manager tile but used the
`FileTransfer` action and `onGoToFileTransfer()`, which entered
`CrossPointWebServerActivity`. The network callback entered `WifiSelectionActivity`
directly, which was too narrow for `网络管理`.

`AppListActivity::Callbacks` was also constructed positionally in `main.cpp`.
That made adding or reordering a callback able to shift DataCapsule/Network/
Settings destinations without a compiler error. The wiring is now assigned by
field name, and the file action has its own `FileManager` enum/callback.

Fengyan had `topPadding = 0`; `AppListActivity` consequently passed `y = 0`
to `drawHeader()`. With `HeaderTitleBaseline = 31`, the observed title ink
started at the physical panel edge region.

## Builtin destination matrix

| Drawer label / id | Action and callback | Destination |
| --- | --- | --- |
| 文件管理 / `builtin.files` | `FileManager` → `onGoToMyLibrary` | `MyLibraryActivity` local path browser |
| 阅读历史 / `builtin.history` | `RecentBooks` → `onGoToRecentBooks` | `RecentBooksActivity` |
| OPDS（conditional） / `builtin.opds` | `Opds` → `onGoToBrowser` | `OpdsBookBrowserActivity` |
| 坚果云（conditional） / `builtin.jianguo` | `JianGuo` → `onGoToJianGuoYun` | JianGuo browser |
| 数据舱（conditional） / `builtin.datacapsule` | `DataCapsule` → `onGoToDataCapsule` | DataCapsule browser |
| 书签笔记（conditional） / `builtin.bookmarks` | `BookmarkNotes` → `onGoToBookmarkNotes` | Bookmark notes |
| 网络管理 / `builtin.network` | `Network` → `onGoToNetwork` | Settings `网络与同步` L2 |
| 设置 / `builtin.settings` | `Settings` → `onGoToSettings` | Settings hub root |

The optional rows retain their existing configured-state gates. The explicit
transfer path is separate from this matrix: the Home `文件传输` entry and the
debug bridge `wifi_transfer` hook call `onGoToFileTransferUsb()`, which enters
`CrossPointWebServerActivity(..., true)`.

## Header measurements

Measurements use the first dark pixel in the title region and a full-width
dark-pixel scan for the divider:

| Screen | Before | After |
| --- | ---: | ---: |
| AppList title first ink | y=5 | y=19 |
| AppList divider | y=45 | y=57 |
| Settings `网络与同步` title first ink | — | y=24 |

Before values are from the coordinator's device capture; after values are
from the fresh QEMU image. The AppList acceptance threshold is y > 8, so the
new y=19 result has visible air above the glyphs.

## Evidence screenshots

Pre-fix coordinator captures:

- `docs/orchestration/assets/round-10-device/04-applist-rgb.png`
- `docs/orchestration/assets/round-10-device/04-applist-topbar-crop.png`
- `docs/orchestration/assets/round-10-device/07-after-files-rgb.png` — old
  `CrossPointWebServer` result for 文件管理.
- `docs/orchestration/assets/round-10-device/08-after-network-rgb.png` — old
  Settings-root result for 网络管理.

Fresh QEMU logical 480×800 captures after the fix:

- `tmp-home-screenshots/round-10-luna/05-applist-final-logical.png` — title
  inset and drawer labels.
- `tmp-home-screenshots/round-10-luna/06-file-manager-final-logical.png` —
  `SD卡` / `apps_data` local path browser.
- `tmp-home-screenshots/round-10-luna/07-network-final-logical.png` —
  Settings `网络与同步` L2.

The corresponding `*-physical.png` files retain the QEMU panel orientation.

## Verification

Focused host tests:

```text
/opt/anaconda3/bin/pytest -q \
  firmware/tests/test_round10_applist_routes_header.py \
  firmware/tests/test_app_drawer_3col.py \
  firmware/tests/test_home_app_drawer.py \
  firmware/tests/test_m4_dependency_bootstrap_contract.py
14 passed

/opt/homebrew/bin/g++-14 -std=c++17 \
  firmware/tests/native_app/test_app_drawer_handoff.cpp \
  -o /tmp/test_app_drawer_handoff_round10 && /tmp/test_app_drawer_handoff_round10
app drawer handoff contracts: ALL PASS
```

QEMU journey (`murphy_m4_qemu_plugin`, no hardware flash):

```text
Home → tap (437,611) → AppList
AppList → tap (90,132) → MyLibrary
AppList → tap (390,132) → Settings (网络与同步 L2)
wifi_transfer → CrossPointWebServer
```

The structured status results were `MyLibrary`, `Settings`, and
`CrossPointWebServer` respectively. The final QEMU build passed with 32.8% RAM
and 75.0% flash. The final production compile also passed with 33.0% RAM and
74.9% flash:

```text
PLATFORMIO_HOME_DIR=/tmp/pio_home2 \
  /Users/zhouxinlai/.platformio/penv/bin/pio run -e murphy_m4_qemu_plugin -j1
PLATFORMIO_HOME_DIR=/tmp/pio_home2 \
  /Users/zhouxinlai/.platformio/penv/bin/pio run -e murphy_m4 -j1
```

No device was flashed and nothing was pushed. Existing dirty files outside
the Round 10 owned set were preserved.

## Diff summary

- Renamed the AppList file action/callback to `FileManager` /
  `onFileManagerOpen` and wired it to `MyLibrary`.
- Added an initial Settings pane/hub handoff so `网络管理` lands directly in
  the existing Network & Sync L2.
- Replaced positional AppList callback initialization with named assignments.
- Added `HomeRef::HeaderSafeTop = 12` and applied it to Fengyan metrics without
  changing the Home reference header path.
- Added `firmware/tests/test_round10_applist_routes_header.py`.

## Open questions

- Optional cloud/data-capsule rows are absent from the clean QEMU SD image, so
  their configured-state destinations are covered by the named wiring contract
  and the source matrix; the production device can recheck them after merge.
- The existing `文件传输` flow still owns CrossPoint hotspot/transfer-server
  setup, intentionally separate from both AppList labels.
