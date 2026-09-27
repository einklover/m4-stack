dofile((arg and arg[0] and arg[0]:match("(.*/)") or "./") .. "game.lua")

local fails = 0
local function eq(a, b, msg)
  if a ~= b then
    fails = fails + 1
    print("FAIL " .. msg .. " got=" .. tostring(a) .. " want=" .. tostring(b))
  end
end

local function truthy(v, msg)
  if not v then
    fails = fails + 1
    print("FAIL " .. msg)
  end
end

do
  for _, n in ipairs({ 3, 4, 5 }) do
    local s = Game.solved(n)
    eq(#s, n * n, "len " .. n)
    eq(s[1], 1, "first " .. n)
    eq(s[n * n], 0, "blank " .. n)
    truthy(Game.is_win(s, n), "solved wins " .. n)
    truthy(Game.solvable(s, n), "solved solvable " .. n)
    eq(Game.slide_index(s, n, 1), false, "corner not adjacent " .. n)
    eq(s[1], 1, "rejected slide keeps tile " .. n)
    eq(Game.move_blank(s, n, 0, 0), false, "zero move " .. n)
    eq(Game.move_blank(s, n, 1, 1), false, "diagonal " .. n)
    eq(Game.move_blank(s, n, 1, 0), false, "blank off bottom " .. n)
    eq(Game.is_win(s, n), true, "still solved " .. n)
  end
end

do
  local b = Game.solved(3)
  truthy(Game.move_blank(b, 3, 0, -1), "blank left")
  eq(b[8], 0, "blank at 8")
  eq(b[9], 8, "tile 8 slid")
  eq(Game.slide_index(b, 3, 9), true, "slide back")
  truthy(Game.is_win(b, 3), "restored win")
  eq(Game.slide_index(b, 3, 9), false, "blank is not a tile")
end

do
  for _, n in ipairs({ 3, 4, 5 }) do
    for seed = 1, 24 do
      local board, moves = Game.scramble(n, seed)
      truthy(board ~= nil, "scramble " .. n .. ":" .. seed)
      truthy(Game.solvable(board, n), "parity " .. n .. ":" .. seed)
      local replay = Game.apply_moves(Game.solved(n), n, moves)
      truthy(Game.boards_equal(replay, board), "replay " .. n .. ":" .. seed)
      local back = Game.apply_moves(board, n, Game.inverse_moves(moves))
      truthy(Game.is_win(back, n), "inverse " .. n .. ":" .. seed)
      local bad = Game.clone(board)
      eq(Game.slide_index(bad, n, 0), false, "index 0 " .. n)
      eq(Game.move_blank(bad, n, 2, 0), false, "step 2 " .. n)
      truthy(Game.boards_equal(bad, board), "invalid keeps " .. n .. ":" .. seed)
    end
  end
end

do
  local st = Game.fresh(4, 7, false)
  truthy(st ~= nil, "fresh")
  eq(st.moves, 0, "fresh moves")
  eq(st.won, false, "fresh not won")
  local before = Game.clone(st.board)
  local far = 1
  if Game.adjacent(4, far, Game.blank_at(st.board)) then far = 2 end
  eq(Game.play_index(st, far), false, "non adjacent play")
  truthy(Game.boards_equal(st.board, before), "no change")
  local blank = Game.blank_at(st.board)
  local br, bc = Game.rc(4, blank)
  local nr, nc = br, bc
  if bc > 1 then nc = bc - 1 else nc = bc + 1 end
  local tile = Game.idx(4, nr, nc)
  truthy(Game.play_index(st, tile), "adjacent play")
  eq(st.moves, 1, "move count")
  truthy(Game.undo(st), "undo")
  truthy(Game.boards_equal(st.board, before), "undo restores")
  eq(st.moves, 0, "undo count")
  eq(Game.undo(st), false, "empty undo")
end

do
  local board, moves = Game.scramble(3, 3, 6)
  local st = {
    n = 3, picture = true, seed = 3, steps = 6, moves = 0, won = false,
    cr = 2, cc = 2, board = Game.clone(board), undo = {},
  }
  for i = #moves, 1, -1 do
    local m = moves[i]
    local blank = Game.blank_at(st.board)
    local br, bc = Game.rc(3, blank)
    local tile = Game.idx(3, br - m[1], bc - m[2])
    truthy(Game.play_index(st, tile), "solve step " .. i)
  end
  truthy(st.won, "won after inverse play")
  eq(Game.play_index(st, 1), false, "no play after win")
  truthy(Game.undo(st), "undo from win")
  eq(st.won, false, "win cleared")
end

do
  local st = Game.fresh(5, 11, true)
  st.cr, st.cc = 2, 3
  local blank = Game.blank_at(st.board)
  local br, bc = Game.rc(5, blank)
  Game.play_index(st, Game.idx(5, br, bc > 1 and bc - 1 or bc + 1))
  local again = Game.deserialize(Game.serialize(st))
  truthy(again ~= nil, "roundtrip")
  eq(again.n, 5, "ser n")
  eq(again.picture, true, "ser picture")
  eq(again.seed, 11, "ser seed")
  eq(again.moves, st.moves, "ser moves")
  eq(again.cr, 2, "ser cr")
  eq(again.cc, 3, "ser cc")
  eq(#again.undo, 1, "ser undo")
  truthy(Game.boards_equal(again.board, st.board), "ser board")
  local empty = Game.fresh(3, 1, false)
  local back = Game.deserialize(Game.serialize(empty))
  eq(#back.undo, 0, "empty undo")
  eq(back.picture, false, "number mode")
  eq(Game.deserialize("nope"), nil, "bad serial")
  eq(Game.deserialize(nil), nil, "nil serial")
end

do
  eq(Game.fragment_ink(4, 0, 0, 0, 8), false, "blank has no fragment")
  local solved_ink = Game.ink_count(4, 8)
  truthy(solved_ink > 0, "diamond has ink")
  local corner = 0
  for v = 0, 7 do
    for u = 0, 7 do
      if Game.fragment_ink(4, 1, u, v, 8) then corner = corner + 1 end
    end
  end
  local center = 0
  for v = 0, 7 do
    for u = 0, 7 do
      if Game.fragment_ink(4, 6, u, v, 8) or Game.fragment_ink(4, 7, u, v, 8)
        or Game.fragment_ink(4, 10, u, v, 8) or Game.fragment_ink(4, 11, u, v, 8) then
        center = center + 1
      end
    end
  end
  truthy(center > corner, "center tiles carry more diamond ink")
  eq(Game.ink_count(3, 6), Game.ink_count(3, 6), "ink stable")
end

if fails > 0 then
  error(fails .. " failures", 0)
end
print("test_game.lua OK")
