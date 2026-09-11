#!/usr/bin/env python3
"""Task 3 (INV-I1) RED — settle + boot-order source contract (no behavior change).

Pins the Phase 1 first-paint input-settle rule before it exists:
  (a) a first-paint completion wait exists in setup() after the home/reader
      branch and before waitForPowerRelease();
  (b) two gpio.update() samples spaced delay(10) follow the wait, still
      before waitForPowerRelease();
  (c) the settle site asserts/forces no pressedEvents/releasedEvents clear —
      the masks go clear only via the first subsequent normal loop-owned
      gpio.update();
  (d) the three serial markers exist ([MAIN] First paint wait ok /
      [MAIN] First paint wait timeout, proceed to settle /
      [MAIN] Input settle done) with no other serial format changed;
  (e) the synthetic one-shot path and the beginFrame clear keep exact
      semantics (40 ms gate, one-shot slot, beginFrame clear, loop order).

No host sampler test: the debounced sampler lives in the untouched SDK
(InputManager.h) behind HalGPIO, which cannot compile under bare g++.
A host re-implementation of debounce would assert only its own model.

RED gate: exits non-zero because the wait, the settle step, and the
markers are absent (boot still enter-then-wait-for-release).
Run: python3 firmware/tests/test_phase1_i1_boot_order_contract.py
"""

from pathlib import Path
import re
import sys

REPO = Path(__file__).resolve().parents[2]
MAIN_CPP = REPO / "firmware" / "src" / "main.cpp"
MAPPED_CPP = REPO / "firmware" / "src" / "MappedInputManager.cpp"
MAPPED_H = REPO / "firmware" / "src" / "MappedInputManager.h"
SDK_INPUT_H = (
    REPO
    / "firmware"
    / "open-m4-sdk"
    / "libs"
    / "hardware"
    / "InputManager"
    / "include"
    / "InputManager.h"
)

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


main_raw = read(MAIN_CPP)
main = mask_text(main_raw)
mapped_raw = read(MAPPED_CPP)
mapped = mask_text(mapped_raw)
mapped_h = mask_text(read(MAPPED_H))

WAIT_PAT = re.compile(r"\b(waitForFirstPaint|requestUpdateAndWait)\s*\(")
SETTLE_PAT = re.compile(r"gpio\.update\s*\(\s*\).*?delay\s*\(\s*10\s*\).*?gpio\.update\s*\(\s*\)", re.DOTALL)

# --- boot-tail region: setup() body from the home/reader branch ---------------
setup_body, setup_off = func_span(main, "void setup()")
check("boot-setup-exists", setup_body is not None, "void setup() missing")
if setup_body is None:
    setup_body = ""
setup_raw_body, _ = func_span(main_raw, "void setup()")
if setup_raw_body is None:
    setup_raw_body = ""

home1_pos = setup_raw_body.find("[MAIN] home1")
reader_pos = setup_raw_body.find("[MAIN] reader")
check("boot-branch-markers-intact", home1_pos >= 0 and reader_pos >= 0,
      "home-or-reader branch serial lines moved/renamed")
branch_pos = min(p for p in (home1_pos, reader_pos) if p >= 0) if (home1_pos >= 0 or reader_pos >= 0) else -1

# Masked-body twin of the tail: same slice offsets (masking preserves length).
tail = setup_body[branch_pos:] if branch_pos >= 0 else ""
tail_raw = setup_raw_body[branch_pos:] if branch_pos >= 0 else ""
power_wait = [m.start() for m in re.finditer(r"\bwaitForPowerRelease\s*\(\s*\)", tail)]
check("boot-power-release-still-last", len(power_wait) >= 1, "waitForPowerRelease() call missing from setup tail")
power_pos = power_wait[0] if power_wait else len(tail)
pre_wait_tail = tail[:power_pos]

# --- (a) first-paint completion wait after branch, before power release -------
waits = [m.start() for m in WAIT_PAT.finditer(pre_wait_tail)]
check("a-first-paint-wait-present", len(waits) >= 1,
      "no waitForFirstPaint/requestUpdateAndWait call between home/reader branch and waitForPowerRelease()")
wait_pos = waits[0] if waits else -1

# --- (b) two gpio.update() samples spaced delay(10) after the wait ------------
settle_span = pre_wait_tail[wait_pos:] if wait_pos >= 0 else ""
check("b-two-sample-settle", SETTLE_PAT.search(settle_span) is not None,
      "no gpio.update() x2 spaced delay(10) between first-paint wait and waitForPowerRelease()")

# --- (c) settle site forces no edge-mask clear; loop owns the first update ---
if wait_pos >= 0 and SETTLE_PAT.search(settle_span) is not None:
    m = SETTLE_PAT.search(settle_span)
    settle_site = settle_span[: m.end()]
    check("c-no-edge-clear-at-settle",
          "pressedEvents" not in settle_site and "releasedEvents" not in settle_site,
          "settle site must not assert/force pressedEvents/releasedEvents clear")
else:
    check("c-no-edge-clear-at-settle", False, "settle site absent — nothing pins the no-clear rule")
loop_body, _ = func_span(main, "void loop()")
check("c-loop-owns-first-update", loop_body is not None and "gpio.update" in loop_body,
      "loop() must keep its normal loop-owned gpio.update() that clears latched edges unread")

# --- (d) three serial markers; no other serial format changed -----------------
for label, marker in (
    ("d-marker-ok", "[MAIN] First paint wait ok"),
    ("d-marker-timeout", "[MAIN] First paint wait timeout, proceed to settle"),
    ("d-marker-settle", "[MAIN] Input settle done"),
):
    check(label, marker in main_raw, "serial marker absent: %s" % marker)
check("d-branch-lines-unmoved",
      '[MAIN] home1\\n' in main_raw and '[MAIN] reader\\n' in main_raw,
      "existing [MAIN] home1/reader line formats must not change")
tail_main_lines = re.findall(r"\[MAIN\] ([^\"\\]+)", tail_raw)
allowed = {"home1", "reader", "First paint wait ok",
           "First paint wait timeout, proceed to settle", "Input settle done"}
unknown = sorted({line.strip() for line in tail_main_lines} - allowed)
check("d-no-other-setup-marker", not unknown, "unexpected [MAIN] line(s) in setup tail: %s" % unknown)

# --- (e) synthetic one-shot path + beginFrame clear keep exact semantics ------
check("e-synth-rate-gate", "kSynthMinIntervalMs" in mapped and re.search(r"kSynthMinIntervalMs\s*=\s*40", mapped_h) is not None,
      "40 ms synthetic injection gate changed")
check("e-synth-one-shot-slot",
      re.search(r"synthKind_\s*!=\s*SynthKind::None", mapped) is not None
      and re.search(r"synthKind_\s*=\s*SynthKind::Key", mapped) is not None,
      "synthetic one-shot slot semantics changed")
check("e-beginframe-clear", re.search(r"synthKind_\s*=\s*SynthKind::None", mapped) is not None,
      "beginFrame() must still consume one-frame synthetic events exactly once")
check("e-loop-order-beginframe-first",
      loop_body is not None and loop_body.find("beginFrame()") >= 0
      and loop_body.find("beginFrame()") < loop_body.find(".poll()"),
      "loop must keep beginFrame() clearing previous synth events before poll() injects")
sdk_h = read(SDK_INPUT_H) if SDK_INPUT_H.is_file() else ""
check("e-sdk-sampler-untouched",
      "pressedEvents" in sdk_h and "releasedEvents" in sdk_h
      and re.search(r"DEBOUNCE_DELAY\s*=\s*5", sdk_h) is not None,
      "vendored InputManager sampler (edge masks, 5 ms debounce) must stay untouched")

print()
if failures:
    print("RED: %d failing check(s) — first-paint settle + boot order absent as intended:" % len(failures))
    for name in failures:
        print("  - " + name)
    sys.exit(1)
print("GREEN: settle + boot-order contract holds.")
