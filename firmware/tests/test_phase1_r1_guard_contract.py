#!/usr/bin/env python3
"""Task 1 (INV-R1) RED — guarded-emission source contract (no behavior change).

Pins the Phase 1 guard discipline before it exists:
  (a) firmware/src/util/M4RenderGuard.h — move-forbidden RAII guard with
      explicit unlock + static peek + bounded-wait acquire (no portMAX_DELAY
      on new paths);
  (b) firmware/src/main.cpp owns one process-wide render mutex alongside
      currentActivity;
  (c) ALL render-consumed AppList main-thread writes take the local mutex
      (or guarded setters): selectIndex/moveSelection plus mode_ writes,
      uninstallClearData_ toggles, and updateRequired_ writes — with the
      setMask policy calls preserved at each site;
  (d) displayTaskLoop is two-phase: snapshot-under-local, release, then
      render-plus-submit-under-global, with updateRequired_/subActivity
      rechecked under local;
  (e) openSelected/openInstall release the local mutex before
      enterNewActivity (never hold local across child installation);
  (f) per-activity submit-site guard lists for AppList/Home/MyLibrary.

RED gate: exits non-zero because the guard header, the all-writes locking,
the two-phase pattern, and the release-before-enter do not exist yet.
Run: python3 firmware/tests/test_phase1_r1_guard_contract.py
"""

from pathlib import Path
import re
import sys

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "firmware" / "src"
GUARD_H = SRC / "util" / "M4RenderGuard.h"
MAIN_CPP = SRC / "main.cpp"
APPLIST_CPP = SRC / "activities" / "apps" / "AppListActivity.cpp"
APPLIST_H = SRC / "activities" / "apps" / "AppListActivity.h"
HOME_CPP = SRC / "activities" / "home" / "HomeActivity.cpp"
MYLIB_CPP = SRC / "activities" / "home" / "MyLibraryActivity.cpp"

failures = []


def check(name, cond, detail=""):
    print(("PASS" if cond else "FAIL") + ": " + name + ("" if cond else " -- " + detail))
    if not cond:
        failures.append(name)


def read(path):
    return path.read_text(encoding="utf-8")


def func_body(text, signature):
    """Return the brace-balanced body of the first function matching signature."""
    start = text.find(signature)
    if start < 0:
        return None
    brace = text.find("{", start)
    if brace < 0:
        return None
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[brace : i + 1]
    return None


def takes_local_lock(body):
    return body is not None and (
        "xSemaphoreTake(renderingMutex_" in body
        or "M4RenderGuard" in body
        or "withRenderLock" in body
        or "setSelectedIndexGuarded" in body
    )


# --- (a) M4RenderGuard.h existence + shape -----------------------------------
check("a-guard-header-exists", GUARD_H.is_file(), "firmware/src/util/M4RenderGuard.h absent")
guard = read(GUARD_H) if GUARD_H.is_file() else ""
check("a-guard-class-final", "class M4RenderGuard final" in guard, "missing final RAII class")
check(
    "a-guard-move-forbidden",
    "M4RenderGuard(const M4RenderGuard&) = delete" in guard
    and "M4RenderGuard(M4RenderGuard&&) = delete" in guard,
    "copy/move must be deleted",
)
check("a-guard-unlock", re.search(r"void\s+unlock\s*\(\s*\)", guard) is not None, "missing explicit unlock()")
check(
    "a-guard-peek",
    re.search(r"static\s+bool\s+peek\s*\(", guard) is not None,
    "missing static peek()",
)
check("a-guard-owns", re.search(r"bool\s+owns\s*\(\s*\)", guard) is not None, "missing owns()")
check(
    "a-guard-bounded-wait",
    "SemaphoreHandle_t" in guard and ("TickType_t" in guard or "pdMS_TO_TICKS" in guard),
    "acquire must take a bounded wait, not portMAX_DELAY",
)
check("a-guard-no-infinite-wait", "portMAX_DELAY" not in guard, "new guard paths must not use portMAX_DELAY")

# --- (b) process-wide mutex in main.cpp --------------------------------------
main = read(MAIN_CPP)
check(
    "b-main-global-mutex",
    re.search(r"SemaphoreHandle_t\s+\w*[Rr]ender\w*", main) is not None
    or re.search(r"SemaphoreHandle_t\s+g_\w*[Mm]utex\w*", main) is not None,
    "no process-wide render mutex in main.cpp",
)
check(
    "b-main-mutex-alongside-current-activity",
    "currentActivity" in main
    and re.search(r"SemaphoreHandle_t\s+\w+", main) is not None
    and abs(main.find("currentActivity") - main.find("SemaphoreHandle_t")) < 2000,
    "global mutex must sit alongside currentActivity ownership",
)
check("b-main-includes-guard", "M4RenderGuard.h" in main, "main.cpp does not include M4RenderGuard.h")

# --- (c) all render-consumed main-thread writes locked ------------------------
applist = read(APPLIST_CPP)
select_body = func_body(applist, "void AppListActivity::selectIndex(")
move_body = func_body(applist, "void AppListActivity::moveSelection(")
loop_body = func_body(applist, "void AppListActivity::loop()")
uninstall_body = func_body(applist, "void AppListActivity::uninstallSelected()")
check("c-selectIndex-locked", takes_local_lock(select_body), "selectIndex writes selectedIndex_/updateRequired_ unlocked")
check("c-moveSelection-locked", takes_local_lock(move_body), "moveSelection path unlocked")
check("c-loop-writes-locked", takes_local_lock(loop_body), "loop() mode_/uninstallClearData_/updateRequired_ writes unlocked")
check(
    "c-uninstallSelected-locked",
    takes_local_lock(uninstall_body),
    "uninstallSelected updateRequired_ write unlocked",
)
check(
    "c-mode-writes-covered",
    loop_body is not None and "mode_ =" in loop_body and takes_local_lock(loop_body),
    "mode_ write sites (:367,:388,:425,:436,:464) not under local mutex",
)
check(
    "c-clear-data-toggles-covered",
    loop_body is not None and "uninstallClearData_ =" in loop_body and takes_local_lock(loop_body),
    "uninstallClearData_ toggles (:395,:406) not under local mutex",
)
check(
    "c-updateRequired-writes-covered",
    all(
        takes_local_lock(b)
        for b in (select_body, loop_body, uninstall_body)
        if b is not None and "updateRequired_" in b
    ),
    "updateRequired_ write sites not all under local mutex",
)
check(
    "c-setMask-preserved",
    applist.count("M4FooterTouchPolicy::setMask") >= 6,
    "setMask policy calls must stay at each write site",
)

# --- (d) two-phase displayTaskLoop --------------------------------------------
display_body = func_body(applist, "void AppListActivity::displayTaskLoop()")
check("d-loop-exists", display_body is not None, "displayTaskLoop missing")
if display_body is None:
    display_body = ""
check(
    "d-recheck-under-local",
    "updateRequired_" in display_body and "subActivity" in display_body,
    "must recheck updateRequired_ && !subActivity",
)
render_pos = display_body.find("render()")
give_pos = display_body.find("xSemaphoreGive(renderingMutex_")
take_pos = display_body.find("xSemaphoreTake(renderingMutex_")
check(
    "d-release-before-render",
    take_pos >= 0 and give_pos > take_pos and 0 < render_pos and give_pos < render_pos,
    "single-phase loop still holds local mutex across render/submit",
)
check(
    "d-global-guard-on-submit",
    "M4RenderGuard" in display_body or re.search(r"xSemaphoreTake\(\s*g\w*Render", display_body) is not None,
    "render-plus-submit not under the global guard",
)
check(
    "d-snapshot-under-local",
    "Snapshot" in display_body or "snapshot" in display_body,
    "no dirty-frame snapshot taken under local before release",
)

# --- (e) release-before-enter ---------------------------------------------------
open_sel = func_body(applist, "void AppListActivity::openSelected()")
open_inst = func_body(applist, "void AppListActivity::openInstall()")
for label, body in (("e-openSelected", open_sel), ("e-openInstall", open_inst)):
    if body is None:
        check(label + "-release-before-enter", False, "function missing")
        continue
    enter_pos = body.find("enterNewActivity")
    give_pos = max(body.rfind("xSemaphoreGive(renderingMutex_"), body.rfind("unlock()"))
    check(
        label + "-release-before-enter",
        enter_pos >= 0 and 0 <= give_pos < enter_pos,
        "enterNewActivity still runs while holding the local mutex",
    )

# --- (f) per-activity submit-site guard lists -----------------------------------
for label, path in (
    ("f-applist-submits", APPLIST_CPP),
    ("f-home-submits", HOME_CPP),
    ("f-mylib-submits", MYLIB_CPP),
):
    text = read(path)
    check(label + "-sites-exist", "renderer.displayBuffer(" in text, "no submit sites found")
    check(label + "-guarded", "M4RenderGuard" in text, "submit sites lack the global guard")

print()
if failures:
    print("RED: %d failing check(s) — Phase 1 guard discipline absent as intended:" % len(failures))
    for name in failures:
        print("  - " + name)
    sys.exit(1)
print("GREEN: guarded-emission contract holds.")
