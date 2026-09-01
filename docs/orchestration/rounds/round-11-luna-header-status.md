# Round 11 — Luna non-Home header/status

## Final geometry

- `HomeRef::HeaderSafeTop = 20`
- `HomeRef::HeaderH = 46`
- `HomeRef::HeaderTitleBaseline = 38`
- `FengyanMetrics::values.topPadding = HomeRef::HeaderSafeTop`
- `FengyanMetrics::values.headerHeight = HomeRef::HeaderH`

The Home-only `HomeHeader*` constants and the `title == "我的mofei"` drawing
branch are unchanged.

## Rationale

For non-Home pages, `rect.y` is `HeaderSafeTop`, so `dy = rect.y - HeaderY`
translates the entire header text/status cluster. Status icons instead use
`rect.y + (HeaderH - HeaderIcon) / 2`; title ink uses
`HeaderTitleBaseline + dy` before `textTop()` conversion.

The interim `28/38/52` experiment improved the relative alignment, but it
also moved the status glyphs about 11 px down: the +8 px `SafeTop` shift plus
the +3 px icon-centering shift from the taller header. Device evidence changed
from title/status first ink around `25/35` to `40/46`. That is why raising
`SafeTop` alone is the wrong alignment control.

Keeping `SafeTop=20` and `HeaderH=46` restores the status cluster to its prior
vertical band while retaining 20 px of top air. Raising only the non-Home
baseline to 38 moves the title down independently; the expected first-ink
positions are approximately title `y=32` and status `y=35` (Δ≈3 px).

## Verification

- `python3 firmware/tests/test_m4_dependency_bootstrap_contract.py` — PASS
- `python3 firmware/tests/test_round10_applist_routes_header.py` — PASS
- `python3 -m pytest -q firmware/tests/test_round10_applist_routes_header.py` — `4 passed`
- `git diff --check` — PASS
- No full PlatformIO build, QEMU journey, device flash, or push was run, as
  required by the task.

## Residual risks

- This lane has no fresh hardware capture for the final constants; the
  predicted ink positions are derived from the supplied interim/prior device
  measurements. Coordinator/device smoke should validate the final optical
  result before release.
- Production compilation was intentionally not run, so the final firmware
  compile remains a merge/freeze check.
