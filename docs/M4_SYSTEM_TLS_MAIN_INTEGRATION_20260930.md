# M4 system TLS and stability integration, 2026-09-30

Base: f3c47b6 (origin/main). Tested source: d1570d00b0cc385a59345f101b526f42d83d5d45.

Import the final firmware tree and its theme dependencies, simulator caller tests,
and Sokoban 0.2.0. This includes prerequisite application/registry/font infrastructure
missing from main; it does not merge the historical branch graph. Preserve main's
20-plugin inventory, four additional plugins, workflow and operating contract.
The other plugin sources and the public catalog are unchanged. Sokoban 0.2.0 is
already published on gh-pages (3496a9676dd6c461dc99908e3dec47de4abc6805).

The change uses PSRAM for mbedTLS allocations and one admission policy for native
and Lua HTTPS callers. It also includes bounded SD enumeration, resumable glyph
index scanning, bitmap validation, idle-flush OOM recovery, owner-loop file-transfer
rendering and the App Store layout correction.

## Evidence and limitations

Actual integration host commands:
- python3 firmware/tests/test_m4_tls_memory.py
- python3 firmware/tests/test_m4_stability_takeover.py
- Sokoban host compiled against vendored Lua (excluding linit.c, whose optional
  unsafe standard libraries are absent); test_game.lua and test_ui.lua.
- python3 plugins/m4-lua-sokoban-plugin/tools/level_audit.py --quality-v2

Production and QEMU builds are run sequentially at integration. The GitHub
m4-development-gate workflow must succeed before advancing protected main.
No independent Muse review is claimed: the user requested direct takeover and
main integration. Astra is not requested. No protections or workflows are changed.

The source firmware d1570d00 was physically flashed to APP1 with hash verification;
Home, SD and USB were confirmed. Earlier native App Store refresh and Sokoban
package download succeeded. The final unified Lua HTTPS routes have host/source
and compile evidence, not a physical plugin HTTPS end-to-end pass. TXT final
short-power shutdown progress persistence remains open. This is not a declaration
that all system stability issues are resolved. This integration does not flash again.
