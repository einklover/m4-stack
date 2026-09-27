dofile((arg and arg[0] and arg[0]:match("(.*/)") or "./") .. "game.lua")

local fails = 0
local function eq(a, b, msg)
  if a ~= b then
    fails = fails + 1
    print("FAIL " .. msg .. " got=" .. tostring(a) .. " want=" .. tostring(b))
  end
end

local function fail(msg)
  fails = fails + 1
  print("FAIL " .. msg)
end

do
  local st = Game.parse("#@$.#\n")
  eq(st.pr, 1, "player r")
  eq(st.pc, 2, "player c")
  eq(st.crates[Game.key(1, 3)], true, "crate")
  eq(st.goals[Game.key(1, 4)], true, "goal")
  eq(Game.step(st, 0, 1), "push", "push onto goal")
  eq(Game.won(st), true, "win one crate")
  eq(st.moves, 1, "moves")
  eq(st.pushes, 1, "pushes")
end

do
  local st = Game.parse("#+$*#\n")
  eq(st.pr, 1, "+ row")
  eq(st.goals[Game.key(1, 2)], true, "+ is goal")
  eq(st.crates[Game.key(1, 3)], true, "dollar")
  eq(st.crates[Game.key(1, 4)], true, "star crate")
  eq(st.goals[Game.key(1, 4)], true, "star goal")
  eq(Game.won(st), false, "loose crate")
end

do
  local st = Game.parse("######\n#@$###\n######\n")
  eq(Game.step(st, 0, 1), nil, "push into wall")
  eq(st.moves, 0, "wall no move")
  eq(st.pc, 2, "player stayed")
  eq(st.crates[Game.key(2, 3)], true, "crate stayed")
end

do
  local st = Game.parse("######\n#@$$ #\n######\n")
  eq(Game.step(st, 0, 1), nil, "no chain push")
  eq(st.crates[Game.key(2, 3)], true, "first crate stays")
  eq(st.crates[Game.key(2, 4)], true, "second crate stays")
end

do
  local st = Game.parse("#####\n#@ $.#\n#####\n")
  eq(Game.step(st, 0, 1), "walk", "walk")
  eq(st.pushes, 0, "walk no push")
  eq(Game.step(st, 0, 1), "push", "then push")
  eq(Game.won(st), true, "walk then win")
end

do
  local st = Game.attach_dead(Game.parse([[
#####
#   #
#@$.#
#   #
#####
]]))
  eq(st.dead[Game.key(2, 2)], true, "top-left corner dead")
  eq(st.dead[Game.key(3, 4)], nil, "goal is not dead")
  local before = Game.snapshot(st)
  eq(Game.step(st, 0, 1), "push", "push to goal")
  Game.restore(st, before)
  eq(st.pc, before.pc, "undo player")
  eq(st.crates[Game.key(3, 3)], true, "undo crate")
  eq(st.moves, 0, "undo moves")
  -- crate (3,3) -> up (2,3) -> left into corner (2,2)
  eq(Game.play(st, "drurul"), true, "route to corner")
  eq(st.crates[Game.key(2, 2)], true, "crate in corner")
  eq(Game.deadlocked(st), true, "corner deadlock")
  eq(Game.won(st), false, "dead is not win")
end

do
  local p = Game.blank_progress()
  Game.note_clear(p, 1, 5, 2)
  Game.note_clear(p, 1, 9, 1)
  eq(p.best_m[1], 5, "best keeps fewer moves")
  Game.note_clear(p, 1, 5, 1)
  eq(p.best_p[1], 1, "tie breaks on pushes")
  p.level = 3
  local back = Game.deserialize_progress(Game.serialize_progress(p))
  eq(back.level, 3, "progress level")
  eq(back.best_m[1], 5, "progress moves")
  eq(back.best_p[1], 1, "progress pushes")
  eq(back.cleared[1], 1, "progress clear")
  eq(back.cleared[2], 0, "uncleared")
end

local function solve(st, cap)
  local function sig(s)
    return tostring(s.pr) .. ":" .. tostring(s.pc) .. "|" .. table.concat(Game.crate_list(s), ";")
  end
  local q, qh = { Game.snapshot(st) }, 1
  local seen = { [sig(st)] = "" }
  local dirs = { u = {-1, 0}, d = {1, 0}, l = {0, -1}, r = {0, 1} }
  local order = { "u", "d", "l", "r" }
  local n = 0
  while qh <= #q do
    n = n + 1
    if n > cap then return nil, "cap" end
    local snap = q[qh]
    qh = qh + 1
    local cur = Game.load(st.index)
    Game.restore(cur, snap)
    cur.dead = st.dead
    if Game.won(cur) then return seen[sig(cur)] end
    if not Game.deadlocked(cur) then
      for i = 1, 4 do
        local name = order[i]
        local d = dirs[name]
        local nxt = Game.load(st.index)
        Game.restore(nxt, snap)
        nxt.dead = st.dead
        if Game.step(nxt, d[1], d[2]) then
          local k = sig(nxt)
          if not seen[k] then
            seen[k] = seen[sig(cur)] .. name
            q[#q + 1] = Game.snapshot(nxt)
          end
        end
      end
    end
  end
  return nil, "none"
end

do
  eq(#Game.LEVELS >= 12, true, "at least 12 levels")
  for i = 1, #Game.LEVELS do
    local st = Game.load(i)
    if not st then
      fail("parse level " .. tostring(i))
    else
      local sol, why = solve(st, 200000)
      if not sol then
        fail("unsolved level " .. tostring(i) .. " " .. tostring(why))
      else
        local again = Game.load(i)
        eq(Game.play(again, sol), true, "replay " .. tostring(i))
        eq(Game.won(again), true, "replay win " .. tostring(i))
        print("level " .. tostring(i) .. " sol=" .. sol .. " moves=" .. tostring(again.moves))
      end
    end
  end
end

if fails > 0 then
  error(fails .. " failures", 0)
end
print("test_game.lua OK")
