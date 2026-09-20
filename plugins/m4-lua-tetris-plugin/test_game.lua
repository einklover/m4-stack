-- Host-runnable Tetris tests. No gui. Exit 0 on success.

dofile((arg and arg[0] and arg[0]:match("(.*/)") or "./") .. "game.lua")

local fails = 0
local function eq(a, b, msg)
  if a ~= b then
    fails = fails + 1
    print("FAIL " .. msg .. " got=" .. tostring(a) .. " want=" .. tostring(b))
  end
end

do
  local a = Game.new_arena()
  eq(#a, 20, "rows")
  eq(#a[1], 12, "cols")
  local t = Game.create_piece("T")
  eq(Game.collide(a, t, Game.spawn_x(t), 0), false, "spawn T")
end

do
  local a = Game.new_arena()
  local o = Game.create_piece("O")
  local px = Game.spawn_x(o)
  eq(px, 5, "O spawn x")
  local nx, ny, locked = Game.drop(a, o, px, 17)
  eq(locked, false, "O drop at 17")
  nx, ny, locked = Game.drop(a, o, nx, ny)
  eq(locked, true, "O lock at bottom")
  eq(ny, 18, "O y at lock")
end

do
  local a = Game.new_arena()
  local o = Game.create_piece("O")
  local hx, hy = Game.hard_drop(a, o, Game.spawn_x(o), 0)
  eq(hy, 18, "hard drop O to floor")
end

do
  local a = Game.new_arena()
  for x = 1, 12 do a[20][x] = 1 end
  local sc = Game.sweep(a, 0)
  eq(sc, 10, "one line 10")
  eq(a[20][1], 0, "row cleared")
end

do
  local a = Game.new_arena()
  for y = 19, 20 do
    for x = 1, 12 do a[y][x] = 1 end
  end
  local sc = Game.sweep(a, 0)
  eq(sc, 30, "two lines 10+20")
end

do
  local a = Game.new_arena()
  local i = Game.create_piece("I")
  local px, py = Game.spawn_x(i), 0
  local ok
  px, py, ok = Game.try_rotate(a, i, px, py, 1)
  eq(ok, true, "I rotate")
  local rowsum = 0
  for y = 1, 4 do
    local s = 0
    for x = 1, 4 do s = s + i[y][x] end
    if s > rowsum then rowsum = s end
  end
  eq(rowsum, 4, "I horizontal row")
end

do
  local a = Game.new_arena()
  local t = Game.create_piece("T")
  local px, py, ok = Game.try_move(a, t, 0, 0, -1, 0)
  eq(ok, false, "T wall left")
end

do
  local a = Game.new_arena()
  Game.merge(a, Game.create_piece("O"), 0, 18)
  eq(a[19][1] ~= 0, true, "merged")
  local t = Game.create_piece("O")
  eq(Game.collide(a, t, 0, 18), true, "overlap")
end

do
  local a = Game.new_arena()
  a[1][1] = 7
  local m = Game.create_piece("T")
  local s = Game.serialize(a, 40, 100, false, 3, 2, m)
  local st = Game.deserialize(s)
  eq(st.score, 40, "ser score")
  eq(st.best, 100, "ser best")
  eq(st.arena[1][1], 7, "ser cell")
  eq(st.px, 3, "ser px")
  eq(st.matrix[2][2], 7, "ser T")
end

if fails > 0 then
  error(fails .. " failures", 0)
end
print("test_game.lua OK")
