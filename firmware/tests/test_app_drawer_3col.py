from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
APP_LIST = ROOT / "src/activities/apps/AppListActivity.cpp"


def _source() -> str:
    return APP_LIST.read_text(encoding="utf-8")


def _constant(source: str, name: str) -> int:
    match = re.search(rf"constexpr\s+(?:int|size_t)\s+{name}\s*=\s*(\d+)", source)
    assert match, f"missing {name}"
    return int(match.group(1))


def _layout(item_count: int, selected: int, width: int = 480, height: int = 800):
    columns = 4
    tile_height = 118
    gap = 8
    top = 60  # Fengyan: topPadding + headerHeight + verticalSpacing.
    bottom = height - 51 - 14
    rows = max(1, (max(0, bottom - top) + gap) // (tile_height + gap))
    page_items = rows * columns
    page_start = (min(max(selected, 0), item_count - 1) // page_items) * page_items if item_count else 0
    grid_width = width - 2 * 4
    tile_width = max(1, (grid_width - (columns - 1) * 6) // columns)
    actual_width = columns * tile_width + (columns - 1) * 6
    start_x = 4

    def rect(index: int):
        if index < page_start or index >= page_start + page_items or index >= item_count:
            return None
        local = index - page_start
        return (
            start_x + (local % columns) * (tile_width + gap),
            top + (local // columns) * (tile_height + gap),
            tile_width,
            tile_height,
        )

    return rows, page_items, page_start, rect


def test_drawer_is_three_columns_with_shared_hit_geometry():
    source = _source()
    assert _constant(source, "kDrawerColumns") == 4
    assert "const int col = local % kDrawerColumns" in source
    assert "moveSelection(-kDrawerColumns)" in source
    assert "moveSelection(kDrawerColumns)" in source

    rows, page_items, page_start, rect = _layout(19, 17)
    assert rows == 5
    assert page_items == 20
    assert page_start == 0
    first_row = [rect(i) for i in (0, 1, 2, 3)]
    assert len({r[0] for r in first_row}) == 4
    assert len({r[1] for r in first_row}) == 1
    assert rect(4)[1] > first_row[0][1]
    assert rect(17) is not None


def test_drawer_retunes_tile_icon_slot_and_label_face_without_pixel_truncation():
    source = _source()
    assert _constant(source, "kDrawerMinTileHeight") > 100
    assert _constant(source, "kDrawerIconSlot") > 64
    assert "M4UiText::draw(renderer, UI_12_FONT_ID" in source
    assert "utf8EllipsizeChars(item.label.c_str(), kDrawerLabelMaxChars)" in source
    assert "truncated(renderer, UI_10_FONT_ID, item.label.c_str(), tile.width - 8)" not in source
