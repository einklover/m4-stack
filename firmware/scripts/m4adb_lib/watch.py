"""Watch plugin sources and re-run journey on content-hash change."""

from __future__ import annotations

import time
from pathlib import Path
from typing import Callable, Optional

from . import package as pkg


def watch_loop(
    source: Path,
    on_change: Callable[[Path, str], None],
    poll_s: float = 1.0,
    debounce_s: float = 0.8,
    once: bool = False,
    stop_flag: Optional[Callable[[], bool]] = None,
) -> None:
    """
    Poll source (file or directory) for content-hash changes.
    Calls on_change(source, hash) on initial start and each change after debounce.
    """
    last_hash: Optional[str] = None
    pending_since: Optional[float] = None
    pending_hash: Optional[str] = None

    def current() -> str:
        if source.is_dir():
            return pkg.content_hash_dir(source)
        return pkg.content_hash_file(source)

    # Initial
    h = current()
    on_change(source, h)
    last_hash = h
    if once:
        return

    while True:
        if stop_flag and stop_flag():
            return
        try:
            h = current()
        except Exception:
            time.sleep(poll_s)
            continue
        if h != last_hash:
            if pending_hash != h:
                pending_hash = h
                pending_since = time.time()
            elif pending_since is not None and (time.time() - pending_since) >= debounce_s:
                on_change(source, h)
                last_hash = h
                pending_hash = None
                pending_since = None
        else:
            pending_hash = None
            pending_since = None
        time.sleep(poll_s)
