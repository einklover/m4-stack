-- Host-only logic tests plus a line-by-line uniqueness counter.
-- The counter is not loaded by main.lua.

local dir = (arg and arg[0] and arg[0]:match("(.*/)")) or "./"
dofile(dir .. "game.lua")

local fails = 0
local function eq(a, b, msg)
  if a ~= b then
    fails = fails + 1
    print("FAIL " .. msg .. " got=" .. tostring(a) .. " want=" .. tostring(b))
  end
end

local function clue_eq(a, b)
  if #a ~= #b then return false end
  for i = 1, #a do
    if #a[i] ~= #b[i] then return false end
    for j = 1, #a[i] do
      if a[i][j] ~= b[i][j] then return false end
    end
  end
  return true
end

local function runs_of(bits)
  return Game.line_clue(bits)
end

local function same_runs(bits, clue)
  local got = runs_of(bits)
  if #got ~= #clue then return false end
  for i = 1, #got do
    if got[i] ~= clue[i] then return false end
  end
  return true
end

local function all_masks(n, clue)
  local out = {}
  local total = 2 ^ n
  for mask = 0, total - 1 do
    local bits = {}
    for i = 1, n do
      local shift = n - i
      bits[i] = math.floor(mask / (2 ^ shift)) % 2
    end
    if same_runs(bits, clue) then
      out[#out + 1] = bits
    end
  end
  return out
end

-- Partial column (length p of n) can still be completed to clue.
local function prefix_ok(bits, n, clue)
  local p = #bits
  local closed = {}
  local run = 0
  local open = false
  for i = 1, p do
    if bits[i] == 1 then
      run = run + 1
      open = true
    elseif run > 0 then
      closed[#closed + 1] = run
      run = 0
      open = false
    end
  end
  for i = 1, #closed do
    if not clue[i] or closed[i] ~= clue[i] then return false end
  end
  local function min_cells(from)
    local m = 0
    for i = from, #clue do
      if m > 0 then m = m + 1 end
      m = m + clue[i]
    end
    return m
  end
  local after = n - p
  if open then
    local k = #closed + 1
    local expect = clue[k]
    if not expect or run > expect then return false end
    local rest = min_cells(k + 1)
    local need
    if run < expect then
      need = (expect - run) + (rest > 0 and (1 + rest) or 0)
    else
      need = rest > 0 and (1 + rest) or 0
    end
    if after < need then return false end
    return true
  end
  if after < min_cells(#closed + 1) then return false end
  return true
end

local function count_solutions(row_clues, col_clues, n, limit)
  local opts = {}
  local branch = 0
  for r = 1, n do
    opts[r] = all_masks(n, row_clues[r])
    branch = branch + #opts[r]
    if #opts[r] == 0 then return 0, branch end
  end
  local col = {}
  for c = 1, n do col[c] = {} end
  local count = 0
  local nodes = 0
  local cap = 2000000
  local function dfs(r)
    if count >= limit then return end
    nodes = nodes + 1
    if nodes > cap then
      count = -1
      return
    end
    if r > n then
      for c = 1, n do
        if not same_runs(col[c], col_clues[c]) then return end
      end
      count = count + 1
      return
    end
    for oi = 1, #opts[r] do
      local mask = opts[r][oi]
      local ok = true
      for c = 1, n do
        col[c][r] = mask[c]
        if not prefix_ok(col[c], n, col_clues[c]) then
          ok = false
          for k = 1, c do col[k][r] = nil end
          break
        end
      end
      if ok then dfs(r + 1) end
      for c = 1, n do col[c][r] = nil end
      if count >= limit or count < 0 then return end
    end
  end
  dfs(1)
  return count, branch
end

do
  eq(Game.level_count() >= 10, true, "at least 10 levels")
  local n5, n10 = 0, 0
  for i = 1, Game.level_count() do
    local lv = Game.level(i)
    eq(#lv.s, lv.n * lv.n, "solution length " .. lv.name)
    eq(lv.n == 5 or lv.n == 10, true, "size " .. lv.name)
    if lv.n == 5 then n5 = n5 + 1 end
    if lv.n == 10 then n10 = n10 + 1 end
    local rows, cols = Game.clues(lv)
    eq(#rows, lv.n, "row clues " .. lv.name)
    eq(#cols, lv.n, "col clues " .. lv.name)
    local again_r, again_c = Game.clues(lv)
    eq(clue_eq(rows, again_r), true, "stable clues " .. lv.name)
    eq(clue_eq(cols, again_c), true, "stable col clues " .. lv.name)
    -- Re-derive from the picture and compare to Game.clues.
    for r = 1, lv.n do
      eq(same_runs(Game.row_bits(lv, r), rows[r]), true, "row derive " .. lv.name .. " r" .. r)
    end
    for c = 1, lv.n do
      eq(same_runs(Game.col_bits(lv, c), cols[c]), true, "col derive " .. lv.name .. " c" .. c)
    end
  end
  eq(n5 >= 4, true, "several 5x5")
  eq(n10 >= 4, true, "several 10x10")
end

do
  local cross = Game.level(1)
  local rows = Game.clues(cross)
  eq(#rows[1], 1, "cross row1 one run")
  eq(rows[1][1], 1, "cross row1 len")
  eq(rows[3][1], 5, "cross mid")
  eq(Game.clue_text({}), "0", "empty clue text")
  eq(Game.clue_text({1, 1}), "1 1", "two runs")
end

do
  local unique = 0
  local best_branch, worst_branch = 999999, 0
  local saw5, saw10 = false, false
  for i = 1, Game.level_count() do
    local lv = Game.level(i)
    local rows, cols = Game.clues(lv)
    local count, branch = count_solutions(rows, cols, lv.n, 2)
    eq(count, 1, "unique " .. lv.name .. " n=" .. lv.n)
    if count == 1 then unique = unique + 1 end
    if branch < best_branch then best_branch = branch end
    if branch > worst_branch then worst_branch = branch end
    if lv.n == 5 and count == 1 then saw5 = true end
    if lv.n == 10 and count == 1 then saw10 = true end
    print(string.format("level %02d %-6s n=%d solutions=%d branch=%d", i, lv.name, lv.n, count, branch))
  end
  eq(unique >= 8, true, "at least 8 single-solution")
  eq(saw5, true, "unique 5x5")
  eq(saw10, true, "unique 10x10")
  eq(worst_branch > best_branch, true, "difficulty spread")
end

do
  local p = Game.new_play(1)
  eq(Game.is_win(p), false, "blank not win")
  local lv = Game.level(1)
  for r = 1, lv.n do
    for c = 1, lv.n do
      if Game.bit(lv, r, c) == 1 then
        Game.cycle(p, r, c)
      end
    end
  end
  eq(Game.is_win(p), true, "filled blacks win")
  eq(p.won, true, "cycle sets won")
  -- Extra fill on a white cell breaks the win. Undo that mistake.
  local wr, wc
  for r = 1, lv.n do
    for c = 1, lv.n do
      if Game.bit(lv, r, c) == 0 then wr, wc = r, c break end
    end
    if wr then break end
  end
  p.won = false
  eq(Game.cycle(p, wr, wc), true, "mark white as black")
  eq(Game.cell(p, wr, wc), Game.FILLED, "now filled")
  eq(Game.is_win(p), false, "extra fill loses")
  eq(Game.undo(p), true, "undo extra")
  eq(Game.cell(p, wr, wc), Game.UNKNOWN, "restored unknown")
  eq(Game.is_win(p), true, "win again")
end

do
  local p = Game.new_play(2)
  eq(Game.cycle(p, 1, 1), true, "cycle unknown")
  eq(Game.cell(p, 1, 1), 1, "to filled")
  eq(Game.cycle(p, 1, 1), true, "cycle filled")
  eq(Game.cell(p, 1, 1), 2, "to X")
  eq(Game.cycle(p, 1, 1), true, "cycle X")
  eq(Game.cell(p, 1, 1), 0, "to unknown")
  eq(Game.hint(p), true, "hint acts")
  eq(Game.cell(p, p.r, p.c) == Game.FILLED or Game.cell(p, p.r, p.c) == Game.MARK, true, "hint wrote")
  local back = Game.deserialize(Game.serialize(p))
  eq(back.level, p.level, "save level")
  eq(back.r, p.r, "save r")
  eq(back.c, p.c, "save c")
  eq(Game.cell(back, p.r, p.c), Game.cell(p, p.r, p.c), "save cell")
  eq(#back.undo, #p.undo, "save undo")
  Game.reset_grid(p)
  eq(Game.cell(p, 1, 1), 0, "reset")
  eq(#p.undo, 0, "reset undo")
  eq(Game.deserialize("nope"), nil, "bad save")
  eq(Game.deserialize(""), nil, "empty save")
end

do
  local lay5 = Game.layout(5, 480, 800)
  local lay10 = Game.layout(10, 480, 800)
  eq(lay5.cell > lay10.cell, true, "5x5 cells larger")
  local r, c = Game.cell_at(lay10, lay10.grid_x + 1, lay10.grid_y + 1)
  eq(r, 1, "touch r1")
  eq(c, 1, "touch c1")
  local r2, c2 = Game.cell_at(lay10,
    lay10.grid_x + lay10.cell * 3 + 2,
    lay10.grid_y + lay10.cell * 4 + 2)
  eq(r2, 5, "touch r5")
  eq(c2, 4, "touch c4")
  local ox, oy = Game.cell_at(lay10, 0, 0)
  eq(ox, nil, "outside nil")
  local bx = Game.buttons(480, 800)
  eq(Game.hit(bx.exit, bx.exit.x + 1, bx.exit.y + 1), true, "exit hit")
  eq(Game.hit(bx.next, bx.exit.x + 1, bx.exit.y + 1), false, "next not exit")
  local br, bc = Game.cell_at(lay10, bx.reset.x + 1, bx.reset.y + 1)
  eq(br, nil, "button is not a cell")
  eq(lay10.grid_y + lay10.grid_h <= bx.exit.y, true, "grid above buttons")
  eq(lay5.grid_x >= lay5.clue_left, true, "room for row clues")
end

do
  -- prefix helper sanity used by the uniqueness search
  eq(prefix_ok({1, 1, 1}, 5, {3}), true, "open run can finish")
  eq(prefix_ok({1, 0, 1}, 5, {3}), false, "broken run mismatches")
  eq(prefix_ok({0, 0, 0, 0, 0}, 5, {1}), false, "no room left")
end

if fails > 0 then
  error(fails .. " failures", 0)
end
print("test_game.lua OK")
