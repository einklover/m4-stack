#!/usr/bin/env python3
"""Clone pinned Espressif QEMU, apply Murphy patches, and build Xtensa QEMU."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
PATCH_DIR = HERE / "patches"
UPSTREAM = PATCH_DIR / "upstream.json"
SERIES = PATCH_DIR / "series"


def run(cmd: list[str], cwd: Path | None = None) -> None:
    print("+", " ".join(str(x) for x in cmd), flush=True)
    subprocess.run(cmd, cwd=cwd, check=True)


def load_manifest() -> tuple[str, str]:
    data = json.loads(UPSTREAM.read_text(encoding="utf-8"))
    return str(data["repository"]), str(data["ref"])


def load_series() -> list[Path]:
    result: list[Path] = []
    for raw in SERIES.read_text(encoding="utf-8").splitlines():
        name = raw.strip()
        if not name or name.startswith("#"):
            continue
        path = PATCH_DIR / name
        if not path.is_file():
            raise RuntimeError(f"missing patch in series: {path}")
        if path.suffix not in {".patch", ".diff", ".py"}:
            raise RuntimeError(f"unsupported patch entry type: {path.name}")
        result.append(path)
    return result


def ensure_tools() -> None:
    missing = [name for name in ("git", "ninja") if shutil.which(name) is None]
    if missing:
        raise RuntimeError("missing required tools: " + ", ".join(missing))


def apply_entry(src: Path, patch: Path) -> None:
    if patch.suffix == ".py":
        run([sys.executable, str(patch), str(src)], cwd=src)
    else:
        run(["git", "apply", "--check", str(patch)], cwd=src)
        run(["git", "apply", str(patch)], cwd=src)
    run(["git", "diff", "--check"], cwd=src)


def prepare_source(src: Path, *, force: bool) -> None:
    repo, ref = load_manifest()
    if force and src.exists():
        shutil.rmtree(src)
    if not src.exists():
        src.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "clone", "--filter=blob:none", repo, str(src)])
    run(["git", "fetch", "origin", ref], cwd=src)
    run(["git", "checkout", "--detach", ref], cwd=src)
    run(["git", "reset", "--hard", ref], cwd=src)
    run(["git", "clean", "-fdx"], cwd=src)

    before = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=src, text=True).strip()
    for patch in load_series():
        apply_entry(src, patch)
    if not subprocess.check_output(["git", "status", "--porcelain"], cwd=src, text=True).strip():
        raise RuntimeError("patch series produced no source changes")
    after = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=src, text=True).strip()
    if before != after:
        raise RuntimeError("patch application unexpectedly changed upstream HEAD")


def configure_and_build(src: Path, *, jobs: int, reconfigure: bool) -> Path:
    build = src / "build-murphy"
    binary = build / "qemu-system-xtensa"
    configure = src / "configure"
    if not configure.is_file():
        raise RuntimeError(f"QEMU configure script missing: {configure}")

    if reconfigure and build.exists():
        shutil.rmtree(build)
    if not build.exists():
        build.mkdir(parents=True)
        run(
            [
                str(configure),
                "--target-list=xtensa-softmmu",
                "--disable-werror",
                "--disable-docs",
            ],
            cwd=build,
        )
    run(["ninja", "-j", str(max(1, jobs))], cwd=build)
    if not binary.is_file():
        raise RuntimeError(f"expected QEMU binary not produced: {binary}")
    return binary


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        "--source-dir",
        default=str(Path.home() / ".cache" / "murphy-m4" / "espressif-qemu"),
        help="clone/build directory",
    )
    p.add_argument("-j", "--jobs", type=int, default=4)
    p.add_argument("--force-source", action="store_true", help="discard and reclone source")
    p.add_argument("--reconfigure", action="store_true", help="discard QEMU build directory")
    p.add_argument("--check-only", action="store_true", help="clone/reset and apply the full checked series")
    args = p.parse_args(argv)

    src = Path(args.source_dir).expanduser().resolve()
    try:
        ensure_tools()
        prepare_source(src, force=args.force_source)
        if args.check_only:
            print("patch series applied cleanly")
            return 0
        binary = configure_and_build(src, jobs=args.jobs, reconfigure=args.reconfigure)
    except (OSError, RuntimeError, subprocess.CalledProcessError, json.JSONDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    print(f"Murphy-patched QEMU: {binary}")
    print(f"export QEMU_XTENSA={binary}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
