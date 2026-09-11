#!/usr/bin/env python3
"""Task 1 (INV-R1) RED — guarded-emission source contract (no behavior change).

Pins the Phase 1 guard discipline before it exists:
  (a) firmware/src/util/M4RenderGuard.h — move-forbidden RAII guard with
      explicit unlock + static peek + bounded-wait acquire (no portMAX_DELAY
      in code on new paths);
  (b) firmware/src/main.cpp owns exactly one file-scope process-wide
      semaphore, created there and consumed as an M4RenderGuard constructor
      argument by the AppList submit path (structural linkage, no name style);
  (c) EVERY render-consumed AppList main-thread write site is covered by a
      lexical lock scope or a guarded setter: selectIndex/moveSelection plus
      each mode_ write, each uninstallClearData_ toggle, and each
      updateRequired_ write — with all 7 setMask policy calls preserved.
      moveSelection may delegate to a guarded selectIndex without taking the
      lock itself; taking a non-recursive lock and then calling a guarded
      selectIndex (double-take) is rejected;
  (d) displayTaskLoop is two-phase: snapshot-under-local, release, then the
      single render() call runs inside a live global-guard scope that
      surrounds the actual submit path, with updateRequired_/subActivity
      rechecked under local;
  (e) openSelected/openInstall hold no outstanding local lock at
      enterNewActivity — explicit give/unlock or end of an RAII guard scope;
  (f) AppList submit guard proven on the path (see (d)); Home/MyLibrary
      submit sites are existence anchors only — their guards belong to
      Task 8 and must not gate Task 2 GREEN.

RED gate: exits non-zero because the guard header, the all-sites locking,
the two-phase pattern, and the release-before-enter do not exist yet.
GREEN scope: Task 2 (AppList only). Home/MyLibrary guards arrive in Task 8.
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
HOME_CPP = SRC / "activities" / "home" / "HomeActivity.cpp"
MYLIB_CPP = SRC / "activities" / "home" / "MyLibraryActivity.cpp"

failures = []


def check(name, cond, detail=""):
    print(("PASS" if cond else "FAIL") + ": " + name + ("" if cond else " -- " + detail))
    if not cond:
        failures.append(name)


def read(path):
    return path.read_text(encoding="utf-8")


def mask_text(text):
    """Blank comments and string/char literals, preserving offsets/newlines."""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        elif text[i] in "\"'":
            q = text[i]
            j = i + 1
            while j < n and text[j] != q and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        else:
            i += 1
    return "".join(out)


def func_span(text, signature):
    """Return (body, body_offset) for the brace-balanced body, else (None, -1)."""
    start = text.find(signature)
    if start < 0:
        return None, -1
    brace = text.find("{", start)
    if brace < 0:
        return None, -1
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[brace : i + 1], brace
    return None, -1


def block_end(body, pos):
    """Offset of the closing brace of the block enclosing pos."""
    depth = body.count("{", 0, pos) - body.count("}", 0, pos)
    cur = depth
    for i in range(pos, len(body)):
        if body[i] == "{":
            cur += 1
        elif body[i] == "}":
            cur -= 1
            if cur < depth:
                return i
    return len(body)


LOCAL_TAKE = r"xSemaphoreTake\s*\(\s*renderingMutex_"
LOCAL_GIVE = r"xSemaphoreGive\s*\(\s*renderingMutex_"
GUARD_DECL = r"M4RenderGuard\s+(\w+)\s*[\(\{]\s*(\w+)"
GUARDED_SETTER = re.compile(r"\b\w*(Guarded|Locked|UnderLock|WithLock)\w*\s*\(")


def locked_spans(body):
    """(take_pos, release_pos) spans covering the local mutex hold regions."""
    spans = []
    for m in re.finditer(LOCAL_TAKE, body):
        g = re.search(LOCAL_GIVE, body[m.end() :])
        end = m.end() + g.end() if g else len(body)
        spans.append((m.start(), end))
    for m in re.finditer(GUARD_DECL, body):
        spans.append((m.start(), block_end(body, m.start())))
    return spans


def guard_scopes(body):
    """(decl_pos, scope_end, var, mutex_arg) for each named M4RenderGuard."""
    return [
        (m.start(), block_end(body, m.start()), m.group(1), m.group(2))
        for m in re.finditer(GUARD_DECL, body)
    ]


def write_sites(body, offset, full, members):
    """(line, stmt) per plain-`=` write to a listed member."""
    pat = re.compile(r"\b(" + "|".join(members) + r")\s*(?<![=!<>])=(?![=>])")
    sites = []
    for m in pat.finditer(body):
        line = full.count("\n", 0, offset + m.start()) + 1
        stmt_l = max(body.rfind(";", 0, m.start()), body.rfind("{", 0, m.start()), body.rfind("}", 0, m.start()))
        stmt = body[stmt_l + 1 : body.find(";", m.start()) + 1]
        sites.append((line, stmt, m.start()))
    return sites


def sites_covered(body, sites):
    spans = locked_spans(body)
    return [
        line
        for line, stmt, pos in sites
        if not any(s <= pos < e for s, e in spans)
        and GUARDED_SETTER.search(stmt) is None
    ]


# --- (a) M4RenderGuard.h existence + shape -----------------------------------
check("a-guard-header-exists", GUARD_H.is_file(), "firmware/src/util/M4RenderGuard.h absent")
guard = mask_text(read(GUARD_H)) if GUARD_H.is_file() else ""
check("a-guard-class-final", "class M4RenderGuard final" in guard, "missing final RAII class")
check(
    "a-guard-copy-deleted",
    "M4RenderGuard(const M4RenderGuard&) = delete" in guard
    and re.search(r"M4RenderGuard\s*&\s*operator\s*=\s*\(\s*const\s+M4RenderGuard\s*&", guard) is not None,
    "copy construction/assignment must be deleted",
)
check(
    "a-guard-move-deleted",
    "M4RenderGuard(M4RenderGuard&&) = delete" in guard
    and re.search(r"M4RenderGuard\s*&\s*operator\s*=\s*\(\s*M4RenderGuard\s*&&", guard) is not None,
    "move construction/assignment must be deleted",
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
    "SemaphoreHandle_t" in guard and "TickType_t" in guard,
    "acquire must take a bounded TickType_t wait, not portMAX_DELAY",
)
check("a-guard-no-infinite-wait", "portMAX_DELAY" not in guard, "new guard code must not use portMAX_DELAY")

# --- (b) one process-wide semaphore, structurally linked to the guard --------
main = mask_text(read(MAIN_CPP))
applist_raw = read(APPLIST_CPP)
applist = mask_text(applist_raw)
main_globals = re.findall(r"(?m)^(?:static\s+)?SemaphoreHandle_t\s+(\w+)", main)
check(
    "b-main-single-global-mutex",
    len(main_globals) == 1,
    "need exactly one file-scope SemaphoreHandle_t in main.cpp, found %d" % len(main_globals),
)
check("b-main-mutex-created", "xSemaphoreCreateMutex" in main, "process-wide mutex is never created in main.cpp")
global_id = main_globals[0] if len(main_globals) == 1 else None
check(
    "b-main-linked-to-guard",
    global_id is not None
    and re.search(r"M4RenderGuard\s+\w+\s*[\(\{]\s*" + re.escape(global_id) + r"\b", applist) is not None,
    "no M4RenderGuard in AppListActivity.cpp is constructed from the main.cpp mutex",
)
check(
    "b-applist-includes-guard",
    "M4RenderGuard.h" in applist,
    "AppListActivity.cpp does not include M4RenderGuard.h",
)

# --- (c) per-site write coverage ----------------------------------------------
WRITE_FUNCS = {
    "selectIndex": ("void AppListActivity::selectIndex(", ["selectedIndex_", "updateRequired_"]),
    "loop": ("void AppListActivity::loop()", ["mode_", "uninstallClearData_", "updateRequired_"]),
    "uninstallSelected": ("void AppListActivity::uninstallSelected()", ["updateRequired_"]),
    "displayTaskLoop": ("void AppListActivity::displayTaskLoop()", ["updateRequired_"]),
    "reload": ("void AppListActivity::reload()", ["selectedIndex_", "mode_"]),
}
bodies = {}
for label, (sig, members) in WRITE_FUNCS.items():
    body, off = func_span(applist, sig)
    bodies[label] = (body, off)
    if body is None:
        check("c-%s-sites-covered" % label, False, "function missing")
        continue
    uncovered = sites_covered(body, write_sites(body, off, applist, members))
    check(
        "c-%s-sites-covered" % label,
        not uncovered,
        "unguarded %s writes at line(s) %s" % (label, sorted(set(uncovered))),
    )

# onEnter is pre-task: its updateRequired_ write must precede xTaskCreate.
enter_body, _ = func_span(applist, "void AppListActivity::onEnter()")
check(
    "c-onEnter-pretask-order",
    enter_body is not None
    and "updateRequired_" in enter_body
    and "xTaskCreate" in enter_body
    and enter_body.find("updateRequired_") < enter_body.find("xTaskCreate"),
    "onEnter must write updateRequired_ before the display task exists",
)

# moveSelection: delegation to a guarded selectIndex needs no own lock (I2).
select_guarded = bodies["selectIndex"][0] is not None and bool(locked_spans(bodies["selectIndex"][0]))
move_body, move_off = func_span(applist, "void AppListActivity::moveSelection(")
if move_body is None:
    check("c-moveSelection-delegates-or-locked", False, "function missing")
    check("c-moveSelection-no-double-take", False, "function missing")
else:
    move_takes = re.search(LOCAL_TAKE, move_body) is not None or bool(
        re.search(r"M4RenderGuard\s+\w+\s*[\(\{]", move_body)
    )
    move_calls_select = "selectIndex(" in move_body
    uncovered_move = sites_covered(move_body, write_sites(move_body, move_off, applist, ["selectedIndex_", "updateRequired_"]))
    check(
        "c-moveSelection-no-double-take",
        not (move_takes and move_calls_select and select_guarded),
        "non-recursive lock taken then guarded selectIndex re-takes it",
    )
    check(
        "c-moveSelection-delegates-or-locked",
        (move_calls_select and select_guarded and not move_takes)
        or (move_takes and not (move_calls_select and select_guarded) and not uncovered_move),
        "moveSelection must delegate to guarded selectIndex or lock its own writes",
    )

check(
    "c-setMask-preserved",
    applist.count("M4FooterTouchPolicy::setMask") == 7,
    "all 7 setMask policy calls must stay at their sites, found %d"
    % applist.count("M4FooterTouchPolicy::setMask"),
)

# --- (d) two-phase displayTaskLoop + guarded submit path -----------------------
display_body, _ = func_span(applist, "void AppListActivity::displayTaskLoop()")
check("d-loop-exists", display_body is not None, "displayTaskLoop missing")
if display_body is None:
    display_body = ""
check(
    "d-recheck-under-local",
    "updateRequired_" in display_body and "subActivity" in display_body,
    "must recheck updateRequired_ && !subActivity",
)
render_calls = [m.start() for m in re.finditer(r"(?<!:)\brender\s*\(\s*\)", display_body)]
takes = [m.start() for m in re.finditer(LOCAL_TAKE, display_body)]
gives = [m.start() for m in re.finditer(LOCAL_GIVE, display_body)]
check(
    "d-release-before-render",
    len(render_calls) == 1
    and any(t < g < render_calls[0] for t in takes for g in gives),
    "local mutex still held across render/submit (single-phase loop)",
)
check(
    "d-global-guard-on-submit-path",
    global_id is not None
    and len(render_calls) == 1
    and any(
        start < render_calls[0] < end
        and arg == global_id
        and re.search(r"\b" + re.escape(var) + r"\s*\.\s*unlock", display_body[start:render_calls[0]]) is None
        for start, end, var, arg in guard_scopes(display_body)
    ),
    "no live M4RenderGuard from the process-wide mutex surrounds the render() submit call",
)
render_callers = len(re.findall(r"(?<!:)\brender\s*\(\s*\)", applist))
check("d-single-render-caller", render_callers == 1, "render() must be called only from displayTaskLoop")
check(
    "d-snapshot-under-local",
    "Snapshot" in display_body or "snapshot" in display_body,
    "no dirty-frame snapshot taken under local before release",
)

# --- (e) release-before-enter (RAII-aware) -------------------------------------
for label, sig in (
    ("e-openSelected", "void AppListActivity::openSelected()"),
    ("e-openInstall", "void AppListActivity::openInstall()"),
):
    body, _ = func_span(applist, sig)
    if body is None:
        check(label + "-release-before-enter", False, "function missing")
        continue
    enters = [m.start() for m in re.finditer(r"enterNewActivity\s*\(", body)]
    acqs = [m.start() for m in re.finditer(LOCAL_TAKE, body)]
    acqs += [m.start() for m in re.finditer(r"M4RenderGuard\s+\w+\s*[\(\{]", body)]
    rels = [m.start() for m in re.finditer(LOCAL_GIVE, body)]
    rels += [m.start() for m in re.finditer(r"\.\s*unlock\s*\(\s*\)", body)]
    rels += [block_end(body, m.start()) for m in re.finditer(r"M4RenderGuard\s+\w+\s*[\(\{]", body)]
    first_enter = min(enters) if enters else -1
    check(
        label + "-release-before-enter",
        bool(enters)
        and any(a < first_enter for a in acqs)
        and all(any(a < r < first_enter for r in rels) for a in acqs if a < first_enter),
        "enterNewActivity runs with the local mutex outstanding (or the copy is unprotected)",
    )

# --- (f) Home/MyLibrary submit-site existence anchors (Task 8 owns guards) ----
for label, path in (("f-home-submits", HOME_CPP), ("f-mylib-submits", MYLIB_CPP)):
    check(label + "-sites-exist", "renderer.displayBuffer(" in read(path), "submit sites moved/renamed")

print()
if failures:
    print("RED: %d failing check(s) — Phase 1 guard discipline absent as intended:" % len(failures))
    for name in failures:
        print("  - " + name)
    sys.exit(1)
print("GREEN: guarded-emission contract holds.")
