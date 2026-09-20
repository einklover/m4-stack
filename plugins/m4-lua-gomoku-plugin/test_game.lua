dofile((arg and arg[0] and arg[0]:match("(.*/)") or "./") .. "game.lua")

local fails = 0
local function eq(a, b, msg)
  if a ~= b then
    fails = fails + 1
    print("FAIL " .. msg .. " got=" .. tostring(a) .. " want=" .. tostring(b))
  end
end

do
  local b = Game.new_board()
  for c = 1, 5 do b[8][c] = Game.BLACK end
  eq(Game.line_at(b, 8, 3), true, "row5")
end

do
  local b = Game.new_board()
  for r = 3, 7 do b[r][4] = Game.WHITE end
  eq(Game.line_at(b, 5, 4), true, "col5")
end

do
  local b = Game.new_board()
  b[8][8] = Game.BLACK
  local cand = Game.candidates(b)
  eq(#cand >= 8, true, "neighbors")
end

do
  local b = Game.new_board()
  for c = 1, 4 do b[5][c] = Game.CPU end
  local r, c = Game.cpu_move(b)
  eq(r, 5, "cpu win row")
  eq(c == 5 or c == 0, true, "cpu takes 5 or 0")
  -- empty is col 5 (and maybe 0 out of board) — col 5 is in bounds
  eq(c, 5, "cpu completes 5")
end

do
  local b = Game.new_board()
  for c = 2, 5 do b[6][c] = Game.HUMAN end
  local r, c = Game.cpu_move(b)
  eq(r, 6, "block row")
  eq(c == 1 or c == 6, true, "block end")
end

do
  local b = Game.new_board()
  b[1][1] = 1
  local st = {
    cur_r = 2, cur_c = 3, current = 1, active = true, won = "0", board = b,
  }
  local back = Game.deserialize(Game.serialize(st))
  eq(back.board[1][1], 1, "ser stone")
  eq(back.cur_c, 3, "ser cursor")
end

if fails > 0 then
  error(fails .. " failures", 0)
end
print("test_game.lua OK")
