# 推箱子 (com.m4.sokoban)

Black-and-white Sokoban for the 480×800 M4 e-ink screen. One static frame per change. No animation and no search on the device.

## Play

Push every crate onto a goal. Walls block movement. A push moves one crate into an empty floor or goal. Two crates in a row do not chain.

- Direction keys, the on-screen 左/上/下/右 buttons, or a tap on a cell next to the player: one step.
- 确认 key or 撤销: undo, up to 80 snapshots.
- 重开: restart the current level.
- 选关: pick a level. 帮助: controls. 返回 leaves that screen.
- The system back key is not consumed during play, so the host can leave the plugin.
- A crate on a non-goal corner (two orthogonal walls) is marked 死角. Undo or 重开.
- Clearing a level stores the best move count, then the best push count, in app data `progress.csv`.

## Tests

`test_game.lua` checks the XSB parser (`# $ . @ * +`), walk versus push, pushes into a wall or a second crate, undo, corner deadlock, win, progress round-trip, and a bounded BFS proof that every bundled level is solvable. The solver runs only in the host test.

```
cc -O2 -DLUA_COMPAT_5_3 -DLUA_32BITS=1 -I firmware/lib/Lua/src \
  plugins/m4-lua-sokoban-plugin/tools/lua_host.c firmware/lib/Lua/src/*.c \
  -o /tmp/sokoban_lua_host
/tmp/sokoban_lua_host plugins/m4-lua-sokoban-plugin/test_game.lua
python3 plugins/m4-lua-sokoban-plugin/tools/make_icon.py
python3 plugins/m4-lua-sokoban-plugin/tools/package.py
```

`tools/` is not in `manifest.files`, so the `.m4x` contains `manifest.json`, `main.lua`, `game.lua`, and `icon_home.bmp`.

## Levels and license

The 12 maps are original small boards written for this plugin. They are not copies of Microban or of the sample set in [rrthomas/sokoban](https://github.com/rrthomas/sokoban).

David W. Skinner’s Microban set is widely described as public domain, including by the KOReader plugin README, but this package does not ship those texts. [kbarni/sokoban.koplugin](https://github.com/kbarni/sokoban.koplugin) is GPL-3.0; none of its code or assets are included. XSB character meanings are the usual Sokoban notation.

`icon_home.bmp` is a program-generated 62×64, 1-bit, uncompressed BMP. Palette index 0 is black and index 1 is white, matching `m4-lua-huarong-plugin`.
