"""Track D contracts for plugin ownership/back routing and WeRead search hiding."""

from __future__ import annotations

import unittest
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def source(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def function_body(src: str, signature: str) -> str:
    start = src.index(signature)
    brace = src.index("{", start)
    depth = 0
    for index in range(brace, len(src)):
        if src[index] == "{":
            depth += 1
        elif src[index] == "}":
            depth -= 1
            if depth == 0:
                return src[brace + 1 : index]
    raise AssertionError(f"unterminated function: {signature}")


class TrackDNavigationContracts(unittest.TestCase):
    def test_app_list_keeps_plugin_as_deferred_child_and_refreshes_once(self) -> None:
        path = "firmware/src/activities/apps/AppListActivity.cpp"
        src = source(path)
        open_body = function_body(src, "void AppListActivity::openSelected()")
        loop_body = function_body(src, "void AppListActivity::loop()")

        self.assertNotIn("exitActivity();", src)
        self.assertIn("enterNewActivity(new NativeAppActivity", open_body)
        self.assertIn("enterNewActivity(new AppRuntimeActivity", open_body)
        self.assertNotIn("exitActivity();", open_body)
        self.assertIn("requestExitSubActivity();", open_body)

        self.assertIn("pumpSubActivityFrame()", loop_body)
        self.assertNotIn("subActivity->loop()", loop_body)
        self.assertIn("reload();", loop_body)
        self.assertIn("updateRequired_ = true;", loop_body)

    def test_deeper_native_provider_children_still_close_inside_their_parent(self) -> None:
        src = source("firmware/src/activities/apps/NativeAppActivity.cpp")
        loop_body = function_body(src, "void NativeAppActivity::loop()")
        action_body = function_body(src, "void NativeAppActivity::handleAction(")

        self.assertIn("pumpSubActivityFrame()", loop_body)
        self.assertNotIn("subActivity->loop()", loop_body)
        self.assertIn("requestExitSubActivity();", action_body)
        self.assertIn("M4NativeUi::ActionKind::Close", action_body)

    def test_repeated_home_back_has_one_parent_transition_per_event(self) -> None:
        # Host model for the ownership contract: the callback only marks a close;
        # the owner applies it after the child frame returns.
        class Host:
            def __init__(self) -> None:
                self.child = None
                self.close_pending = False
                self.transitions = 0
                self.refreshes = 0

            def open(self) -> None:
                self.child = "NativeApp"

            def child_back(self) -> None:
                self.close_pending = True

            def pump(self) -> None:
                if self.close_pending:
                    self.close_pending = False
                    self.child = None
                    self.transitions += 1
                    self.refreshes += 1

        host = Host()
        for _ in range(5):
            host.open()
            self.assertEqual(host.child, "NativeApp")
            host.child_back()
            host.pump()
            self.assertIsNone(host.child)
            self.assertFalse(host.close_pending)
        self.assertEqual(host.transitions, 5)
        self.assertEqual(host.refreshes, 5)

    def test_weread_search_api_and_screen_remain_but_home_entry_is_hidden(self) -> None:
        xml_path = ROOT / "plugins/m4-weread-plugin/main.xml"
        root = ET.parse(xml_path).getroot()
        home = root.find("./screen[@id='home']")
        search = root.find("./screen[@id='search']")
        self.assertIsNotNone(home)
        self.assertIsNotNone(search)
        home_buttons = home.find("./buttons")
        self.assertIsNotNone(home_buttons)
        self.assertNotIn("找书", ET.tostring(home, encoding="unicode"))
        self.assertNotIn("left", home_buttons.attrib)
        self.assertNotIn("onLeft", home_buttons.attrib)
        self.assertIn("找书", ET.tostring(search, encoding="unicode"))
        self.assertEqual(search.attrib.get("id"), "search")

    def test_system_back_and_close_reach_the_app_list_owner_callback(self) -> None:
        factory = source("firmware/src/apps/native/M4NativeAppControllerFactory.cpp")
        native = source("firmware/src/activities/apps/NativeAppActivity.cpp")
        self.assertIn('action == "system.back" || action == "system.close"', factory)
        self.assertIn("case M4NativeUi::ActionKind::Close:", native)
        self.assertIn("onExitApp_();", native)


if __name__ == "__main__":
    unittest.main()
