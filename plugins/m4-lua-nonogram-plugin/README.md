# 数织 (com.m4.nonogram)

Black-and-white Nonogram / Picross for the Murphy M4 e-ink screen (480×800). One static frame per change. No animation and no picture assets beyond the 62×64 home icon.

## Play

Each level is a 5×5 or 10×10 picture. Numbers beside a row and above a column are the filled runs in that line, generated from the picture (a blank line shows `0`).

- Tap a cell to cycle unknown → filled → X → unknown.
- Direction keys move the cursor. Confirm cycles the cursor cell.
- 撤销 undoes one cell change. 重开 clears the current level.
- 提示 fills one still-wrong cell (black first).
- 上关 / 下关 change level. A finished board’s confirm key also advances.
- 帮助 opens the on-screen control list. 返回 closes help; on the board it calls `sys.exit`.
- 退出 leaves the plugin the same way.

X and unknown both count as white. The level is won when every black cell is filled and no white cell is filled.

Progress (level, cursor, grid, short undo stack) is stored with `fs.writeFile("state.csv")` under the app-data directory and restored on the next launch.

## Levels

Thirteen original pictures ship in `game.lua`: eight 5×5 (十字, 方框, 丁字, 台阶, 菱形, 箭头, U形, 心形) and five 10×10 (横条, 方环, 中十字, 下阶, 上阶). They were drawn for this plugin. Clues are computed from those bitmaps; they are not copied from a puzzle pack.

`test_game.lua` contains a host-only counter. It enumerates legal row masks and searches row by row with a column-prefix prune, stopping at two solutions. The device build does not load that file. The test requires every shipped level to have exactly one solution, and it prints a branch score (sum of legal row masks) so 5×5 and 10×10 difficulty can be compared.

## References and license

Mechanics follow the usual picross rules. These repositories were read only as design references and were not copied:

- https://github.com/timonkobusch/nonogram (MIT) — human-solvable generation ideas
- https://github.com/TricksterGuy/PicrossPuzzleExporter (MIT) — image-to-puzzle and uniqueness ideas

No third-party level, image, or source file is included. Pictures, Lua, and the icon are original.

## Tests and package

From this directory, with the vendored firmware Lua 5.4:

```
cc -O2 -I <repo>/firmware/lib/Lua/src -o /tmp/nonogram_lua_host tools/lua_host.c <repo>/firmware/lib/Lua/src/*.c
# drop lua.c luac.c liolib.c loslib.c loadlib.c ldblib.c linit.c from that link
/tmp/nonogram_lua_host test_game.lua
python3 tools/make_icon.py
python3 -c "from tools.package import build_m4x; from pathlib import Path; build_m4x(Path('.'), Path('/tmp/nonogram.m4x'))"
```

`tools/lua_host.c` opens the same safe libraries as the device host (no io, os, package, or debug). A passing host run is not a simulator or device pass.
