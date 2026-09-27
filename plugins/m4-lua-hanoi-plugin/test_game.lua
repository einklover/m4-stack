dofile((arg and arg[0] and arg[0]:match("(.*/)") or "./") .. "game.lua")

local fails = 0
local function eq(a, b, msg)
  if a ~= b then
    fails = fails + 1
    print("FAIL " .. msg .. " got=" .. tostring(a) .. " want=" .. tostring(b))
  end
end

do
  eq(Game.min_moves(3), 7, "min 3")
  eq(Game.min_moves(4), 15, "min 4")
end

do
  local s = Game.new_stacks(3)
  eq(#s[1], 3, "start left")
  eq(s[1][1], 3, "biggest at base")
  eq(s[1][3], 1, "smallest on top")
  eq(Game.can_move(s, 1, 2), true, "to empty")
  eq(Game.move(s, 1, 2), true, "move 1")
  eq(Game.top(s[2]), 1, "on peg2")
  eq(Game.can_move(s, 1, 2), false, "bigger on smaller")
  eq(Game.move(s, 1, 3), true, "2 to 3")
  eq(Game.move(s, 2, 3), true, "1 onto 2")
  eq(Game.is_win(s, 3), false, "not yet")
end

do
  local s = { {}, {}, { 3, 2, 1 } }
  eq(Game.is_win(s, 3), true, "all on peg 3")
end

do
  local st = {
    n = 3, moves = 4, sel = 2, cursor = 3, won = false,
    stacks = { { 3 }, { 1 }, { 2 } },
  }
  local back = Game.deserialize(Game.serialize(st))
  eq(back.n, 3, "ser n")
  eq(back.moves, 4, "ser moves")
  eq(back.stacks[2][1], 1, "ser peg2")
  eq(back.cursor, 3, "ser cursor")
end

if fails > 0 then
  error(fails .. " failures", 0)
end
print("test_game.lua OK")
