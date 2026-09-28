# 推箱子 (com.m4.sokoban)

Black-and-white Sokoban for the 480×800 M4 e-ink screen. The board uses custom high-contrast 1-bit player, crate, target and wall silhouettes; a compact three-action toolbar replaces eight crowded text buttons. One static frame per state change, no animations or runtime puzzle solver. Interface layout draws inspiration from the KOReader e-reader Sokoban plugin, but all M4 drawing/control code is independently written.

## Play and controls

- The large central board is the main focus. A filled pixel pawn is **you**; an X-outlined square is a **crate**; a square with a bullseye is a **goal**. A crate with a solid center marker is already on a goal. Black blocks are walls.
- Use hardware direction keys or large touch arrows to **walk and push**. Crates can be pushed only one at a time, never pulled.
- Tap **any reachable empty floor tile** for bounded shortest-path walking. This never auto-pushes a crate or runs the solver on the device. Physical Confirm acts as Undo during play.
- The three large actions are **撤销 Undo**, **重开 Restart**, **选关 Levels**. A small **?** at the upper right opens a legend and controls page. System Back is handled by the host to leave the plugin.
- The level picker displays **three named chapters** with eight 2×4 level cards per page. Tap a chapter tab or use the chapter arrows. Finished cards show the saved best result. Physical D-pad and Confirm work here as well.
- After a win, the board stays visible and the two unambiguous actions are **下一关 Next** and **选择关卡 Levels**. On the final level, Next returns to the completed chapter.
- State is saved as versioned app data `microban_progress.csv`. The original `progress.csv` belongs to old V1 maps and is never overwritten or misinterpreted.

The provided `tools/render_ui_trace.lua` and `tools/render_ui_preview.py` make reproducible **host-simulated** black/white 480×800 PNG screens of the actual Lua GUI draw calls. These are preview frames, not QEMU or real-device e-ink output. Their mock font rasterization uses a locally installed CJK font and can differ from actual firmware.

## Tests

`test_ui.lua` verifies 480×800 draw-call bounds, discoverable help, selected chapter cards, win-next, V1 save isolation and bounded no-push floor tapping. `test_game.lua` checks the XSB parser (`# $ . @ * +`), walk versus push, pushes into a wall or a second crate, undo, corner deadlock, win, progress round-trip, and a bounded BFS proof that every bundled level is solvable. The solver runs only in the host test.

```
cc -O2 -DLUA_COMPAT_5_3 -DLUA_32BITS=1 -I firmware/lib/Lua/src \
  plugins/m4-lua-sokoban-plugin/tools/lua_host.c firmware/lib/Lua/src/*.c \
  -o /tmp/sokoban_lua_host
/tmp/sokoban_lua_host plugins/m4-lua-sokoban-plugin/test_game.lua
/tmp/sokoban_lua_host plugins/m4-lua-sokoban-plugin/test_ui.lua
python3 plugins/m4-lua-sokoban-plugin/tools/level_audit.py --quality-v2
python3 plugins/m4-lua-sokoban-plugin/tools/render_ui_preview.py --host /tmp/sokoban_lua_host --out /tmp/sokoban-ui-preview
python3 plugins/m4-lua-sokoban-plugin/tools/make_icon.py
python3 plugins/m4-lua-sokoban-plugin/tools/package.py
```

`tools/` is not in `manifest.files`, so the `.m4x` contains `manifest.json`, `main.lua`, `game.lua`, and `icon_home.bmp`.

## Levels and license

This version uses 24 selected puzzles from **Microban by David W. Skinner**, revised April 2000, with author-level IDs preserved in `Game.MICROBAN_IDS`. They are not original M4-created levels. Source: [OMerkel's attributed Microban text](https://github.com/OMerkel/Sokoban/blob/master/3rdParty/Levels/Microban.txt).

David W. Skinner’s Microban set is widely described as public domain, including by the KOReader plugin README, but the source header identifies Skinner as copyright holder without an explicit license grant. The maps are staged on an isolated development branch. Verify redistribution permissions before a public app-store release. [kbarni/sokoban.koplugin](https://github.com/kbarni/sokoban.koplugin) is GPL-3.0; none of its code or assets are included. XSB character meanings are the usual Sokoban notation.

`icon_home.bmp` is a program-generated 62×64, 1-bit, uncompressed BMP. Palette index 0 is black and index 1 is white, matching `m4-lua-huarong-plugin`.
