"""Phase1 S1 RED: frame-skip contract gates (expected FAIL until GREEN).

Checks:
  1. M4SuppressState helper exists.
  2. Physical-only MappedInputManager wiring to suppress APIs.
  3. beginFrame untouched (no suppress/skip logic inside it).
  4. Frame-top consume check after gesture routing, before currentActivity->loop.
  5. Skip-to-tail bookkeeping present.
  6. Back 1500ms threshold + onGoHome preserved.
"""
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
FW = REPO / "firmware"

failures = []


def fail(msg):
    failures.append(msg)
    print(f"FAIL: {msg}")


def ok(msg):
    print(f"ok: {msg}")


def find_sources():
    exts = {".cpp", ".h", ".hpp", ".ino"}
    files = [p for p in FW.rglob("*") if p.is_file() and p.suffix in exts]
    return files


def main():
    sources = find_sources()
    by_text = {}
    for p in sources:
        try:
            by_text[p] = p.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
    blob = "\n".join(by_text.values())

    # 1. M4SuppressState existence + four planned APIs declared.
    state_hits = [p for p in sources if p.name == "M4SuppressState.h"]
    if not state_hits:
        fail("M4SuppressState.h missing under firmware/")
    else:
        decl = "\n".join(by_text[p] for p in state_hits)
        for api in ("m4LongPressFired", "m4ConsumeSuppressedRelease",
                    "m4SuppressNextRelease", "m4SuppressButtonIndex"):
            if api not in decl:
                fail(f"M4SuppressState.h missing API {api}")
            else:
                ok(f"M4SuppressState declares {api}")

    # 2. Physical-only MappedInputManager wiring.
    mapped = [p for p, t in by_text.items() if "MappedInputManager" in t]
    if not mapped:
        fail("MappedInputManager not found in firmware sources")
    else:
        wired = [p for p in mapped
                 if "m4ConsumeSuppressedRelease" in by_text[p]
                 or "m4SuppressNextRelease" in by_text[p]]
        if not wired:
            fail("MappedInputManager has no suppress-API wiring")
        else:
            ok(f"MappedInputManager wiring in {[p.name for p in wired]}")
        # Physical-only: suppress wiring must not live in a synthetic/software-input path.
        for p in wired:
            t = by_text[p]
            if re.search(r"synthetic.*m4(Suppress|Consume)|m4(Suppress|Consume).*synthetic",
                         t, re.IGNORECASE):
                fail(f"{p} wires suppress APIs to synthetic input path (must be physical-only)")

    # 3. beginFrame untouched: no suppress/skip tokens inside its body.
    begin_hits = [p for p, t in by_text.items() if "beginFrame" in t]
    if not begin_hits:
        fail("beginFrame not found in firmware sources")
    else:
        touched = [p for p in begin_hits
                   if re.search(r"beginFrame[^}]{0,2000}m4(Suppress|Consume)|"
                                r"m4(Suppress|Consume)[^}]{0,2000}beginFrame",
                                by_text[p], re.DOTALL)]
        if touched:
            fail(f"beginFrame touched by suppress logic in {[p.name for p in touched]}")
        else:
            ok("beginFrame untouched by suppress/skip logic")

    # 4. Frame-top check after gesture routing, before currentActivity->loop.
    loop_files = [p for p, t in by_text.items() if "currentActivity->loop" in t]
    if not loop_files:
        fail("currentActivity->loop frame site not found")
    else:
        placed = False
        for p in loop_files:
            t = by_text[p]
            for m in re.finditer(r"m4ConsumeSuppressedRelease", t):
                window_before = t[max(0, m.start() - 4000):m.start()]
                window_after = t[m.end():m.end() + 4000]
                has_gesture_before = "gesture" in window_before.lower()
                has_loop_after = "currentActivity->loop" in window_after
                if has_gesture_before and has_loop_after:
                    placed = True
                    ok(f"frame-top consume check placed in {p}")
                    break
        if not placed:
            fail("no frame-top m4ConsumeSuppressedRelease after gesture routing before currentActivity->loop")

    # 5. Skip-to-tail bookkeeping.
    if not re.search(r"skip.*tail|tail.*skip|frameSkip|skippedFrame", blob, re.IGNORECASE):
        fail("skip-to-tail bookkeeping not found")
    else:
        ok("skip-to-tail bookkeeping present")

    # 6. Back 1500 threshold + onGoHome preserved.
    if "1500" not in blob:
        fail("Back 1500ms threshold missing")
    else:
        ok("Back 1500 threshold present")
    if "onGoHome" not in blob:
        fail("onGoHome missing (must be preserved)")
    else:
        ok("onGoHome preserved")

    if failures:
        print(f"\nphase1-s1 frame-skip contract RED: {len(failures)} gate(s) failing")
        return 1
    print("\nphase1-s1 frame-skip contract: all gates pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
