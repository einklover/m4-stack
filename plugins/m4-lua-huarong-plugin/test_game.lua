dofile((arg and arg[0] and arg[0]:match("(.*/)") or "./") .. "game.lua")

local fails = 0
local function eq(a, b, msg)
  if a ~= b then
    fails = fails + 1
    print("FAIL " .. msg .. " got=" .. tostring(a) .. " want=" .. tostring(b))
  end
end

do
  local p = Game.classic()
  eq(#p, 10, "10 pieces")
  eq(Game.is_win(p), false, "start not win")
  local g = Game.grid(p)
  eq(g[1][2], 1, "cao")
  eq(g[5][2], 0, "exit empty")
  eq(g[5][3], 0, "exit empty2")
end

do
  local p = Game.classic()
  -- soldier at 4,2 can move down into 5,2
  local s = Game.find(p, 7)
  eq(s.r, 4, "zu r")
  eq(Game.can_move(p, 7, 1, 0), true, "zu down")
  eq(Game.move(p, 7, 1, 0), true, "zu moved")
  eq(s.r, 5, "zu now r5")
  eq(Game.can_move(p, 1, 1, 0), false, "cao blocked")
end

do
  local p = Game.classic()
  local cao = Game.find(p, 1)
  cao.r, cao.c = 4, 2
  eq(Game.is_win(p), true, "cao at exit")
end

do
  local p = Game.classic()
  local st = { sel = 3, moves = 12, won = false, pieces = p }
  Game.move(p, 7, 1, 0)
  local back = Game.deserialize(Game.serialize(st))
  eq(back.moves, 12, "ser moves")
  eq(Game.find(back.pieces, 7).r, 5, "ser zu")
end

if fails > 0 then
  error(fails .. " failures", 0)
end
print("test_game.lua OK")
