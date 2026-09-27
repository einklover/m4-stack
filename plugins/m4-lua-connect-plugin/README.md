# 连连看 (com.m4.connect)

Shisen-Sho style tile pairs for the M4 480×800 black/white screen.

## Play

Easy is 4×6 (6 shapes × 4). Medium is 6×8 (12 shapes × 4). Each shape has a distinct line drawing and a Latin label (`O S T P X H D U Z L N A`). Two tiles of the same shape clear when an orthogonal path through empty cells, or the one-cell border around the board, turns at most twice. A third turn does not count.

D-pad moves the cursor. Confirm or a tap selects a tile; a second select on a legal partner clears both and draws the path once (no animation). The same tile again deselects. Buttons: `easy`, `med`, `hint`, `undo`, `mix` (shuffle), `new`, `help`, `exit`. Hardware Back/Home also leaves the activity. Progress is stored in app-scoped `state.csv`.

Hint highlights one connectable pair and does not remove it. Undo restores the last cleared pair (bounded to 48). Shuffle tries to rebuild the remaining tiles into a still-solvable layout; if that search fails the board is left unchanged.

## Level provenance

Layouts are generated in this plugin. They are not copied from KDE kshisen or any other project. kshisen (GPL) was used only as a rules reference: match equal tiles with at most two bends, and allow the path to leave the board. No kshisen source, artwork, or level pack is included.

V1 deals do not search. Easy is 4×6 and medium is 6×8. Every row is filled with horizontal neighbor pairs — (r,1)-(r,2), (r,3)-(r,4), and so on — so each cell is used once. A seeded RNG shuffles only the symbol label of each pair. The cells stay in those slots. There is no backtracking and no reverse-placement search. Because each dealt pair occupies two adjacent cells, either tile can be cleared with its partner at any time, including after other pairs are gone. That is the solvability guarantee for a fresh easy or medium board.

Limits of that guarantee: the opening layout is always side-by-side pairs, so it is easier than a scrambled Shisen-Sho deal. Paths with one or two bends, including the outside border, still work during play after the player clears some tiles. A mid-game shuffle is a separate bounded search and can fail, in which case the board is unchanged. The 100-seed host test clears every opening board by removing those initial horizontal pairs in row order.

## Tests

```
/tmp/m4-lua-host plugins/m4-lua-connect-plugin/test_game.lua
python3 plugins/m4-lua-connect-plugin/tools/package.py
```

`test_game.lua` covers a straight line, a one-bend corner, a border path, a two-bend detour, a three-bend rejection, hint legality, 100 seeds of both sizes, replaying the horizontal pairs, undo, shuffle, and save/load. The host is the existing `/tmp/m4-lua-host` (safe Lua libs only). Packaging is a zip of the manifest allowlist.

## License

Original MIT-compatible project code for this plugin directory. No third-party assets.
