#!/usr/bin/env python3
"""Source contracts for WeRead/Fanqie cooperative TOC (JJWXC-style phasing).

Locks the merged OX plugin-blocking fix:
  - TOC load is split into painted phases (paint -> cache -> download/connect -> open)
  - ensure_network is isolated from the TOC GET (own tick)
  - FileRows/JSON prefetch is honored BEFORE any chapters={} wipe
  - a prefetch hit never calls Api.fetch_toc*
  - JJWXC is not rewritten by this change
Host/static only: no QEMU, ADB, hardware, or Lua runtime.
"""
from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WEREAD = ROOT / "plugins/m4-weread-plugin/main.lua"
FANQIE = ROOT / "plugins/m4-fanqie-plugin/main.lua"
JJWXC = ROOT / "plugins/m4-jjwxc-plugin/main.lua"


def lua_function(src: str, name: str) -> str:
    marker = f"function {name}("
    start = src.index(marker)
    nxt = src.find("\nfunction ", start + len(marker))
    if nxt < 0:
        return src[start:]
    return src[start:nxt]


class PluginTocPhasingContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.weread = WEREAD.read_text(encoding="utf-8")
        cls.fanqie = FANQIE.read_text(encoding="utf-8")
        cls.jjwxc = JJWXC.read_text(encoding="utf-8")

    def _assert_phased_toc(self, src: str, label: str) -> None:
        advance = lua_function(src, "advance_network_job")
        begin = lua_function(src, "begin_toc_load")
        finish = lua_function(src, "finish_toc_load")

        # Phases: paint -> cache -> download -> connect -> open.
        for phase in ('job.phase == "paint"', 'job.phase == "cache"',
                      'job.phase == "download"', 'job.phase == "connect"',
                      'job.phase == "open"'):
            self.assertIn(phase, advance, f"{label} missing {phase}")

        cache = advance[advance.index('job.phase == "cache"'):advance.index('job.phase == "download"')]
        self.assertIn("begin_toc_load(job.data)", cache)
        self.assertIn("toc_prefetched", cache)
        self.assertNotIn("ensure_network()", cache)
        self.assertNotIn("Api.fetch_toc", cache)

        download = advance[advance.index('job.phase == "download"'):advance.index('job.phase == "connect"')]
        self.assertIn("ensure_network()", download)
        self.assertNotIn("Api.fetch_toc", download)
        self.assertNotIn("finish_toc_load", download)

        connect = advance[advance.index('job.phase == "connect"'):advance.index('job.phase == "open"')]
        self.assertIn("finish_toc_load(book, {})", connect)
        self.assertNotIn("ensure_network()", connect)

        open_phase = advance[advance.index('job.phase == "open"'):]
        self.assertIn("toc_prefetched = true", open_phase)
        self.assertIn("cached_toc = cached_toc", open_phase)

        # begin_toc_load is cache-only and arms toc_prefetched on a FileRows hit.
        self.assertIn("network_job.toc_prefetched = true", begin)
        self.assertNotIn("Api.fetch_toc", begin)
        self.assertNotIn("ensure_network()", begin)

        # Honor prefetch BEFORE any wipe. The first chapters={} in finish_toc_load
        # must sit under `elseif not prefetched`, never before the prefetched check.
        pref = finish.index("local prefetched = opts.toc_prefetched")
        wipe = finish.index("chapters = {}")
        self.assertLess(pref, wipe, f"{label} wiped chapters before honoring toc_prefetched")
        self.assertIn("elseif not prefetched then", finish)
        self.assertLess(
            finish.index("elseif not prefetched then"),
            wipe,
            f"{label} chapters={{}} is not under the miss-only wipe branch",
        )

        # Prefetch hit marks loaded before any fetch_toc* call.
        loaded_guard = finish.index("if prefetched and (have_cached_json or chapter_catalog) then")
        fetch_file = finish.find("Api.fetch_toc_to_file")
        fetch_json = finish.find("Api.fetch_toc(")
        self.assertIn("loaded = true", finish[loaded_guard:loaded_guard + 200])
        if fetch_file >= 0:
            self.assertLess(loaded_guard, fetch_file, f"{label} FileRows fetch before prefetch guard")
        if fetch_json >= 0:
            self.assertLess(loaded_guard, fetch_json, f"{label} JSON fetch before prefetch guard")

    def test_weread_toc_is_cooperative(self) -> None:
        self._assert_phased_toc(self.weread, "weread")

    def test_fanqie_toc_is_cooperative(self) -> None:
        self._assert_phased_toc(self.fanqie, "fanqie")

    def test_jjwxc_was_not_rewritten_as_the_fix(self) -> None:
        # JJWXC already had advance_network_job + Api.toc_loader_spec
        # (progressive catalog) before this OX. The WeRead/Fanqie rewrite
        # must not copy toc_prefetched / lupa / finish_toc_load into JJWXC,
        # and must leave the existing loader-spec path in place.
        runtime = (ROOT / "plugins/m4-jjwxc-plugin/app_runtime_a.lua").read_text(encoding="utf-8")
        api = (ROOT / "plugins/m4-jjwxc-plugin/api.lua").read_text(encoding="utf-8")
        self.assertIn("function advance_network_job()", runtime)
        self.assertIn("function Api.toc_loader_spec(", api)
        self.assertIn("Api.toc_loader_spec(", runtime)
        self.assertNotIn("function begin_toc_load", runtime)
        self.assertNotIn("function finish_toc_load", runtime)
        for path in (ROOT / "plugins/m4-jjwxc-plugin").glob("*.lua"):
            text = path.read_text(encoding="utf-8")
            self.assertNotIn("toc_prefetched", text, msg=str(path))
            self.assertNotIn("lupa", text.lower(), msg=str(path))


if __name__ == "__main__":
    unittest.main()
