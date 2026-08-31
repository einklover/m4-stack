import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
THEME = ROOT / "themes/murphy-default/theme.json"

class MurphyDefaultActionsContract(unittest.TestCase):
    def test_all_action_is_open_history(self):
        cfg = json.loads(THEME.read_text(encoding="utf-8"))
        nodes = cfg["nodes"]
        all_node = next((n for n in nodes if n.get("text") == "全部  >"), None)
        self.assertIsNotNone(all_node, "missing '全部  >' node")
        self.assertEqual(all_node.get("action"), "open_history", "'全部  >' must be open_history")

    def test_more_action_is_open_apps(self):
        cfg = json.loads(THEME.read_text(encoding="utf-8"))
        nodes = cfg["nodes"]
        more_node = next((n for n in nodes if n.get("text") == "更多  >"), None)
        self.assertIsNotNone(more_node, "missing '更多  >' node")
        self.assertEqual(more_node.get("action"), "open_apps", "'更多  >' must be open_apps")

    def test_each_action_once_and_not_swapped(self):
        cfg = json.loads(THEME.read_text(encoding="utf-8"))
        nodes = cfg["nodes"]
        history = [n for n in nodes if n.get("action") == "open_history"]
        apps = [n for n in nodes if n.get("action") == "open_apps"]
        self.assertEqual(len(history), 1, "open_history exactly once")
        self.assertEqual(len(apps), 1, "open_apps exactly once")
        self.assertEqual(history[0]["text"], "全部  >")
        self.assertEqual(apps[0]["text"], "更多  >")

if __name__ == "__main__":
    unittest.main()
