#!/usr/bin/env python3
"""Task 5 (INV-N1) RED — logical-navigation adoption source contract.

Pins the Phase 1 navigation unification before it exists:
  (a) MappedInputManager::Button appends NavNext/NavPrevious after PageForward
      (ordinals 0-8 unchanged, 11 enumerators total) with ordinal static_asserts
      against the dependency-free util/M4NavMapping.h helper;
  (b) wasPressed/wasReleased resolve NavNext/NavPrevious by recursion into
      synth-inclusive member getters placed BEFORE the synthetic-Back,
      synthetic-Key (member-expansion m4SynthSatisfiesKey), and footer-tap
      branches; isPressed stays level-OR with synthetic Keys inert to level;
      mapButton gains no logical case;
  (c) ButtonNavigator getNextButtons/getPreviousButtons return the logical
      singletons; index/page math untouched;
  (d) per-site adoption: AppList verticals via explicit-axis navigator
      overloads with Left/Right/Confirm/Back bindings unchanged (never folded
      into logical); MyLibrary linear rows via logical release with the
      preview/action-menu region unchanged; Home lists via logical press with
      Confirm/Back unchanged; Settings S1+S2 via logical release with
      Back/Confirm unchanged.

Companion behavioral gate: firmware/tests/native_app/test_phase1_n1_nav_mapping.cpp
(real host test over util/M4NavMapping.h; compile failure is its RED signal).

RED gate: exits non-zero because the helper header, the enum append, the
recursion wiring, the singleton getters, and all four site adoptions are absent
(activities still use hand-rolled Down/Right/Up/Left edge lists).
Run: python3 firmware/tests/test_phase1_n1_adoption_contract.py
"""

from pathlib import Path
import re
import sys

REPO = Path(__file__).resolve().parents[2]
MAPPED_H = REPO / "firmware" / "src" / "MappedInputManager.h"
MAPPED_CPP = REPO / "firmware" / "src" / "MappedInputManager.cpp"
NAV_HELPER = REPO / "firmware" / "src" / "util" / "M4NavMapping.h"
NAV_H = REPO / "firmware" / "src" / "util" / "ButtonNavigator.h"
NAV_CPP = REPO / "firmware" / "src" / "util" / "ButtonNavigator.cpp"
APPLIST_CPP = REPO / "firmware" / "src" / "activities" / "apps" / "AppListActivity.cpp"
MYLIB_CPP = REPO / "firmware" / "src" / "activities" / "home" / "MyLibraryActivity.cpp"
HOME_CPP = REPO / "firmware" / "src" / "activities" / "home" / "HomeActivity.cpp"
SETTINGS_CPP = REPO / "firmware" / "src" / "activities" / "settings" / "SettingsActivity.cpp"

failures = []


def check(name, cond, detail=""):
    print(("PASS" if cond else "FAIL") + ": " + name + ("" if cond else " -- " + detail))
    if not cond:
        failures.append(name)


def read(path):
    return path.read_text(encoding="utf-8") if path.is_file() else ""


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
    """Return the brace-balanced body for signature, else None."""
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


BTN = r"(?:MappedInputManager::\s*)?Button::\s*"
WSP = r"\s*"

mapped_h_raw = read(MAPPED_H)
mapped_cpp_raw = read(MAPPED_CPP)
mapped_h = mask_text(mapped_h_raw)
mapped_cpp = mask_text(mapped_cpp_raw)
helper_raw = read(NAV_HELPER)
nav_h = mask_text(read(NAV_H))
nav_cpp = mask_text(read(NAV_CPP))
applist = mask_text(read(APPLIST_CPP))
mylib = mask_text(read(MYLIB_CPP))
home = mask_text(read(HOME_CPP))
settings = mask_text(read(SETTINGS_CPP))

LEGACY_ORDER = ["Back", "Confirm", "Left", "Right", "Up", "Down", "Power", "PageBack", "PageForward"]
FULL_ORDER = LEGACY_ORDER + ["NavNext", "NavPrevious"]

# --- (helper) dependency-free mapping truth Task 6 production consumes -------
check("n1-helper-exists", NAV_HELPER.is_file(), "util/M4NavMapping.h absent (Task 6 creates it)")
helper = mask_text(helper_raw)
for label, pat in (
    ("n1-helper-enum", r"enum\s+class\s+M4NavButton[^{]*\{[^}]*NavNext[^}]*NavPrevious"),
    ("n1-helper-members", r"m4IsNavNextMember.*m4IsNavPreviousMember"),
    ("n1-helper-synth", r"m4SynthSatisfiesKey"),
    ("n1-helper-edge-folds", r"m4NavNextEdge.*m4NavPreviousEdge"),
    ("n1-helper-level-folds", r"m4NavNextLevel.*m4NavPreviousLevel"),
):
    check(label, re.search(pat, helper, re.DOTALL) is not None,
          "util/M4NavMapping.h missing: %s" % pat)

# --- (a) enum append after PageForward, ordinals 0-8 unchanged ---------------
enum_body = func_span(mapped_h, "enum class Button")
names = re.findall(r"[A-Za-z_]\w*", enum_body) if enum_body else []
check("n1-enum-ordinals-0-8-unchanged", names[:9] == LEGACY_ORDER,
      "legacy Button ordinals 0-8 changed: %s" % names[:9])
check("n1-enum-count-11", names == FULL_ORDER,
      "Button enum is not the appended 11-value list: %s" % names)
check("n1-enum-static-assert",
      re.search(r"static_assert\s*\(.*Button::NavNext.*M4NavButton::NavNext", mapped_h_raw + mapped_cpp_raw,
                re.DOTALL) is not None,
      "no static_assert pinning Button::NavNext == M4NavButton::NavNext")

# --- (b) recursion-before-synth/footer wiring --------------------------------
was_pressed = func_span(mapped_cpp, "bool MappedInputManager::wasPressed(")
was_released = func_span(mapped_cpp, "bool MappedInputManager::wasReleased(")
is_pressed = func_span(mapped_cpp, "bool MappedInputManager::isPressed(")
map_button = func_span(mapped_cpp, "bool MappedInputManager::mapButton(")
check("n1-waspressed-exists", was_pressed is not None, "wasPressed body missing")
check("n1-wasreleased-exists", was_released is not None, "wasReleased body missing")


def recursion_shape(body, getter, first, second, logical):
    if body is None:
        return False
    pat = (r"Button::" + logical + r"\s*\)\s*return\s+" + getter + r"\s*\(\s*" + BTN + first +
           r"\s*\)\s*\|\|\s*" + getter + r"\s*\(\s*" + BTN + second + r"\s*\)")
    return re.search(pat, body) is not None


check("n1-waspressed-recursion-next",
      recursion_shape(was_pressed, "wasPressed", "Down", "Right", "NavNext"),
      "wasPressed lacks NavNext -> wasPressed(Down) || wasPressed(Right) recursion")
check("n1-waspressed-recursion-previous",
      recursion_shape(was_pressed, "wasPressed", "Up", "Left", "NavPrevious"),
      "wasPressed lacks NavPrevious -> wasPressed(Up) || wasPressed(Left) recursion")
check("n1-wasreleased-recursion-next",
      recursion_shape(was_released, "wasReleased", "Down", "Right", "NavNext"),
      "wasReleased lacks NavNext -> wasReleased(Down) || wasReleased(Right) recursion")
check("n1-wasreleased-recursion-previous",
      recursion_shape(was_released, "wasReleased", "Up", "Left", "NavPrevious"),
      "wasReleased lacks NavPrevious -> wasReleased(Up) || wasReleased(Left) recursion")


def order_before(body, first_tok, second_tok):
    if body is None:
        return False
    a, b = body.find(first_tok), body.find(second_tok)
    return 0 <= a < b


check("n1-waspressed-recursion-before-synth",
      order_before(was_pressed, "NavNext", "synthKey_"),
      "wasPressed NavNext recursion must precede the synthetic-Key branch")
check("n1-wasreleased-recursion-before-synth",
      order_before(was_released, "NavNext", "synthKey_"),
      "wasReleased NavNext recursion must precede the synthetic-Key branch")
check("n1-wasreleased-recursion-before-footer",
      order_before(was_released, "NavNext", "M4FooterTouchPolicy::enabled"),
      "wasReleased NavNext recursion must precede the footer-tap branch")
check("n1-synth-matcher-pressed",
      was_pressed is not None and "m4SynthSatisfiesKey" in was_pressed,
      "wasPressed must consult m4SynthSatisfiesKey (member-expansion synth match)")
check("n1-synth-matcher-released",
      was_released is not None and "m4SynthSatisfiesKey" in was_released,
      "wasReleased must consult m4SynthSatisfiesKey (member-expansion synth match)")
check("n1-ispressed-level-or",
      is_pressed is not None and "NavNext" in is_pressed and "NavPrevious" in is_pressed
      and "isPressed" in is_pressed,
      "isPressed must fold NavNext/NavPrevious over member isPressed (level-OR)")
check("n1-ispressed-synth-inert",
      is_pressed is not None and "synthKey_" not in is_pressed,
      "isPressed must stay level-only: synthetic Keys never affect level")
check("n1-mapbutton-no-logical-case",
      map_button is not None and "NavNext" not in map_button and "NavPrevious" not in map_button,
      "mapButton must gain no NavNext/NavPrevious case (any reach is a bug)")
check("n1-wiring-includes-helper",
      "M4NavMapping" in mapped_cpp_raw or "M4NavMapping" in mapped_h_raw,
      "MappedInputManager must include util/M4NavMapping.h")

# --- (c) singleton getters, index/page math untouched ------------------------
next_body = func_span(nav_h, "getNextButtons()")
prev_body = func_span(nav_h, "getPreviousButtons()")
check("n1-next-singleton",
      next_body is not None and "NavNext" in next_body
      and "Button::Down" not in next_body and "Button::Right" not in next_body,
      "getNextButtons must return {NavNext} only, not raw Down/Right")
check("n1-previous-singleton",
      prev_body is not None and "NavPrevious" in prev_body
      and "Button::Up" not in prev_body and "Button::Left" not in prev_body,
      "getPreviousButtons must return {NavPrevious} only, not raw Up/Left")
check("n1-navigator-delegation-intact",
      "getNextButtons()" in nav_cpp and "getPreviousButtons()" in nav_cpp,
      "press/release/continuous helpers must keep delegating to the singleton getters")
for label, pat in (
    ("n1-index-next", r"nextIndex.*?\(\s*currentIndex\s*\+\s*1\s*\)\s*%\s*totalItems"),
    ("n1-index-previous", r"previousIndex.*?\(\s*currentIndex\s*\+\s*totalItems\s*-\s*1\s*\)\s*%\s*totalItems"),
    ("n1-page-next", r"\(\s*currentPageIndex\s*\+\s*1\s*\)\s*\*\s*itemsPerPage"),
    ("n1-page-previous", r"\(\s*currentPageIndex\s*-\s*1\s*\)\s*\*\s*itemsPerPage"),
):
    check(label, re.search(pat, nav_cpp, re.DOTALL) is not None,
          "ButtonNavigator index/page math changed: %s" % pat)

# --- (d) AppList: explicit Up/Down grid axes; Left/Right/Confirm/Back stay ----
check("n1-applist-vertical-explicit-axes",
      re.search(r"onRelease\s*\(\s*\{\s*" + BTN + r"Down\s*\}", applist) is not None
      and re.search(r"onRelease\s*\(\s*\{\s*" + BTN + r"Up\s*\}", applist) is not None,
      "AppList verticals must use explicit-axis onRelease({Down})/onRelease({Up})")
check("n1-applist-no-logical-fold",
      "NavNext" not in applist and "NavPrevious" not in applist,
      "AppList must never query logical buttons (Left/Right carry install/uninstall)")
check("n1-applist-grid-step-intact",
      "moveSelection(-kDrawerColumns)" in applist and "moveSelection(kDrawerColumns)" in applist,
      "AppList grid vertical step (+-kDrawerColumns) changed")
for label, tok in (
    ("n1-applist-left-dialog", "selectedIsPlugin()"),
    ("n1-applist-right-install", "openInstall()"),
    ("n1-applist-confirm-open", "openSelected()"),
    ("n1-applist-back", "onGoBack()"),
):
    check(label, tok in applist, "AppList binding lost: %s" % tok)

# --- (d) MyLibrary: linear rows logical; preview/action-menu region excluded --
check("n1-mylibrary-logical-release",
      "onNextRelease" in mylib and "onPreviousRelease" in mylib,
      "MyLibrary linear rows must use onNextRelease/onPreviousRelease logical")
check("n1-mylibrary-row-math-intact",
      "SKIP_PAGE_MS" in mylib and "selectorIndex" in mylib,
      "MyLibrary SKIP_PAGE_MS page-vs-row math changed")
for label, tok in (
    ("n1-mylibrary-preview-menu-kept", "PREVIEW_MENU_COUNT"),
    ("n1-mylibrary-preview-exit-kept", "GO_HOME_MS"),
    ("n1-mylibrary-action-menu-kept", "ACTION_MENU_COUNT"),
    ("n1-mylibrary-menu-latch-kept", "menuJustOpened"),
):
    check(label, tok in mylib, "MyLibrary preview/action-menu region changed: %s" % tok)
check("n1-mylibrary-preview-handrolled-kept",
      ("wasReleased" in mylib and "Button::Up" in mylib and "Button::Left" in mylib
       and "Button::Down" in mylib and "Button::Right" in mylib),
      "MyLibrary preview/action-menu rows must keep hand-rolled dispatch")

# --- (d) Home: lists logical press; Confirm/Back unchanged --------------------
check("n1-home-logical-press",
      "onNextPress" in home and "onPreviousPress" in home,
      "Home lists must use onNextPress/onPreviousPress logical")
check("n1-home-confirm-unchanged",
      "wasReleased" in home and "Button::Confirm" in home,
      "Home Confirm-released binding changed")
check("n1-home-back-unchanged",
      "wasPressed" in home and "Button::Back" in home,
      "Home Back-pressed early-return changed")
check("n1-home-focus-math-intact",
      "sceneFocusIndex" in home and "focusCount" in home,
      "Home focus wrap math changed")

# --- (d) Settings: exact S1/S2 rows logical release; Back/Confirm unchanged ---
check("n1-settings-s1-hub-logical",
      "onPreviousRelease" in settings and "onNextRelease" in settings
      and "settingsNavMoveHub" in settings,
      "Settings S1 Hub pane must use logical release around settingsNavMoveHub")
check("n1-settings-s2-row-logical",
      "onPreviousRelease" in settings and "onNextRelease" in settings
      and "settingsNavMoveRow" in settings and "settingsNavSyncWindow" in settings,
      "Settings S2 Row pane must use logical release around settingsNavMoveRow+SyncWindow")
check("n1-settings-back-unchanged",
      "wasPressed" in settings and "Button::Back" in settings,
      "Settings Back-pressed row changed")
check("n1-settings-confirm-unchanged",
      "wasReleased" in settings and "Button::Confirm" in settings,
      "Settings Confirm-released row changed")

print()
if failures:
    print("RED: %d failing check(s) — logical navigation absent as intended:" % len(failures))
    for name in failures:
        print("  - " + name)
    sys.exit(1)
print("GREEN: logical-navigation adoption contract holds.")
