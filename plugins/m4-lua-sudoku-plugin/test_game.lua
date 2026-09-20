dofile((arg and arg[0] and arg[0]:match("(.*/)") or "./") .. "game.lua")

local fails = 0
local function eq(a, b, msg)
  if a ~= b then
    fails = fails + 1
    print("FAIL " .. msg .. " got=" .. tostring(a) .. " want=" .. tostring(b))
  end
end

local function rng_seq()
  local n = 0
  return function(max)
    n = n + 1
    return ((n - 1) % max) + 1
  end
end

do
  eq(Game.DIFF.easy.remove, 30, "easy remove")
  eq(Game.DIFF.medium.remove, 50, "med remove")
  eq(Game.DIFF.hard.remove, 64, "hard remove")
end

do
  local sol = Game.generate(rng_seq())
  eq(#sol, 81, "sol len")
  local row = {}
  for c = 1, 9 do row[sol[Game.idx(1, c)]] = true end
  local n = 0
  for _ = 1, 9 do if row[_] then n = n + 1 end end
  eq(n, 9, "row1 unique")
  local box = {}
  for r = 1, 3 do
    for c = 1, 3 do box[sol[Game.idx(r, c)]] = true end
  end
  n = 0
  for i = 1, 9 do if box[i] then n = n + 1 end end
  eq(n, 9, "box unique")
end

do
  local sol, cur, fixed = Game.new_puzzle(30, rng_seq())
  local holes, fx = 0, 0
  for i = 1, 81 do
    if cur[i] == 0 then holes = holes + 1 end
    if fixed[i] then fx = fx + 1 end
    if fixed[i] then eq(cur[i], sol[i], "fixed matches sol") end
  end
  eq(holes, 30, "holes")
  eq(fx, 51, "fixed count")
  eq(Game.is_win(cur, sol), false, "incomplete not win")
  eq(Game.is_win(sol, sol), true, "sol is win")
end

do
  local sol, cur, fixed = Game.new_puzzle(10, rng_seq())
  local empty
  for i = 1, 81 do
    if not fixed[i] then empty = i break end
  end
  cur[empty] = (sol[empty] % 9) + 1
  local err = Game.errors(cur, sol, fixed)
  eq(#err >= 1, true, "wrong marked")
end

do
  local st = {
    diff = "easy", sel = 4, msg = "ok", won = false,
    sol = {}, cur = {}, fixed = {},
  }
  for i = 1, 81 do
    st.sol[i] = (i % 9) + 1
    st.cur[i] = 0
    st.fixed[i] = i <= 40
  end
  local back = Game.deserialize(Game.serialize(st))
  eq(back.diff, "easy", "ser diff")
  eq(back.sel, 4, "ser sel")
  eq(back.sol[1], st.sol[1], "ser sol")
  eq(back.fixed[1], true, "ser fixed")
  eq(back.fixed[41], false, "ser hole")
end

if fails > 0 then
  error(fails .. " failures", 0)
end
print("test_game.lua OK")
