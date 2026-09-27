# 滑动拼图 (`com.m4.sliding`)

Black-and-white 480×800 e-ink sliding puzzle. Logic is original Lua. Scrambles are random walks of legal blank moves from the solved board, so every deal is solvable. References for the usual parity rule (not copied code): [imshubhamsingh/15-puzzle](https://github.com/imshubhamsingh/15-puzzle) (MIT) and [Firnox/SlidingPuzzle](https://github.com/Firnox/SlidingPuzzle) (Unlicense). No third-party levels or pictures are shipped.

## Play

- 3×3 is the 8-puzzle, 4×4 is the 15-puzzle, 5×5 is the 24-puzzle.
- Number mode paints the tile id. Picture mode paints a geometric diamond fragment of that tile's home cell. There are no photos.
- Direction keys move the cursor. Confirm slides the cursor tile when it shares an edge with the blank. On a win, confirm restarts the same deal.
- A tap on a tile next to the blank slides it. A tap elsewhere only moves the cursor.
- 重开 rebuilds the same seed. 新局 advances the seed. 撤销 undoes one player move. 说明 opens the control card.
- The device back key leaves the app. Progress is `state.csv` in app data (size, picture flag, seed, step count, move count, cursor, board, undo).

## Tests

From the repo root, with the existing safe Lua 5.4 host:

```sh
/tmp/m4-lua-host plugins/m4-lua-sliding-plugin/test_game.lua
```

`tools/lua_host.c` matches that host (base, table, string, math, utf8, coroutine). `tools/package.py` builds an allowlisted `.m4x`.

The logic tests check solvability parity, replay of the scramble, the inverse walk back to solved, rejected moves, undo, win, serialize/deserialize, and seeds 1–24 on 3×3, 4×4, and 5×5.

## License

Plugin code in this directory is original project code. No upstream puzzle assets are included.
