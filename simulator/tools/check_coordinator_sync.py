#!/usr/bin/env python3
"""Compile both PageTurnCoordinator headers and compare behavioral fingerprints.

The two repositories may format/refactor equivalent conditions differently. A
text diff would therefore produce false drift. This probe executes the same
production and injected-bug event corpus against both headers and requires an
identical state/decision transcript.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

SOURCE = r'''
#include <cstdint>
#include <iostream>
#include HEADER_PATH
using m4reader::PageTurnCoordinator;

static void dump(const char* tag, const PageTurnCoordinator& c, bool busy, bool sup,
                 bool indexComplete, int cursor) {
  std::cout << tag << ':'
            << c.targetPage() << ',' << c.physicalPage() << ','
            << c.pendingPhysicalPage() << ',' << c.updateRequired() << ','
            << c.firstShown() << ',' << c.quickMode() << ','
            << c.shouldRender(busy, sup) << ',' << c.catchupNeeded(busy, sup) << ','
            << c.targetBeyondIndex(indexComplete, cursor) << ','
            << c.needsCatchupRender() << '\n';
}

static void run(PageTurnCoordinator::TestPolicy p) {
  PageTurnCoordinator c(p);
  uint32_t t = 1000;
  dump("reset", c, false, false, false, 1);
  c.onTap(t, false, 1); dump("slow1", c, false, false, false, 1);
  c.onRenderStarted(); dump("render1", c, false, false, false, 1);
  c.onTap(t + 90, true, 2); dump("busy2", c, true, false, false, 2);
  const int frame = c.onFrameReady();
  std::cout << "frame:" << frame << '\n';
  std::cout << "commit_ret:" << c.onCommitted(t + 500, frame) << '\n';
  dump("commit1", c, false, false, false, 2);
  if (c.catchupNeeded(false, false)) c.forceCatchupArm();
  dump("arm", c, false, false, false, 2);
  c.onRenderStarted(); dump("render2", c, false, false, true, 99);
  std::cout << "commit_ret:" << c.onCommitted(t + 900, c.onFrameReady()) << '\n';
  dump("commit2", c, false, false, true, 99);
  c.setTarget(8); c.setPhysical(3); c.setUpdateRequired(false);
  dump("shadow_align", c, false, false, false, 5);
  c.rearm(); dump("rearm", c, false, true, false, 5);
  c.reset();
  c.onTap(0xFFFFFFF0u, false, 1);
  c.onTap(0x00000020u, false, 2);
  dump("wrap", c, false, false, true, 3);
}

int main() {
  run(PageTurnCoordinator::productionPolicy());
  PageTurnCoordinator::TestPolicy noCatch; noCatch.bugNoCatchup = true; run(noCatch);
  PageTurnCoordinator::TestPolicy live; live.bugLivePhysical = true; run(live);
  PageTurnCoordinator::TestPolicy threshold; threshold.quickTapMs = 17; run(threshold);
  return 0;
}
'''


def fingerprint(header: Path, cxx: str) -> str:
    with tempfile.TemporaryDirectory(prefix="m4-coord-") as tmp:
        root = Path(tmp)
        src = root / "probe.cpp"
        exe = root / "probe"
        src.write_text(SOURCE, encoding="utf-8")
        cmd = [cxx, "-std=c++17", f'-DHEADER_PATH="{header.resolve()}"', str(src), "-o", str(exe)]
        try:
            subprocess.run(cmd, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            return subprocess.run([str(exe)], check=True, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True).stdout
        except subprocess.CalledProcessError as exc:
            raise RuntimeError(
                f"command failed: {shlex.join(exc.cmd)}\nstdout:\n{exc.stdout}\nstderr:\n{exc.stderr}"
            ) from exc


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("simulator")
    p.add_argument("firmware")
    p.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = p.parse_args(argv)
    try:
        left = fingerprint(Path(args.simulator), args.cxx)
        right = fingerprint(Path(args.firmware), args.cxx)
    except (OSError, RuntimeError) as exc:
        print(f"coordinator sync error: {exc}", file=sys.stderr)
        return 2
    if left != right:
        print("ERROR: PageTurnCoordinator behavioral drift", file=sys.stderr)
        print("--- simulator ---", file=sys.stderr)
        print(left, file=sys.stderr)
        print("--- firmware ---", file=sys.stderr)
        print(right, file=sys.stderr)
        return 1
    print(f"PASS coordinator behavior matches ({len(left.splitlines())} observations)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
