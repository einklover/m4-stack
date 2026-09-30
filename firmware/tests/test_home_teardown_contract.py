#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
home_cpp = (ROOT / "firmware/src/activities/home/HomeActivity.cpp").read_text(encoding="utf-8")
home_h = (ROOT / "firmware/src/activities/home/HomeActivity.h").read_text(encoding="utf-8")
main = (ROOT / "firmware/src/main.cpp").read_text(encoding="utf-8")


def body(source: str, signature: str) -> str:
    start = source.find(signature)
    assert start >= 0, f"missing function: {signature}"
    brace = source.find("{", start)
    assert brace >= 0, f"missing body: {signature}"
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace : index + 1]
    raise AssertionError(f"unterminated body: {signature}")


display = body(home_cpp, "void HomeActivity::displayTaskLoop(const std::shared_ptr<BackendContext>& ctx)")
assert "backendCtx" not in display
assert "displayStopRequested" in display
assert "render(ctx)" in display

on_exit = body(home_cpp, "void HomeActivity::onExit()")
assert "displayStopRequested.store(true" in on_exit
assert "kExitWaitMs" in on_exit and "deadline" in on_exit
assert "backendCtx.reset()" not in on_exit
assert "vTaskDelete" not in on_exit.split("#else", 1)[0]
legacy_exit = on_exit.split("#else", 1)[1]
assert "displayTaskExited.store(true" in legacy_exit

destructor = body(home_cpp, "HomeActivity::~HomeActivity()")
assert "backendCtx.reset()" in destructor
assert "displayTaskExited" in home_h and "readyForDestruction() const override" in home_h

reaper = body(main, "static void reapDeferredActivities()")
assert "readyForDestruction()" in reaper
reset = main.find("if (gM4PendingTransientReset && !hasDeferredActivities())")
assert reset >= 0
reset_body = body(main, "if (gM4PendingTransientReset && !hasDeferredActivities())")
assert "!m4HomeBoundaryWorkersBusy()" in reset_body

print("home bounded teardown/context ownership contract: PASS")
