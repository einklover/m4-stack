-- Host-runnable 2048 logic tests. No gui. Exit 0 on success.

dofile((arg and arg[0] and arg[0]:match("(.*/)") or "./") .. "game.lua")

local fails = 0
local function eq(a, b, msg)
  if a ~= b then
    fails = fails + 1
    print("FAIL " .. msg .. " got=" .. tostring(a) .. " want=" .. tostring(b))
  end
end

local function board_eq(a, b, msg)
  for r = 1, 4 do
    for c = 1, 4 do
      if a[r][c] ~= b[r][c] then
        fails = fails + 1
        print("FAIL " .. msg .. " at " .. r .. "," .. c .. " " .. a[r][c] .. "~=" .. b[r][c])
        return
      end
    end
  end
end

local function set_row(vals)
  local row = {}
  for i = 1, 4 do row[i] = vals[i] end
  return row
end

-- slide/merge
do
  local out, sc, mv = Game.slide_row_left(set_row({2, 2, 0, 0}))
  eq(out[1], 4, "merge 2+2")
  eq(out[2], 0, "merge rest 2")
  eq(sc, 4, "merge score")
  eq(mv, true, "merge moved")
end
do
  local out, sc, mv = Game.slide_row_left(set_row({2, 2, 2, 2}))
  eq(out[1], 4, "double merge a")
  eq(out[2], 4, "double merge b")
  eq(out[3], 0, "double merge pad")
  eq(sc, 8, "double merge score")
  eq(mv, true, "double merge moved")
end
do
  local out, sc, mv = Game.slide_row_left(set_row({2, 0, 2, 0}))
  eq(out[1], 4, "gap merge")
  eq(sc, 4, "gap score")
  eq(mv, true, "gap moved")
end
do
  local out, sc, mv = Game.slide_row_left(set_row({4, 2, 0, 0}))
  eq(out[1], 4, "no merge keep")
  eq(out[2], 2, "no merge 2")
  eq(sc, 0, "no merge score")
  eq(mv, false, "already packed no move")
end
do
  local out, _, mv = Game.slide_row_left(set_row({0, 0, 2, 4}))
  eq(out[1], 2, "slide compact")
  eq(out[2], 4, "slide compact 2")
  eq(mv, true, "slide moved")
end
do
  -- 2048 rule: a tile merges at most once per move
  local out, sc = Game.slide_row_left(set_row({2, 2, 4, 0}))
  eq(out[1], 4, "no cascade 1")
  eq(out[2], 4, "no cascade 2")
  eq(out[3], 0, "no cascade 3")
  eq(sc, 4, "no cascade score (only 2+2)")
end

-- directional move
do
  local b = Game.new_board()
  b[1][1] = 2
  b[1][2] = 2
  local nb, sc, mv = Game.move(b, "left")
  eq(nb[1][1], 4, "left merge")
  eq(sc, 4, "left score")
  eq(mv, true, "left moved")
end
do
  local b = Game.new_board()
  b[1][3] = 2
  b[1][4] = 2
  local nb = select(1, Game.move(b, "right"))
  eq(nb[1][4], 4, "right merge")
end
do
  local b = Game.new_board()
  b[1][1] = 2
  b[2][1] = 2
  local nb = select(1, Game.move(b, "up"))
  eq(nb[1][1], 4, "up merge")
end
do
  local b = Game.new_board()
  b[3][2] = 2
  b[4][2] = 2
  local nb = select(1, Game.move(b, "down"))
  eq(nb[4][2], 4, "down merge")
end
do
  local b = Game.new_board()
  b[1][1] = 2
  local nb, _, mv = Game.move(b, "left")
  eq(mv, false, "blocked left no move")
  board_eq(nb, b, "blocked left board")
end

-- spawn fills empty only, 2 or 4
do
  local b = Game.new_board()
  local function rng()
    return 0.5 -- >= 0.1 → spawn 2
  end
  local function rng_int(max)
    eq(max, 16, "empty count at start")
    return 1
  end
  Game.spawn(b, rng, rng_int)
  eq(b[1][1], 2, "spawn 2 in first empty")
  local nempty = #Game.empty_cells(b)
  eq(nempty, 15, "one cell filled")
end
do
  local b = Game.new_board()
  for r = 1, 4 do
    for c = 1, 4 do b[r][c] = 2 end
  end
  eq(Game.spawn(b, function() return 0 end), false, "full board no spawn")
end
do
  -- 4-spawn when rng >= 0.1 is false... SPAWN_FOUR_CHANCE = 0.1 so u < 0.1 -> 4
  local b = Game.new_board()
  Game.spawn(b, function() return 0.0 end, function() return 1 end)
  -- rng for value is 0.0 < 0.1 -> 4
  eq(b[1][1], 4, "spawn 4 when rng < 0.1")
end

-- game over: full, no adjacent equals
do
  local b = Game.new_board()
  local v = 2
  for r = 1, 4 do
    for c = 1, 4 do
      b[r][c] = v
      v = v * 2
    end
  end
  eq(Game.is_game_over(b), true, "unique full is over")
end
do
  local b = Game.new_board()
  for r = 1, 4 do
    for c = 1, 4 do b[r][c] = ((r + c) % 2 == 0) and 2 or 4 end
  end
  -- checkerboard 2/4 still has no equal neighbors? 2 and 4 alternate - no merge, full -> over
  eq(Game.is_game_over(b), true, "checkerboard over")
end
do
  local b = Game.new_board()
  for r = 1, 4 do
    for c = 1, 4 do b[r][c] = 2 end
  end
  eq(Game.is_game_over(b), false, "full but merges remain")
end
do
  local b = Game.new_board()
  b[1][1] = 2
  eq(Game.is_game_over(b), false, "empty cells not over")
end

-- serialize roundtrip
do
  local b = Game.new_board()
  b[2][3] = 8
  local s = Game.serialize(b, 12, 99, false)
  local st = Game.deserialize(s)
  eq(st.score, 12, "ser score")
  eq(st.best, 99, "ser best")
  eq(st.game_over, false, "ser over")
  eq(st.board[2][3], 8, "ser tile")
end

if fails > 0 then
  error(fails .. " failures", 0)
end
print("test_game.lua OK")
