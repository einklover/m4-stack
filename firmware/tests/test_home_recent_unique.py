from pathlib import Path

HOME = Path(__file__).resolve().parents[1] / "src/activities/home/HomeActivity.cpp"
SRC = HOME.read_text(encoding="utf-8")


def test_mini_recents_skip_hero_book():
    assert "if (i == 0) continue" in SRC
    assert "for (size_t i = 1; i < ctx.recentBooks.size() && itemIndex < 3" in SRC
    assert "snapshot.currentPath" in SRC
    assert "snapshot.recent[0]" not in SRC.split("kActionOpenCurrentBook", 1)[1][:400]


def test_theme_title_slots_are_two_lines():
    theme = Path(__file__).resolve().parents[2] / "themes/murphy-default/theme.json"
    text = theme.read_text(encoding="utf-8")
    assert '"text": "$current.title"' in text
    assert '"text": "$item.title"' in text
