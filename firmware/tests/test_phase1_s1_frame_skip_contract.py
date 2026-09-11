"""Phase1 S1 RED: frame-skip contract gates (expected FAIL until GREEN).

Pins Task 10 production wiring:
  1. M4SuppressState helper exists with the pinned shape.
  2. Physical-only MappedInputManager wiring (synthetic inert).
  3. beginFrame() untouched by suppression.
  4. Frame-top check between gesture routing and currentActivity->loop().
  5. Skip-to-frame-tail around actual tail identifiers (no bare return).
  6. Back 1500ms threshold + onGoHome() preserved, firing via helper.
"""
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
FW = REPO / "firmware"
SRC = FW / "src"

failures = []


def fail(msg):
    failures.append(msg)
    print(f"FAIL: {msg}")


def ok(msg):
    print(f"ok: {msg}")


SUPPRESS_TOKENS = ("M4SuppressState", "m4SuppressNextRelease",
                   "m4ConsumeSuppressedRelease", "m4LongPressFired",
                   "firedLatch")
# Actual frame-tail bookkeeping identifiers from main.cpp:1580-1660.
TAIL_IDS = ("deferredDeleteActivity", "frontlight", "delay(10)", "yield()")


def read_sources():
    exts = {".cpp", ".h", ".hpp", ".ino"}
    files = [p for p in FW.rglob("*") if p.is_file() and p.suffix in exts]
    by_text = {}
    for p in files:
        try:
            by_text[p] = p.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
    return by_text


def main():
    by_text = read_sources()
    blob = "\n".join(by_text.values())

    # 1. Helper exists with pinned shape.
    state_hits = [p for p in by_text if p.name == "M4SuppressState.h"]
    if not state_hits:
        fail("M4SuppressState.h missing under firmware/ (Task 10 GREEN)")
    else:
        decl = "\n".join(by_text[p] for p in state_hits)
        for token in ("struct M4SuppressState", "uint16_t mask",
                      "uint16_t firedLatch", "m4SuppressButtonIndex",
                      "m4SuppressNextRelease", "m4ConsumeSuppressedRelease",
                      "m4LongPressFired", "physicalReleasedMask",
                      "thresholdMs"):
            if token not in decl:
                fail(f"M4SuppressState.h missing pinned token: {token}")
            else:
                ok(f"M4SuppressState declares {token}")

    # 2. Physical-only MappedInputManager wiring.
    mapped_h = SRC / "MappedInputManager.h"
    mapped_cpp = SRC / "MappedInputManager.cpp"
    mapped_texts = {}
    for p in (mapped_h, mapped_cpp):
        if p in by_text:
            mapped_texts[p] = by_text[p]
    if not mapped_texts:
        fail("MappedInputManager.h/cpp not found under firmware/src/")
    else:
        wired = [p for p, t in mapped_texts.items()
                 if any(tok in t for tok in SUPPRESS_TOKENS)]
        if not wired:
            fail("MappedInputManager has no physical suppress-API wiring")
        else:
            ok(f"MappedInputManager wiring in {[p.name for p in wired]}")
        # Synthetic inertness: synthetic paths must never reference suppression.
        synth_pat = re.compile(
            r"injectSyntheticKey|syntheticBack|synthKey_|SynthKind", re.IGNORECASE)
        for p, t in mapped_texts.items():
            for m in synth_pat.finditer(t):
                window = t[max(0, m.start() - 600):m.end() + 600]
                if any(tok in window for tok in SUPPRESS_TOKENS):
                    fail(f"{p.name} mixes suppression with synthetic path "
                         f"(must be physical-only)")
                    break
        else:
            ok("synthetic paths free of suppression references")

    # 3. beginFrame() untouched: exact clear set only, no suppression refs.
    if mapped_cpp in by_text:
        t = by_text[mapped_cpp]
        m = re.search(r"beginFrame\s*\(\s*\)[^{]*\{", t)
        if not m:
            fail("MappedInputManager::beginFrame body not found")
        else:
            # Conservative body slice: next 2500 chars cover the clear set.
            body = t[m.end():m.end() + 2500]
            if any(tok in body for tok in SUPPRESS_TOKENS):
                fail("beginFrame() touches suppression state (must survive untouched)")
            else:
                ok("beginFrame() untouched by suppression")
    else:
        fail("MappedInputManager.cpp not found for beginFrame pin")

    # 4+5. main.cpp placement + skip-to-tail around actual tail identifiers.
    main_cpp = SRC / "main.cpp"
    main_text = by_text.get(main_cpp)
    if main_text is None:
        # Fall back to any file owning the frame site.
        cands = [p for p, t in by_text.items() if "currentActivity->loop" in t]
        if not cands:
            fail("currentActivity->loop frame site not found")
            main_text = ""
        else:
            main_cpp = cands[0]
            main_text = by_text[main_cpp]
    if main_text:
        # Tail identifiers must exist at the frame site (preservation pin).
        missing_tail = [tid for tid in TAIL_IDS if tid not in main_text]
        if missing_tail:
            fail(f"frame-tail bookkeeping missing identifiers: {missing_tail}")
        else:
            ok(f"frame-tail identifiers present ({', '.join(TAIL_IDS)})")

        consume_sites = list(
            re.finditer(r"m4ConsumeSuppressedRelease", main_text))
        if not consume_sites:
            fail("no frame-top m4ConsumeSuppressedRelease in main.cpp "
                 "(after gesture routing, before currentActivity->loop)")
        else:
            placed = False
            tail_ok = False
            for site in consume_sites:
                before = main_text[max(0, site.start() - 6000):site.start()]
                after = main_text[site.end():site.end() + 6000]
                gesture_before = ("wasHomeGesture" in before
                                  or "wasHistoryGesture" in before
                                  or "wasBackGesture" in before
                                  or "gesture" in before.lower())
                loop_after = "currentActivity->loop" in after
                if gesture_before and loop_after:
                    placed = True
                    ok(f"frame-top consume check placed in {main_cpp.name}")
                # Skip-to-tail: consume-true path must still reach every tail
                # identifier and must not bare-return past the bookkeeping.
                tail_after = all(tid in after for tid in TAIL_IDS)
                segment = after.split("currentActivity->loop")[0] \
                    if "currentActivity->loop" in after else after[:3000]
                bare_return = re.search(r"(?<![A-Za-z_:])return\s*;",
                                        segment)
                guarded = re.search(r"if\s*\(\s*!?\s*\w*(consum|suppress)",
                                    after, re.IGNORECASE)
                goto_tail = re.search(r"goto\s+\w*(tail|bookkeeping|end)",
                                      after, re.IGNORECASE)
                if tail_after and (guarded or goto_tail) and not bare_return:
                    tail_ok = True
                    ok("skip-to-tail reaches deferred-delete/frontlight/"
                       "delay/yield without bare return")
            if not placed:
                fail("consume check not between gesture routing and "
                     "currentActivity->loop()")
            if not tail_ok:
                fail("skip-to-tail missing: consume-true path must guard only "
                     "currentActivity->loop() and still run deferred-delete "
                     "drain + frontlight re-apply + delay(10)/yield()")

    # 6. Back site: threshold + destination preserved, firing via helper.
    if "1500" not in blob:
        fail("Back 1500ms threshold missing")
    else:
        ok("Back 1500 threshold present")
    if "onGoHome" not in blob:
        fail("onGoHome missing (destination must be preserved)")
    else:
        ok("onGoHome preserved")
    back_users = [p for p, t in by_text.items()
                  if "m4LongPressFired" in t and "1500" in t
                  and "tests" not in p.parts]
    if not back_users:
        fail("Back 1.5 s site not converted: m4LongPressFired + 1500 "
             "must share the site (only firing/release-consumption changes)")
    else:
        ok(f"Back site converted in {[p.name for p in back_users]}")

    if failures:
        print(f"\nphase1-s1 frame-skip contract RED: {len(failures)} gate(s) failing")
        return 1
    print("\nphase1-s1 frame-skip contract: all gates pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
