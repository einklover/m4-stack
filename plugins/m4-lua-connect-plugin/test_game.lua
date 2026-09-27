dofile((arg and arg[0] and arg[0]:match("(.*/)") or "./") .. "game.lua")

local fails = 0
local function eq(a, b, msg)
  if a ~= b then
    fails = fails + 1
    print("FAIL " .. msg .. " got=" .. tostring(a) .. " want=" .. tostring(b))
  end
end

local function board_of(rows, cols, rows_s)
  local b = Game.blank(rows, cols)
  for r = 1, rows do
    local line = rows_s[r]
    for c = 1, cols do
      local ch = line:sub(c, c)
      if ch ~= "." and ch ~= "#" then
        b[r][c] = string.byte(ch) - string.byte("0")
      elseif ch == "#" then
        b[r][c] = 9
      end
    end
  end
  return b
end

do
  local b = board_of(1, 4, { "1001" })
  local p = Game.route(1, 4, b, 1, 1, 1, 4)
  eq(p ~= nil, true, "straight exists")
  eq(Game.bends(p), 0, "straight bends")
  eq(Game.can_link(1, 4, b, 1, 1, 1, 4), true, "straight link")
end

do
  local b = board_of(3, 3, { "1..", "...", "..1" })
  local p = Game.route(3, 3, b, 1, 1, 3, 3)
  eq(p ~= nil, true, "corner exists")
  eq(Game.bends(p) <= 2, true, "corner within 2")
  eq(Game.bends(p) >= 1, true, "corner not straight")
end

do
  -- Internal row is blocked; the outside border still joins them in 2 bends.
  local b = board_of(3, 4, {
    "1##.",
    "###.",
    "###1",
  })
  local p = Game.route(3, 4, b, 1, 1, 3, 4)
  eq(p ~= nil, true, "perimeter exists")
  eq(Game.bends(p) <= 2, true, "perimeter bends")
  local outside = false
  for i = 1, #p do
    local r, c = p[i][1], p[i][2]
    if r < 1 or c < 1 or r > 3 or c > 4 then outside = true end
  end
  eq(outside, true, "perimeter uses border")
end

do
  local b = board_of(1, 3, { "191" })
  local p = Game.route(1, 3, b, 1, 1, 1, 3)
  eq(p ~= nil, true, "two bends around block")
  eq(Game.bends(p), 2, "exactly two bends")
end

do
  -- Only corridor needs three turns. Border exits are walled.
  local b = board_of(5, 5, {
    "#####",
    "#1###",
    "#00##",
    "##01#",
    "#####",
  })
  -- digits: 9 is wall via '#', 0 empty via '.', wait board_of '#' -> 9, '.' -> 0, '1' -> 1
  -- I used 0 as digit which is byte('0')-byte('0') = 0. Good.
  -- Fix: row3 "100##" would be tiles. Pattern uses 0 chars as empty. '#' walls.
  local p = Game.route(5, 5, b, 2, 2, 4, 4)
  eq(p, nil, "three bends rejected")
  eq(Game.can_link(5, 5, b, 2, 2, 4, 4), false, "three bends no link")
end

do
  local b = board_of(2, 2, { "12", "12" })
  eq(Game.can_link(2, 2, b, 1, 1, 1, 2), false, "different kinds")
  eq(Game.hint(2, 2, b) ~= nil, true, "hint finds a pair")
  local r1, c1, r2, c2 = Game.hint(2, 2, b)
  eq(b[r1][c1] == b[r2][c2], true, "hint same kind")
  eq(Game.can_link(2, 2, b, r1, c1, r2, c2), true, "hint legal")
end

do
  local varied = false
  local first = nil
  for _, level in ipairs({ "easy", "medium" }) do
    for seed = 1, 100 do
      local st = Game.new_game(level, seed)
      eq(st ~= nil, true, "gen " .. level .. " " .. seed)
      if st then
        eq(st.fallback, false, "v1 deal " .. level .. " " .. seed)
        eq(Game.apply_proof(st.rows, st.cols, st.board, st.proof), true,
          "proof " .. level .. " " .. seed)
        local again = Game.new_game(level, seed)
        eq(Game.serialize(again) == Game.serialize(st), true, "seed stable " .. level .. " " .. seed)
        if first == nil then
          first = Game.serialize(st)
        elseif Game.serialize(st) ~= first then
          varied = true
        end
        local counts = Game.counts(st.board)
        for k = 1, st.kinds do
          eq((counts[k] or 0) % 2, 0, "even kind " .. level .. " " .. k)
          eq((counts[k] or 0) > 0, true, "kind used " .. level .. " " .. k)
        end
        local b = Game.copy_board(st.board)
        local cleared = true
        for r = 1, st.rows do
          for c = 1, st.cols, 2 do
            if b[r][c] == 0 or b[r][c] ~= b[r][c + 1]
                or not Game.can_link(st.rows, st.cols, b, r, c, r, c + 1) then
              cleared = false
            else
              b[r][c] = 0
              b[r][c + 1] = 0
            end
          end
        end
        eq(cleared, true, "horizontal replay " .. level .. " " .. seed)
        eq(Game.remaining(b), 0, "horizontal clear " .. level .. " " .. seed)
        eq(Game.hint(st.rows, st.cols, st.board) ~= nil, true, "opening hint " .. level .. " " .. seed)
      end
    end
  end
  eq(varied, true, "seeds change pair labels")
end

do
  local st = Game.new_game("easy", 3)
  local r1, c1, r2, c2 = Game.hint(st.rows, st.cols, st.board)
  eq(Game.can_link(st.rows, st.cols, st.board, r1, c1, r2, c2), true, "hint before")
  local how = Game.pick(st, r1, c1)
  eq(how, "select", "first pick")
  how = Game.pick(st, r2, c2)
  eq(how, "match", "hint pair matches")
  eq(#st.undo, 1, "undo recorded")
  eq(Game.undo(st), true, "undo ok")
  eq(st.board[r1][c1] ~= 0, true, "restored")
  eq(Game.can_link(st.rows, st.cols, st.board, r1, c1, r2, c2), true, "still linked")
end

do
  local st = Game.new_game("easy", 5)
  local before = Game.remaining(st.board)
  eq(Game.shuffle(st, 9), true, "shuffle")
  eq(Game.remaining(st.board), before, "shuffle keeps tiles")
  eq(st.shuffles, 1, "shuffle count")
  eq(Game.apply_proof(st.rows, st.cols, st.board, st.proof), true, "shuffle proof")
  local r1, c1, r2, c2 = Game.hint(st.rows, st.cols, st.board)
  eq(Game.can_link(st.rows, st.cols, st.board, r1, c1, r2, c2), true, "hint after shuffle")
end

do
  local st = Game.new_game("medium", 8)
  Game.pick(st, 1, 1)
  local raw = Game.serialize(st)
  local back = Game.deserialize(raw)
  eq(back.level, "medium", "ser level")
  eq(back.seed, 8, "ser seed")
  eq(back.board[1][1], st.board[1][1], "ser tile")
  eq(back.sel[1], st.sel[1], "ser sel")
  Game.clear_pair(st, Game.hint(st.rows, st.cols, st.board))
  local raw2 = Game.serialize(st)
  local back2 = Game.deserialize(raw2)
  eq(#back2.undo, 1, "ser undo")
  eq(back2.board[back2.undo[1][1]][back2.undo[1][2]], 0, "ser cleared")
end

if fails > 0 then
  error(fails .. " failures", 0)
end
print("test_game.lua OK")
