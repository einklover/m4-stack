#!/usr/bin/env python3
"""Generate self-contained 480x800 pixel-style HTML previews for M4 screens."""

from __future__ import annotations

import argparse
import html
from pathlib import Path

from m4ui_preview import DEFAULT_OUTPUT_DIR, SCREENS, validate_output_dir


SCREEN_TEXT = {
    "home": ("晋江文学城", "分类热推", "古代言情 · 24 本", "继续阅读"),
    "detail": ("晋江文学城", "霍格沃茨的学习面板", "官场职场 · 连载 · 131.2万字", "继续阅读"),
}


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Murphy M4 480x800 pixel-style HTML preview")
    parser.add_argument("--screen", choices=["home", "detail", "both", "all"], default="both")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUTPUT_DIR)
    return parser


def selected_screens(screen: str) -> tuple[str, ...]:
    return SCREENS if screen in ("both", "all") else (screen,)


def render_html(screen: str) -> str:
    title, heading, metadata, action = SCREEN_TEXT[screen]
    values = [title, heading, metadata, action]
    escaped = [html.escape(value) for value in values]
    return f"""<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=480, height=800, initial-scale=1">
  <title>{escaped[0]} - M4 preview</title>
  <style>
    :root {{ color-scheme: light; }}
    body {{ margin: 0; background: #d8d8d8; font-family: sans-serif; }}
    .screen {{ box-sizing: border-box; width:480px; height:800px; margin: 1rem auto;
      padding: 30px; background: #fff; color: #111; border: 1px solid #888; }}
    .rule {{ border-top: 1px solid #111; margin: 1rem -30px; }}
    .muted {{ color: #666; }}
    .button {{ margin-top: 2rem; padding: 1rem; border: 1px solid #111; text-align: center; }}
  </style>
</head>
<body>
  <main class="screen">
    <header><strong>{escaped[0]}</strong><span style="float:right">87%</span></header>
    <div class="rule"></div>
    <h1>{escaped[1]}</h1>
    <p class="muted">{escaped[2]}</p>
    <div class="button">{escaped[3]}</div>
  </main>
</body>
</html>
"""


def render(out: Path, screen: str) -> list[Path]:
    out = validate_output_dir(out)
    out.mkdir(parents=True, exist_ok=True)
    paths = []
    for name in selected_screens(screen):
        path = out / f"{name}.html"
        path.write_text(render_html(name), encoding="utf-8")
        paths.append(path)
    return paths


def main() -> None:
    args = build_parser().parse_args()
    try:
        for path in render(args.out, args.screen):
            print(path)
    except (ValueError, OSError) as exc:
        raise SystemExit(str(exc)) from exc


if __name__ == "__main__":
    main()
