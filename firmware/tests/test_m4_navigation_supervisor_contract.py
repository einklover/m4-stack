#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

NAV_FILES = [
    ROOT / "firmware/src/navigation/M4NavigationSupervisor.h",
    ROOT / "firmware/src/navigation/M4NavigationSupervisor.cpp",
]
ACTIVITY_CPP = ROOT / "firmware/src/activities/network/CrossPointWebServerActivity.cpp"

missing = [str(path.relative_to(ROOT)) for path in [*NAV_FILES, ACTIVITY_CPP] if not path.exists()]
if missing:
    raise SystemExit("P1D navigation supervisor RED contract: implementation missing:\n  " + "\n  ".join(missing))

source = "\n".join(path.read_text(encoding="utf-8") for path in NAV_FILES)
activity_source = ACTIVITY_CPP.read_text(encoding="utf-8")

required = [
    "detach",
    "idempot",
    "callback",
]

missing_markers = [marker for marker in required if marker not in source.lower()]
if missing_markers:
    raise SystemExit(
        "P1D navigation supervisor RED contract missing detach safety markers:\n  "
        + "\n  ".join(missing_markers)
    )

activity_markers = [
    "navigationSupervisor.attach();",
    "navigationSupervisor.detach();",
    "navigationSupervisor.canDispatchCallback()",
    "requestNavigationExit",
    "xTaskNotifyGive",
]
missing_activity_markers = [marker for marker in activity_markers if marker not in activity_source]
if missing_activity_markers:
    raise SystemExit(
        "P1D activity integration contract missing non-blocking navigation wiring:\n  "
        + "\n  ".join(missing_activity_markers)
    )


def function_body(signature: str) -> str:
    start = activity_source.find(signature)
    if start < 0:
        raise SystemExit(f"P1D activity integration contract missing function: {signature}")
    brace = activity_source.find("{", start)
    if brace < 0:
        raise SystemExit(f"P1D activity integration contract missing body: {signature}")
    depth = 0
    for index in range(brace, len(activity_source)):
        char = activity_source[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return activity_source[brace : index + 1]
    raise SystemExit(f"P1D activity integration contract unterminated body: {signature}")


exit_body = function_body("void CrossPointWebServerActivity::onExit()")
if "portMAX_DELAY" in exit_body:
    raise SystemExit("P1D activity integration contract: onExit must not wait forever for the render mutex")
if "fileTransferService.stop(" in exit_body or "fileTransferService->stop(" in exit_body:
    raise SystemExit("P1D activity integration contract: onExit must not synchronously stop file-transfer networking")

navigation_body = function_body("void CrossPointWebServerActivity::requestNavigationExit()")
detach_pos = navigation_body.find("navigationSupervisor.detach();")
callback_pos = navigation_body.find("onGoBack();")
if detach_pos < 0 or callback_pos < 0 or detach_pos > callback_pos:
    raise SystemExit("P1D activity integration contract: navigation must detach before dispatching Back/Home")

print("m4 navigation supervisor contract: PASS")
