# 几A几B (Bulls and Cows)

Standalone M4 Lua plugin. Black/white 480×800. No frame animation.

## Play

The device picks 4 distinct digits. A leading zero is not used (the title says 无前导0). Guess by the on-screen keypad or by moving the focus with up/down/left/right and pressing confirm.

- **A**: digit is correct and in the correct place.
- **B**: digit is in the answer but in another place.
- **C** clears the current entry. **DEL** deletes one digit. **ok** submits.
- **3位 / 4位 / 5位** start a new game of that length. **new** restarts the same length.
- **hint** appends one digit that is **not** in the answer. The answer stays hidden until you win.
- **help** explains the marks. The hardware back key leaves the app (the activity consumes it; Lua does not see it).
- After a win, confirm or **new** starts another secret. Wins, give-ups, and best guess count stay in app storage (`state.csv`, last 12 guesses).

## Tests

```
/tmp/m4-lua-host plugins/m4-lua-bulls-plugin/test_game.lua
```

The host matches the firmware safe libraries (no `io`/`os`). `test_game.lua` fails with `error(n .. " failures")`.

Covered: secret `4271` vs `1234` is `1A2B`; `4271` vs `4271` is `4A0B`; duplicate and leading-zero rejection; same-seed secret; serialize edges; win and restore; unknown keys do not change state.

## Level / number provenance

Secrets are generated in `game.lua` with a 31-bit LCG (`1103515245`, `12345`) and a Fisher-Yates shuffle of `0`–`9`, then a leading zero is moved out of the first place. No external puzzle pack is shipped.

The A/B definition matches the common bulls-and-cows rule. Test vectors were checked against that rule. [magiclen/bulls-and-cows](https://github.com/magiclen/bulls-and-cows) (MIT) was used only as an algorithm reference; none of its code or data is included. This plugin’s Lua is original.

## Package

`tools/package.py` is the same allowlist packager as the 2048 plugin (`manifest.files` only). `tools/lua_host.c` is the local test host and is not in the `.m4x`.

## License

Plugin source in this directory is original project code, same terms as the surrounding M4 tree. No third-party asset is bundled. The icon is a generated 62×64 1-bit BMP.
