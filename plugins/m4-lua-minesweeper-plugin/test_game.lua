dofile((arg and arg[0] and arg[0]:match("(.*/)") or "./") .. "game.lua")

local fails = 0
local function eq(a, b, msg)
  if a ~= b then
    fails = fails + 1
    print("FAIL " .. msg .. " got=" .. tostring(a) .. " want=" .. tostring(b))
  end
end

do
  local d = Game.DIFF.easy
  eq(d.rows, 10, "easy rows")
  eq(d.mines, 10, "easy mines")
  eq(Game.DIFF.medium.mines, 40, "med mines")
  eq(Game.DIFF.hard.cols, 20, "hard cols")
end

do
  local rows, cols, mines = 5, 5, 5
  local g = Game.new_grid(rows, cols)
  local n = 0
  local function rng(max)
    n = n + 1
    return ((n - 1) % max) + 1
  end
  Game.place_mines(g, rows, cols, mines, 3, 3, rng)
  eq(g[3][3].mine, false, "first cell safe")
  local mc = 0
  for r = 1, rows do
    for c = 1, cols do
      if g[r][c].mine then mc = mc + 1 end
    end
  end
  eq(mc, mines, "mine count")
end

do
  local g = Game.new_grid(3, 3)
  g[1][1].mine = true
  Game.count_neighbors(g, 3, 3)
  eq(g[1][2].n, 1, "n right")
  eq(g[2][2].n, 1, "n diag")
  eq(g[3][3].n, 0, "n far")
end

do
  local g = Game.new_grid(3, 3)
  Game.count_neighbors(g, 3, 3)
  eq(Game.reveal(g, 3, 3, 2, 2), "ok", "flood")
  eq(Game.shown_count(g, 3, 3), 9, "all shown")
  eq(Game.is_win(g, 3, 3, 0), true, "win empty")
end

do
  local g = Game.new_grid(2, 2)
  g[1][1].mine = true
  eq(Game.reveal(g, 2, 2, 1, 1), "dead", "hit mine")
end

do
  local g = Game.new_grid(2, 2)
  eq(Game.toggle_flag(g, 1, 1), true, "flag")
  eq(g[1][1].flag, true, "flagged")
  eq(Game.reveal(g, 2, 2, 1, 1), "noop", "no reveal flag")
  Game.toggle_flag(g, 1, 1)
  eq(g[1][1].flag, false, "unflag")
end

do
  local g = Game.new_grid(2, 2)
  g[1][1].mine = true
  g[1][1].n = 0
  g[1][2].n = 1
  local st = {
    diff = "easy", rows = 2, cols = 2, mines = 1,
    first = false, over = false, won = false, mode = "dig",
    cursor_r = 1, cursor_c = 2, grid = g,
  }
  local s = Game.serialize(st)
  local d = Game.deserialize(s)
  eq(d.grid[1][1].mine, true, "ser mine")
  eq(d.grid[1][2].n, 1, "ser n")
  eq(d.cursor_c, 2, "ser cursor")
end

if fails > 0 then
  error(fails .. " failures", 0)
end
print("test_game.lua OK")
