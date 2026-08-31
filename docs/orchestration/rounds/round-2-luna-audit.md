# Round 2 — Luna drawer audit

## Drawer item list

The app drawer is a four-column e-ink grid. Items are assembled when the drawer
opens, in this order:

1. 文件管理 — `onGoToFileTransfer()` / `CrossPointWebServerActivity`
2. 阅读历史 — `onGoToRecentBooks()` / `RecentBooksActivity`
3. OPDS 浏览器 — shown when `SETTINGS.opdsServerUrl` is non-empty
4. 坚果云盘 — shown when `SETTINGS.jgUsername` is non-empty
5. 数据胶囊 — shown when `SETTINGS.dcUsername` is non-empty
6. 书签笔记 — shown when `BookmarkStore::hasAnyBookmarks()` is true
7. 网络管理 — Wi-Fi network selection/management
8. 系统设置 — `onGoToSettings()` / `SettingsActivity`
9. Installed M4x plugins — names and registry entries from `M4xRegistry::load()`;
   valid 62×64 BMP icons reuse `HomeSceneAssetDecoder`, with the standard
   library icon as fallback.

## Navigation map

- Home `open_apps` → `onAppsOpen` → `onGoToApps` → `AppListActivity`.
- Tap a tile or press Confirm to activate it; built-ins use the supplied main
  callbacks and plugins remain deferred child activities.
- Network management opens `WifiSelectionActivity`; cancel or completion returns
  home without erasing credentials.
- Back/edge-back from the drawer returns home.
- The existing `open_history` → `onRecentsOpen` → `RecentBooksActivity` path was
  not re-plumbed.

## Uninstall policy

Only a selected installed plugin can enter uninstall mode. A long press on a
plugin tile or the left action opens the existing confirmation dialog, including
the clear-data choice. Built-in drawer items never expose or execute uninstall.
The right action still opens the existing `.m4x` install flow.

## Files changed

- `firmware/src/activities/apps/AppListActivity.cpp`
- `firmware/src/activities/apps/AppListActivity.h`
- `firmware/src/main.cpp`
- `firmware/src/I18n.h`
- `docs/orchestration/rounds/round-2-luna-audit.md`

## Validation and residual risks

- Passed `python3 simulator/tests/test_track_d_weread_back_contract.py`.
- Passed `python3 firmware/tests/test_m4_dependency_bootstrap_contract.py`.
- Passed `git diff --check`.
- PlatformIO/QEMU and hardware were not run per the lane instruction; the
  on-device grid layout and touch feel remain unverified.
- Plugin artwork is intentionally limited to the existing exact-size BMP decode
  contract; malformed, missing, or other-format artwork falls back to the
  generic plugin icon.
- “Network manage” is routed to the existing Wi-Fi selector directly; the
  existing file-transfer activity remains the separate file-manager shortcut.
- Pre-existing unrelated dirty files in the worktree were left untouched.
