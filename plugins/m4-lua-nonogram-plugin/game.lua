-- Black-and-white Nonogram / Picross. Pure logic, no I/O.
-- Solutions are original handcrafted pictures. Clues are derived from them.
-- Cells: 0 unknown, 1 filled, 2 marked X. Win ignores X vs unknown on white.

Game = Game or {}

Game.UNKNOWN = 0
Game.FILLED = 1
Game.MARK = 2

-- Row-major '0'/'1' pictures. Clues are never hand-copied.
Game.LEVELS = {
  { name = "十字", n = 5, s = "0010000100111110010000100" },
  { name = "方框", n = 5, s = "1111110001100011000111111" },
  { name = "丁字", n = 5, s = "1111100100001000010000100" },
  { name = "台阶", n = 5, s = "1000011000111001111011111" },
  { name = "菱形", n = 5, s = "0010001110111110111000100" },
  { name = "箭头", n = 5, s = "0010000110111110011000100" },
  { name = "U形", n = 5, s = "1000110001100011000101110" },
  { name = "心形", n = 5, s = "0110011110111110111000100" },
  { name = "横条", n = 10, s =
      "1111111111" ..
      "0000000000" ..
      "1111111111" ..
      "0000000000" ..
      "1111111111" ..
      "0000000000" ..
      "1111111111" ..
      "0000000000" ..
      "1111111111" ..
      "0000000000" },
  { name = "方环", n = 10, s =
      "1111111111" ..
      "1000000001" ..
      "1000000001" ..
      "1000000001" ..
      "1000000001" ..
      "1000000001" ..
      "1000000001" ..
      "1000000001" ..
      "1000000001" ..
      "1111111111" },
  { name = "中十字", n = 10, s =
      "0000100000" ..
      "0000100000" ..
      "0000100000" ..
      "0000100000" ..
      "1111111111" ..
      "1111111111" ..
      "0000100000" ..
      "0000100000" ..
      "0000100000" ..
      "0000100000" },
  { name = "下阶", n = 10, s =
      "1000000000" ..
      "1100000000" ..
      "1110000000" ..
      "1111000000" ..
      "1111100000" ..
      "1111110000" ..
      "1111111000" ..
      "1111111100" ..
      "1111111110" ..
      "1111111111" },
  { name = "上阶", n = 10, s =
      "1111111111" ..
      "1111111110" ..
      "1111111100" ..
      "1111111000" ..
      "1111110000" ..
      "1111100000" ..
      "1111000000" ..
      "1110000000" ..
      "1100000000" ..
      "1000000000" },
}

function Game.level_count()
  return #Game.LEVELS
end

function Game.level(i)
  if type(i) ~= "number" then return nil end
  i = math.floor(i)
  if i < 1 or i > #Game.LEVELS then return nil end
  return Game.LEVELS[i]
end

function Game.bit(lv, r, c)
  local n = lv.n
  if r < 1 or c < 1 or r > n or c > n then return 0 end
  local ch = lv.s:byte((r - 1) * n + c)
  if ch == 49 then return 1 end
  return 0
end

function Game.line_clue(bits)
  local runs = {}
  local c = 0
  for i = 1, #bits do
    if bits[i] == 1 then
      c = c + 1
    elseif c > 0 then
      runs[#runs + 1] = c
      c = 0
    end
  end
  if c > 0 then runs[#runs + 1] = c end
  return runs
end

function Game.row_bits(lv, r)
  local bits = {}
  for c = 1, lv.n do bits[c] = Game.bit(lv, r, c) end
  return bits
end

function Game.col_bits(lv, c)
  local bits = {}
  for r = 1, lv.n do bits[r] = Game.bit(lv, r, c) end
  return bits
end

function Game.clues(lv)
  local rows, cols = {}, {}
  for i = 1, lv.n do
    rows[i] = Game.line_clue(Game.row_bits(lv, i))
    cols[i] = Game.line_clue(Game.col_bits(lv, i))
  end
  return rows, cols
end

function Game.clue_text(runs)
  if #runs == 0 then return "0" end
  local t = {}
  for i = 1, #runs do t[i] = tostring(runs[i]) end
  return table.concat(t, " ")
end

local function blank_grid(n)
  local g = {}
  for i = 1, n * n do g[i] = Game.UNKNOWN end
  return g
end

function Game.new_play(level_index)
  local i = level_index or 1
  if not Game.level(i) then i = 1 end
  local n = Game.level(i).n
  return {
    level = i,
    r = 1,
    c = 1,
    grid = blank_grid(n),
    undo = {},
    won = false,
  }
end

local function idx(n, r, c)
  return (r - 1) * n + c
end

function Game.cell(play, r, c)
  local lv = Game.level(play.level)
  if not lv then return nil end
  if r < 1 or c < 1 or r > lv.n or c > lv.n then return nil end
  return play.grid[idx(lv.n, r, c)]
end

function Game.is_win(play)
  local lv = Game.level(play.level)
  if not lv then return false end
  local n = lv.n
  for r = 1, n do
    for c = 1, n do
      local filled = play.grid[idx(n, r, c)] == Game.FILLED
      local black = Game.bit(lv, r, c) == 1
      if filled ~= black then return false end
    end
  end
  return true
end

local function push_undo(play, r, c, prev)
  local u = play.undo
  u[#u + 1] = r
  u[#u + 1] = c
  u[#u + 1] = prev
  local cap = 32 * 3
  if #u > cap then
    local drop = #u - cap
    local nxt = {}
    for i = drop + 1, #u do nxt[#nxt + 1] = u[i] end
    play.undo = nxt
  end
end

function Game.cycle(play, r, c)
  local lv = Game.level(play.level)
  if not lv or play.won then return false end
  if r < 1 or c < 1 or r > lv.n or c > lv.n then return false end
  local i = idx(lv.n, r, c)
  local prev = play.grid[i]
  local nxt = Game.FILLED
  if prev == Game.FILLED then nxt = Game.MARK
  elseif prev == Game.MARK then nxt = Game.UNKNOWN
  end
  play.grid[i] = nxt
  play.r, play.c = r, c
  push_undo(play, r, c, prev)
  play.won = Game.is_win(play)
  return true
end

function Game.undo(play)
  local u = play.undo
  if #u < 3 then return false end
  local prev = u[#u]
  local c = u[#u - 1]
  local r = u[#u - 2]
  u[#u] = nil
  u[#u] = nil
  u[#u] = nil
  local lv = Game.level(play.level)
  if not lv then return false end
  if r < 1 or c < 1 or r > lv.n or c > lv.n then return false end
  play.grid[idx(lv.n, r, c)] = prev
  play.r, play.c = r, c
  play.won = Game.is_win(play)
  return true
end

function Game.reset_grid(play)
  local lv = Game.level(play.level)
  if not lv then return false end
  play.grid = blank_grid(lv.n)
  play.undo = {}
  play.won = false
  play.r, play.c = 1, 1
  return true
end

function Game.hint(play)
  local lv = Game.level(play.level)
  if not lv or play.won then return false end
  local n = lv.n
  local pick_r, pick_c, pick_to
  for r = 1, n do
    for c = 1, n do
      local black = Game.bit(lv, r, c) == 1
      local v = play.grid[idx(n, r, c)]
      local wrong = (v == Game.FILLED) ~= black
      if wrong and black and v ~= Game.FILLED then
        pick_r, pick_c, pick_to = r, c, Game.FILLED
        break
      end
    end
    if pick_r then break end
  end
  if not pick_r then
    for r = 1, n do
      for c = 1, n do
        local black = Game.bit(lv, r, c) == 1
        local v = play.grid[idx(n, r, c)]
        if (v == Game.FILLED) ~= black then
          pick_r, pick_c = r, c
          pick_to = black and Game.FILLED or Game.MARK
          break
        end
      end
      if pick_r then break end
    end
  end
  if not pick_r then return false end
  local i = idx(n, pick_r, pick_c)
  local prev = play.grid[i]
  if prev == pick_to then return false end
  play.grid[i] = pick_to
  play.r, play.c = pick_r, pick_c
  push_undo(play, pick_r, pick_c, prev)
  play.won = Game.is_win(play)
  return true
end

function Game.set_level(play, level_index)
  if not Game.level(level_index) then return false end
  play.level = level_index
  return Game.reset_grid(play)
end

function Game.serialize(play)
  local lv = Game.level(play.level)
  if not lv then return "" end
  local chars = {}
  for i = 1, #play.grid do chars[i] = tostring(play.grid[i] or 0) end
  local u = {}
  for i = 1, #play.undo do u[i] = tostring(play.undo[i]) end
  return table.concat({
    "1",
    tostring(play.level),
    tostring(play.r),
    tostring(play.c),
    play.won and "1" or "0",
    table.concat(chars),
    table.concat(u, ","),
  }, "|")
end

function Game.deserialize(s)
  if type(s) ~= "string" then return nil end
  local ver, li, rs, cs, ws, cells, undo_s = s:match("^([^|]*)|([^|]*)|([^|]*)|([^|]*)|([^|]*)|([^|]*)|(.*)$")
  if ver ~= "1" then return nil end
  local level = tonumber(li)
  local lv = Game.level(level)
  if not lv then return nil end
  local r, c = tonumber(rs), tonumber(cs)
  if not r or not c then return nil end
  r = math.floor(r)
  c = math.floor(c)
  if r < 1 or r > lv.n or c < 1 or c > lv.n then return nil end
  if #cells ~= lv.n * lv.n then return nil end
  local grid = {}
  for i = 1, #cells do
    local ch = cells:byte(i)
    if ch == 48 then grid[i] = 0
    elseif ch == 49 then grid[i] = 1
    elseif ch == 50 then grid[i] = 2
    else return nil end
  end
  local undo = {}
  if undo_s and undo_s ~= "" then
    for tok in undo_s:gmatch("[^,]+") do
      local v = tonumber(tok)
      if not v then return nil end
      undo[#undo + 1] = math.floor(v)
    end
    if #undo % 3 ~= 0 or #undo > 96 then return nil end
  end
  local play = {
    level = level,
    r = r,
    c = c,
    grid = grid,
    undo = undo,
    won = ws == "1",
  }
  play.won = Game.is_win(play)
  return play
end

-- 480x800 layout shared by UI and touch tests. Clue bands scale with n.
function Game.layout(n, W, H)
  W = W or 480
  H = H or 800
  local title_h = 34
  local btn_h = 48
  local max_runs = math.ceil(n / 2)
  local clue_top = max_runs * 13
  local clue_left = max_runs * 18 + 6
  local avail_w = W - 8 - clue_left
  local avail_h = H - title_h - btn_h - 10 - clue_top
  local cell = math.floor(math.min(avail_w / n, avail_h / n))
  if cell < 8 then cell = 8 end
  local grid_w, grid_h = cell * n, cell * n
  local grid_x = clue_left + math.floor((W - clue_left - grid_w) / 2)
  if grid_x < clue_left then grid_x = clue_left end
  local grid_y = title_h + clue_top
  local spare = H - btn_h - 6 - grid_y - grid_h
  if spare > 0 then grid_y = grid_y + math.floor(spare / 2) end
  return {
    n = n,
    cell = cell,
    grid_x = grid_x,
    grid_y = grid_y,
    grid_w = grid_w,
    grid_h = grid_h,
    clue_top = clue_top,
    clue_left = clue_left,
    title_h = title_h,
    btn_h = btn_h,
    W = W,
    H = H,
  }
end

function Game.cell_at(lay, x, y)
  if not lay then return nil, nil end
  if x < lay.grid_x or y < lay.grid_y then return nil, nil end
  if x >= lay.grid_x + lay.grid_w or y >= lay.grid_y + lay.grid_h then return nil, nil end
  local c = math.floor((x - lay.grid_x) / lay.cell) + 1
  local r = math.floor((y - lay.grid_y) / lay.cell) + 1
  if r < 1 or c < 1 or r > lay.n or c > lay.n then return nil, nil end
  return r, c
end

function Game.buttons(W, H)
  W = W or 480
  H = H or 800
  local h = 48
  local y = H - h
  local labels = { "exit", "help", "undo", "hint", "reset", "prev", "next" }
  local gap = 4
  local w = math.floor((W - 8 - gap * (#labels - 1)) / #labels)
  local btns = {}
  for i = 1, #labels do
    btns[labels[i]] = { x = 4 + (i - 1) * (w + gap), y = y, w = w, h = h }
  end
  return btns
end

function Game.hit(b, x, y)
  return x >= b.x and x < b.x + b.w and y >= b.y and y < b.y + b.h
end
