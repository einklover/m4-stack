#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
reader = (ROOT / "firmware/src/activities/reader/TxtReaderActivity.cpp").read_text(encoding="utf-8")


def body(signature: str) -> str:
    start = reader.find(signature)
    assert start >= 0, f"missing function: {signature}"
    brace = reader.find("{", start)
    assert brace >= 0, f"missing body: {signature}"
    depth = 0
    for index in range(brace, len(reader)):
        if reader[index] == "{":
            depth += 1
        elif reader[index] == "}":
            depth -= 1
            if depth == 0:
                return reader[brace : index + 1]
    raise AssertionError(f"unterminated body: {signature}")


open_menu = body("void TxtReaderActivity::openMenu(EpubReaderMenuActivity::MenuLayer layer)")
failed_lock = open_menu.find("if (!lockState(pdMS_TO_TICKS(400)))")
assert failed_lock >= 0, "Reader must refuse the menu if its render lock is still busy"
failure_path = open_menu[failed_lock : open_menu.find("}", failed_lock) + 1]
assert "suppressDisplay_ = false" in failure_path
assert "updateRequired = true" in failure_path
assert "return" in failure_path
assert failed_lock < open_menu.find("enterNewActivity(new EpubReaderMenuActivity")
assert failed_lock < open_menu.find("renderer.setRenderMode(GfxRenderer::BW)")

display_loop = body("void TxtReaderActivity::displayTaskLoop()")
idle_heartbeat = display_loop.find("esp_task_wdt_reset();")
idle_gate = display_loop.find("if (suppressDisplay_ || subActivity)")
assert idle_heartbeat >= 0 and idle_heartbeat < idle_gate, (
    "the registered Reader display task must feed TWDT before entering idle/menu waits"
)
for gate in (
    "if (suppressDisplay_ || subActivity) {\n            unlockState();\n            continue;",
    "if (suppressDisplay_ || subActivity) {\n          unlockState();\n          continue;",
):
    assert gate in display_loop, "Reader layout/index work must recheck suppression under its state lock"

print("Reader TTF reload quiescence contract: PASS")
