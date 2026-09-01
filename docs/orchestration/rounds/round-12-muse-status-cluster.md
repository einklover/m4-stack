# Round 12 — Muse non-Home status cluster (clock / Wi-Fi / battery)

Branch: `agent/home-muse-impl`
Base: merge of `agent/home-orch-integration` (4d51a70) with locked chrome HeaderY=0 HeaderSafeTop=0 HeaderH=46 HeaderTitleBaseline=38

## Locked chrome (preserved)
- `HomeRef::HeaderY = 0`
- `HomeRef::HeaderSafeTop = 0` (was 20 in integration, reset to locked 0)
- `HomeRef::HeaderH = 46`
- `HomeRef::HeaderTitleBaseline = 38`
- `HomeHeaderH=60 HomeHeaderTitleBaseline=43 HomeHeaderWifiX=430` unchanged
- `FengyanMetrics::values.topPadding = HomeRef::HeaderSafeTop`, `headerHeight = HomeRef::HeaderH`

## Bugs fixed

### 1) Wi-Fi glyph right-half only
File: `firmware/src/components/themes/fengyan/FengyanTheme.cpp:127`
- Deleted `drawArc(r, cx, cy, -1, -1, ...)` left upper quadrant
- Kept `drawArc(r, cx, cy, 1, -1, ...)` right upper quadrant only, no lower quadrants
- Kept 2x2 origin dot and radii `{5,10,15}`
- Shifted `cx` left by 6 (`x + s/2 - 6`) so visible right-half fan is optically centered in 22px slot (otherwise hugs right edge). Home header uses same helper and inherits fix.

### 2) Vertical alignment — one optical center
File: `firmware/src/components/themes/fengyan/FengyanTheme.cpp:270`
- Battery double-offset bug removed: `drawBattery` no longer adds `(fontHeight - rect.height)/2` when caller already passed centered rect. Now `y = rect.y` directly. `batteryYOffset` and `fontHeight` removed from `drawBattery`.
- Header `drawHeader` non-Home path now computes shared center from title:
  ```
  titleTop = textTop(UI_12, HeaderTitleBaseline + dy) // 14
  statusCenter = titleTop + ui12H/2 // 26
  battY = statusCenter - HeaderBatteryH/2 // 20
  wifiY = statusCenter - HeaderIcon/2 // 15
  clockY = statusCenter - smallH/2 // 18
  percentY = statusCenter - smallH/2 // 18
  ```
  All share `statusCenter` inside 46px band; title left shares same center so row is one line. No `rect.y`/`SafeTop` drop.

### 3) Horizontal layout — right-packed
File: `firmware/src/util/HomeRef.h:25`
- Before: `TimeX=314 WifiX=366 BatteryTextX=400 BatteryX=438` right inset 13px, gaps 14/12/10 sparse
- After: `TimeX=314 WifiX=364 BatteryTextX=398 BatteryX=431` right inset 20px (18–27 range, PagePad-like)
- Order: clock (314) → wifi (364) → battery (431, far right), battery at far right per spec
- Gaps: clock→wifi 12px (364-(314+38)=12), wifi→text 12px (398-386=12), text→battery 5px immediate left (431-(398+28)=5) without colliding wifi
- Percent when shown: drawn at `batteryX -4 - textWidth` immediately left of battery, not fixed `HeaderBatteryTextX` (constant kept for packing reference but code computes dynamic). Title max width `HeaderTimeX - HeaderTitleX -8 =248` stops before clock. Divider 47 and TitleX 58 unchanged.

## Verification

```bash
/opt/anaconda3/bin/pytest firmware/tests/test_round10_applist_routes_header.py -q
# 6 passed
```

- Host test `test_non_home_header_uses_balanced_geometry` asserts `HeaderSafeTop=0 HeaderH=46 HeaderTitleBaseline=38 HeaderY=0`
- New `test_wifi_glyph_is_right_half_only` asserts `drawArc` right half kept, left half deleted, no bitmap swap
- New `test_header_status_cluster_is_right_packed` asserts right inset 18–27, order clock<wifi<battery, even gaps 10–14, title left of divider, title max width before clock, `batteryYOffset` absent, `statusCenter` present

No `pio run`, QEMU, m4sim, flash, or push executed per task. Unrelated dirty files `M4ProviderCoverCache.*` were stashed before merge and not committed.

## Files changed (allowed only)
- `firmware/src/util/HomeRef.h`
- `firmware/src/components/themes/fengyan/FengyanTheme.cpp`
- `firmware/tests/test_round10_applist_routes_header.py`
- `docs/orchestration/rounds/round-12-muse-status-cluster.md` (new)
