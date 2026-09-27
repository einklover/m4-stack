-- Sudoku rules from ReKindle sudoku.html (not a source copy).
-- Easy/medium/hard remove 30/50/64 cells from a shifted Latin pattern.

Game = Game or {}

Game.DIFF = {
  easy   = { remove = 30 },
  medium = { remove = 50 },
  hard   = { remove = 64 },
}

local SHIFTS = { 0, 3, 6, 1, 4, 7, 2, 5, 8 }

function Game.idx(r, c)
  return (r - 1) * 9 + c
end

function Game.generate(rng_int)
  local base = { 1, 2, 3, 4, 5, 6, 7, 8, 9 }
  for i = 9, 2, -1 do
    local j = rng_int(i)
    base[i], base[j] = base[j], base[i]
  end
  local sol = {}
  for r = 1, 9 do
    local shift = SHIFTS[r]
    for c = 1, 9 do
      sol[Game.idx(r, c)] = base[(c - 1 + shift) % 9 + 1]
    end
  end
  return sol
end

function Game.new_puzzle(remove, rng_int)
  local sol = Game.generate(rng_int)
  local cur, fixed = {}, {}
  for i = 1, 81 do
    cur[i] = sol[i]
    fixed[i] = true
  end
  local holes, guard = 0, 0
  while holes < remove and guard < 500 do
    guard = guard + 1
    local i = rng_int(81)
    if fixed[i] then
      cur[i] = 0
      fixed[i] = false
      holes = holes + 1
    end
  end
  return sol, cur, fixed
end

function Game.is_win(cur, sol)
  for i = 1, 81 do
    if cur[i] == 0 or cur[i] ~= sol[i] then return false end
  end
  return true
end

-- Returns list of wrong (non-fixed, filled) indices.
function Game.errors(cur, sol, fixed)
  local err = {}
  for i = 1, 81 do
    if not fixed[i] and cur[i] ~= 0 and cur[i] ~= sol[i] then
      err[#err + 1] = i
    end
  end
  return err
end

function Game.has_empty(cur)
  for i = 1, 81 do
    if cur[i] == 0 then return true end
  end
  return false
end

function Game.next_free(fixed, from)
  local i = from
  for _ = 1, 81 do
    i = i % 81 + 1
    if not fixed[i] then return i end
  end
  return from
end

function Game.serialize(st)
  local parts = {
    st.diff, tostring(st.sel), st.msg or "",
    st.won and "1" or "0",
  }
  local function pack(arr)
    local t = {}
    for i = 1, 81 do t[i] = tostring(arr[i] or 0) end
    return table.concat(t, "")
  end
  local fx = {}
  for i = 1, 81 do fx[i] = st.fixed[i] and "1" or "0" end
  return table.concat(parts, ",") .. "\n" .. pack(st.sol) .. "\n" .. pack(st.cur) .. "\n" .. table.concat(fx, "")
end

function Game.deserialize(s)
  if type(s) ~= "string" then return nil end
  local line1, sol_s, cur_s, fx_s = s:match("^([^\n]+)\n([^\n]+)\n([^\n]+)\n([^\n]+)")
  if not line1 or not sol_s or #sol_s < 81 or #cur_s < 81 or #fx_s < 81 then
    return nil
  end
  local diff, sel_s, msg, won_s = line1:match("^([^,]+),([^,]*),(.*),([01])$")
  if not diff then return nil end
  local function unpack81(str)
    local t = {}
    for i = 1, 81 do t[i] = tonumber(str:sub(i, i)) or 0 end
    return t
  end
  local fixed = {}
  for i = 1, 81 do fixed[i] = fx_s:sub(i, i) == "1" end
  return {
    diff = diff,
    sel = tonumber(sel_s) or 1,
    msg = msg,
    won = won_s == "1",
    sol = unpack81(sol_s),
    cur = unpack81(cur_s),
    fixed = fixed,
  }
end
