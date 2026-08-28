#!/usr/bin/env python3
"""RED/GREEN contract for a one-repository public M4 build."""

from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[2]

REQUIRED_VENDOR_PATHS = (
    "firmware/open-m4-sdk",
    "firmware/lib/Epub",
    "firmware/lib/Lua",
    "firmware/lib/expat",
    "firmware/lib/miniz",
    "firmware/lib/picojpeg",
    "firmware/src/network/updater_fw.bin",
)

FONT_GENERATOR_FILES = (
    "firmware/lib/EpdFont/scripts/convert-builtin-fonts.sh",
    "firmware/lib/EpdFont/scripts/fontconvert.py",
    "firmware/lib/EpdFont/scripts/requirements.txt",
    "firmware/lib/EpdFont/m4_ui_charset.txt",
)

# These files are executable build/configuration surfaces, not historical notes.
BUILD_SURFACES = (
    ROOT / "scripts",
    ROOT / "firmware" / "scripts",
    ROOT / "simulator" / "qemu",
    ROOT / "simulator" / "m4sim.py",
    ROOT / ".github" / "workflows",
    ROOT / "README.md",
    ROOT / "VERSIONS.md",
    ROOT / "docs" / "BUILD_AND_DEPS.md",
)


def _files_under(path: Path) -> list[Path]:
    if path.is_file():
        return [path]
    return [candidate for candidate in path.rglob("*") if candidate.is_file()]


def test_public_build_owns_all_dependency_inputs() -> None:
    missing = [rel for rel in REQUIRED_VENDOR_PATHS if not (ROOT / rel).exists()]
    assert not missing, f"missing vendored dependency inputs: {missing}"
    untracked = []
    for rel in REQUIRED_VENDOR_PATHS:
        listing = subprocess.run(
            ["git", "ls-files", "--", rel],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
        if not listing:
            untracked.append(rel)
    assert not untracked, f"dependency inputs exist only in a local cache: {untracked}"

    missing_font_tools = [rel for rel in FONT_GENERATOR_FILES if not (ROOT / rel).is_file()]
    assert not missing_font_tools, f"missing in-repo font generation tools: {missing_font_tools}"

    gitignore_text = (ROOT / "firmware" / ".gitignore").read_text(encoding="utf-8")
    assert "lib/EpdFont/builtinFonts" in gitignore_text
    tracked_fonts = subprocess.run(
        ["git", "ls-files", "--", "firmware/lib/EpdFont/builtinFonts"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
    assert not tracked_fonts, "generated builtinFonts must not be committed"

    build_docs = (ROOT / "docs" / "BUILD_AND_DEPS.md").read_text(encoding="utf-8")
    for required_phrase in ("provide", "TTF", "builtinFonts", "convert-builtin-fonts.sh"):
        assert required_phrase in build_docs, f"font workflow is not documented: {required_phrase}"

    forbidden = ("einklover/m4-device", "f86b134")
    references = []
    for surface in BUILD_SURFACES:
        for path in _files_under(surface):
            try:
                text = path.read_text(encoding="utf-8")
            except UnicodeDecodeError:
                continue
            for token in forbidden:
                if token in text:
                    references.append(f"{path.relative_to(ROOT)}: {token}")
    assert not references, "active build surfaces still require private dependency repo: " + "; ".join(references)


if __name__ == "__main__":
    test_public_build_owns_all_dependency_inputs()
    print("m4 self-contained build contract: PASS")
