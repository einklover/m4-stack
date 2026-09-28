# Open-source puzzle references and concrete M4 task handoff (2026-09-28)

The user requested adapting real, established open-source puzzle content instead of inventing superficial filler. This document supersedes the **original-only Sokoban level requirement** in M4_GAMEPLAY_V2_20260928.md. The UI, gameplay and verification expectations there still apply. Work on Sokoban first. Do not modify main, the live app-store catalog or any firmware/kernel source in this task.

## References and license boundaries

1. **Sokoban / Microban**: David W. Skinner, 155 original numbered puzzles, revised April 2000. Read the author-attributed source file at https://github.com/OMerkel/Sokoban/blob/master/3rdParty/Levels/Microban.txt and KOReader's player/docs at https://github.com/kbarni/sokoban.koplugin. KOReader describes Microban as public domain, but the source header names Skinner as copyright owner and contains no explicit license grant. This repo currently stores 24 maps as a *development-only fixture*; confirm actual redistribution permissions before including maps in a public app-store release. Preserve level IDs, attribution and source link. Do not copy KOReader's GPL-3.0 implementation code into this plugin.
2. **Sliding and Nonogram**: Simon Tatham's Portable Puzzle Collection, https://www.chiark.greenend.org.uk/~sgtatham/puzzles/ (MIT) and its source Fifteen/Pattern. Study generation, puzzle IDs, difficulty and solver invariants; if porting code, include the original MIT notice. For Nonogram also see https://github.com/timonkobusch/nonogram (MIT, generator + human solver).
3. **Connect / Shisen-Sho**: current M4 rules engine already handles 0/1/2-turn routes and outside-border paths. Reference the game behavior but implement independent reverse-removal placement; no static row of adjacent identical pairs. Avoid copying GPL-only logic/assets without complying with license.

## Ready-to-use Microban M4 selection

Fixture: `plugins/m4-lua-sokoban-plugin/tools/fixtures/microban_curated24.xsb`.
Offline converter: `plugins/m4-lua-sokoban-plugin/tools/import_microban.py` (requires a local copy of the original upstream text; do not fetch at device runtime).
Offline Dijkstra verifier: `plugins/m4-lua-sokoban-plugin/tools/level_audit.py`.

In deliberate difficulty-based display order, three chapters of eight:

- 初识搬运: Microban IDs `44,2,21,4,25,1,17,24`.
- 绕路布局: IDs `9,26,27,14,23,12,15,3`.
- 次序机关: IDs `18,22,20,11,19,13,10,8`.

These 24 original-author puzzle maps have been normalized for M4 without changing interior tiles. Each passed an *individual* bounded host search with a 20,000-state cap during curation. There were no identical maps under rotation/mirroring; each has matching goal and crate counts. Largest width 11, height 12; 23 have 2+ crates and 7 have 3+ crates. The deliberate introductory first map is Microban #44 (1 push); the subsequent levels exercise actual 3–32-push solutions. This is not a claim of visual or real-device acceptance.

Observed shortest **push counts** in display order:
`1,3,5,7,7,8,9,9 | 10,10,10,10,10,11,12,13 | 13,15,16,16,20,21,21,32`.

The source belongs to Skinner: never write “24 original M4-created maps.” The plugin's current README/manifest still describes its old original 12-level V1 and must be changed when the new set is actually integrated.

## Exact implementation work for songzhangchi01 Grok 4.7 High

1. First import **only these staged 24 maps** into `Game.LEVELS` (or a separate packaged read-only module), store `Game.MICROBAN_IDS`, chapter names and original source attribution. Do not make up new maps or add unverified extras. Run `level_audit.py --quality-v2` on the actual shipped `game.lua`; fix the parser/normalization if necessary. Keep the device free of solving/search tasks. Keep the original 12-level save file untouched: use a separately named Microban-progress file and a version/level-set marker.
2. The current 24-level picker would run off the 480x800 screen. Add **8-level pagination**, visible chapter label, chapter-level index, progress/best moves and touch/physical-key selection. Make win display a functional Next button and final-level chapter completion; don't invent impossible screenshots. Distinct high-contrast 1-bit target, crate, crate-on-goal, wall, player glyphs; actual GUI methods must come from existing M4 API. Preserve undo/restart, system Back, goal semantics and fail-safe deadlock message. Bounded tap-to-walk must never auto-push.
3. Make host tests prove every shipped map (individual bound or improved push-state solver; don't run unbounded Lua BFS over all difficult maps). Test migration/isolation of old progress, picker pagination+touch hit boxes, win-next flow, save/load and bad file fallback. Capture a *real* 480x800 rendered screenshot/UI trace for selection, mid-game and win from a run/render harness; name missing evidence honestly. Package only after tests. Independent Muse High read-only review after the real frozen code SHA.
4. No Astra. One writer/worktree; do not start a second agent or ask the user what to do. Commit and push **only** the isolated task branch. No firmware changes, live app-store publication, merge to main or hardware flash without separate integration approval.

## Future changes for other games

- Sliding: implement coherent 1-bit picture tiles, not repeated abstract diamond fragments; compare against Tatham's MIT Fifteen logic and preserve its license if ported.
- Nonogram: derive *uniquely solvable* visual pixel-art puzzles using Tatham Pattern or timonkobusch's MIT solver; no geometric filler.
- Connect: construct decks backwards from a proven removal sequence and test that all pairs can be removed with <=2 bends; at least 30% of initial same-symbol pairs must be nonadjacent.
