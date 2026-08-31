import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
THEME = ROOT / "themes" / "murphy-default" / "theme.json"

def _load():
    return json.loads(THEME.read_text(encoding="utf-8"))

def test_murphy_default_has_open_history_and_open_apps():
    cfg = _load()
    nodes = cfg["nodes"]
    # "全部  >" must exist and have action open_history
    all_node = next((n for n in nodes if n.get("type") == "text" and n.get("text") == "全部  >"), None)
    assert all_node is not None, "theme murphy-default missing '全部  >' text node"
    assert all_node.get("action") == "open_history", f"'全部  >' action must be open_history, got {all_node.get('action')}"

    more_node = next((n for n in nodes if n.get("type") == "text" and n.get("text") == "更多  >"), None)
    assert more_node is not None, "theme murphy-default missing '更多  >' text node"
    assert more_node.get("action") == "open_apps", f"'更多  >' action must be open_apps, got {more_node.get('action')}"

def test_murphy_default_actions_are_not_swapped_and_only_once():
    cfg = _load()
    nodes = cfg["nodes"]
    history_nodes = [n for n in nodes if n.get("action") == "open_history"]
    apps_nodes = [n for n in nodes if n.get("action") == "open_apps"]
    assert len(history_nodes) == 1, f"open_history must appear exactly once, got {len(history_nodes)}"
    assert len(apps_nodes) == 1, f"open_apps must appear exactly once, got {len(apps_nodes)}"
    # Ensure they are the correct text nodes, not swapped
    assert history_nodes[0]["text"] == "全部  >", "open_history must be on '全部  >', not elsewhere"
    assert apps_nodes[0]["text"] == "更多  >", "open_apps must be on '更多  >', not elsewhere"

def test_murphy_default_other_actions_intact():
    cfg = _load()
    nodes = cfg["nodes"]
    # Hero cover should still open current book, apps repeat should open_app
    cover_nodes = [n for n in nodes if n.get("type") == "cover" and n.get("binding") == "$current.cover"]
    assert len(cover_nodes) == 1
    assert cover_nodes[0].get("action") == "open_current_book"
    # Apps repeat icon action
    repeat_apps = next((n for n in nodes if n.get("type") == "repeat" and n.get("source") == "$apps"), None)
    assert repeat_apps is not None
    icon_child = next((c for c in repeat_apps["children"] if c.get("type") == "icon"), None)
    assert icon_child is not None
    assert icon_child.get("action") == "open_app"
    assert icon_child.get("action_arg") == "$item.id"
    # No extra unknown actions
    allowed = {"open_history", "open_apps", "open_current_book", "open_app"}
    for n in nodes:
        if "action" in n:
            assert n["action"] in allowed, f"unexpected action {n['action']} on {n}"
        if n.get("type") == "repeat":
            for c in n.get("children", []):
                if "action" in c:
                    assert c["action"] in allowed, f"unexpected child action {c['action']}"

def test_murphy_default_id_and_screen():
    cfg = _load()
    assert cfg["id"] == "murphy-default"
    assert cfg["screen"] == [480, 800]
    assert cfg["format"] == 1
