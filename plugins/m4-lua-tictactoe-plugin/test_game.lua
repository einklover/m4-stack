-- Host-runnable Tic-Tac-Toe tests. No gui. Exit 0 on success.

dofile((arg and arg[0] and arg[0]:match("(.*/)") or "./") .. "game.lua")

local fails = 0
local function eq(a, b, msg)
  if a ~= b then
    fails = fails + 1
    print("FAIL " .. msg .. " got=" .. tostring(a) .. " want=" .. tostring(b))
  end
end

local function line_eq(got, want, msg)
  if not got then
    fails = fails + 1
    print("FAIL " .. msg .. " got=nil")
    return
  end
  for i = 1, 3 do
    if got[i] ~= want[i] then
      fails = fails + 1
      print("FAIL " .. msg .. " line mismatch")
      return
    end
  end
end

do
  local b = Game.new_board()
  eq(Game.check_win(b) == nil, true, "empty no win")
  eq(Game.is_draw(b), false, "empty not draw")
end

do
  local b = Game.new_board()
  b[1], b[2], b[3] = "X", "X", "X"
  line_eq(Game.check_win(b), {1, 2, 3}, "row win")
end

do
  local b = Game.new_board()
  b[1], b[5], b[9] = "O", "O", "O"
  line_eq(Game.check_win(b), {1, 5, 9}, "diag win")
end

do
  local b = { "X", "O", "X", "X", "O", "O", "O", "X", "X" }
  eq(Game.check_win(b) == nil, true, "full no line")
  eq(Game.is_draw(b), true, "draw")
end

do
  local b = Game.new_board()
  eq(Game.can_place(b, 5, true, Game.HUMAN), true, "can center")
  eq(Game.can_place(b, 5, true, Game.CPU), false, "not during cpu")
  Game.place(b, 5, Game.HUMAN)
  eq(Game.can_place(b, 5, true, Game.HUMAN), false, "occupied")
end

do
  local b = Game.new_board()
  -- X about to win on 1-2; O to play should block 3.
  b[1], b[2] = "X", "X"
  b[5] = "O"
  eq(Game.best_move(b), 3, "hard blocks row")
end

do
  local b = Game.new_board()
  b[1], b[5] = "O", "O"
  -- X elsewhere so O can win 9
  b[3] = "X"
  eq(Game.best_move(b), 9, "hard takes win")
end

do
  local b = Game.new_board()
  b[1] = "X"
  eq(Game.best_move(b), 5, "opening takes center")
end

do
  local b = Game.new_board()
  local seen = {}
  local n = 0
  local function rng_int(max)
    n = n + 1
    return ((n - 1) % max) + 1
  end
  for _ = 1, 9 do
    local i = Game.cpu_move(b, "easy", rng_int)
    eq(b[i] == nil, true, "easy empty")
    seen[i] = true
    b[i] = "X"
  end
  eq(Game.cpu_move(b, "easy", rng_int), -1, "easy full")
end

do
  local b = Game.new_board()
  b[1] = "X"
  local s = Game.serialize(b, "hard", Game.HUMAN, true, 5)
  local st = Game.deserialize(s)
  eq(st.difficulty, "hard", "ser diff")
  eq(st.board[1], "X", "ser x")
  eq(st.board[2] == nil, true, "ser empty")
  eq(st.cursor, 5, "ser cursor")
end

if fails > 0 then
  error(fails .. " failures", 0)
end
print("test_game.lua OK")
