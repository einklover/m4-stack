-- Minesweeper (ReKindle minesweeper.html).
-- Portrait 480x800, square cells. Boards follow ~472:704 so the grid fills
-- the panel: easy 12x8/10, medium 21x14/40, hard 27x18/99.
-- First click is never a mine. Flood zeros. Flag/dig.

Game = Game or {}

Game.DIFF = {
  easy   = { rows = 12, cols = 8,  mines = 10 },
  medium = { rows = 21, cols = 14, mines = 40 },
  hard   = { rows = 27, cols = 18, mines = 99 },
}

function Game.new_grid(rows, cols)
  local g = {}
  for r = 1, rows do
    g[r] = {}
    for c = 1, cols do
      g[r][c] = { mine = false, shown = false, flag = false, n = 0 }
    end
  end
  return g
end

function Game.in_bounds(rows, cols, r, c)
  return r >= 1 and r <= rows and c >= 1 and c <= cols
end

function Game.count_neighbors(grid, rows, cols)
  for r = 1, rows do
    for c = 1, cols do
      if not grid[r][c].mine then
        local n = 0
        for dr = -1, 1 do
          for dc = -1, 1 do
            local nr, nc = r + dr, c + dc
            if Game.in_bounds(rows, cols, nr, nc) and grid[nr][nc].mine then
              n = n + 1
            end
          end
        end
        grid[r][c].n = n
      else
        grid[r][c].n = 0
      end
    end
  end
end

-- rng_int(n) -> 1..n. Safe cell (sr,sc) is never a mine.
function Game.place_mines(grid, rows, cols, mines, sr, sc, rng_int)
  local placed = 0
  local guard = 0
  while placed < mines and guard < mines * 80 do
    guard = guard + 1
    local r = rng_int(rows)
    local c = rng_int(cols)
    if not grid[r][c].mine and not (r == sr and c == sc) then
      grid[r][c].mine = true
      placed = placed + 1
    end
  end
  Game.count_neighbors(grid, rows, cols)
  return placed
end

function Game.flags_used(grid, rows, cols)
  local n = 0
  for r = 1, rows do
    for c = 1, cols do
      if grid[r][c].flag then n = n + 1 end
    end
  end
  return n
end

function Game.toggle_flag(grid, r, c)
  local cell = grid[r][c]
  if cell.shown then return false end
  cell.flag = not cell.flag
  return true
end

function Game.reveal(grid, rows, cols, r, c)
  local cell = grid[r][c]
  if cell.shown or cell.flag then return "noop" end
  cell.shown = true
  if cell.mine then return "dead" end
  if cell.n == 0 then
    local q = { { r, c } }
    local qi = 1
    while qi <= #q do
      local cr, cc = q[qi][1], q[qi][2]
      qi = qi + 1
      for dr = -1, 1 do
        for dc = -1, 1 do
          local nr, nc = cr + dr, cc + dc
          if Game.in_bounds(rows, cols, nr, nc) then
            local nb = grid[nr][nc]
            if not nb.shown and not nb.flag and not nb.mine then
              nb.shown = true
              if nb.n == 0 then
                q[#q + 1] = { nr, nc }
              end
            end
          end
        end
      end
    end
  end
  return "ok"
end

function Game.shown_count(grid, rows, cols)
  local n = 0
  for r = 1, rows do
    for c = 1, cols do
      if grid[r][c].shown then n = n + 1 end
    end
  end
  return n
end

function Game.is_win(grid, rows, cols, mines)
  return Game.shown_count(grid, rows, cols) == (rows * cols - mines)
end

function Game.serialize(st)
  local d = st.diff
  local rows, cols = st.rows, st.cols
  local parts = {
    d, tostring(rows), tostring(cols), tostring(st.mines),
    st.first and "1" or "0", st.over and "1" or "0", st.won and "1" or "0",
    st.mode or "dig", tostring(st.cursor_r or 1), tostring(st.cursor_c or 1),
  }
  for r = 1, rows do
    for c = 1, cols do
      local cell = st.grid[r][c]
      local v = 0
      if cell.mine then v = v + 1 end
      if cell.shown then v = v + 2 end
      if cell.flag then v = v + 4 end
      v = v + cell.n * 8
      parts[#parts + 1] = tostring(v)
    end
  end
  return table.concat(parts, ",")
end

function Game.deserialize(s)
  if type(s) ~= "string" or s == "" then return nil end
  local f = {}
  for token in string.gmatch(s, "[^,]+") do
    f[#f + 1] = token
  end
  if #f < 10 then return nil end
  local d = f[1]
  if not Game.DIFF[d] then return nil end
  local rows = tonumber(f[2])
  local cols = tonumber(f[3])
  local mines = tonumber(f[4])
  if not rows or not cols or not mines then return nil end
  if #f ~= 10 + rows * cols then return nil end
  local grid = Game.new_grid(rows, cols)
  local i = 11
  for r = 1, rows do
    for c = 1, cols do
      local v = tonumber(f[i])
      if not v then return nil end
      grid[r][c].mine = (v % 2) == 1
      grid[r][c].shown = math.floor(v / 2) % 2 == 1
      grid[r][c].flag = math.floor(v / 4) % 2 == 1
      grid[r][c].n = math.floor(v / 8)
      i = i + 1
    end
  end
  local cr = tonumber(f[9]) or 1
  local cc = tonumber(f[10]) or 1
  if cr < 1 then cr = 1 elseif cr > rows then cr = rows end
  if cc < 1 then cc = 1 elseif cc > cols then cc = cols end
  return {
    diff = d,
    rows = rows,
    cols = cols,
    mines = mines,
    first = f[5] == "1",
    over = f[6] == "1",
    won = f[7] == "1",
    mode = (f[8] == "flag") and "flag" or "dig",
    cursor_r = cr,
    cursor_c = cc,
    grid = grid,
  }
end
