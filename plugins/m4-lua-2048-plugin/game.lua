-- Pure 2048 board logic. No gui/sys; host tests can dofile() this file.

Game = Game or {}

Game.SIZE = 4
Game.SPAWN_FOUR_CHANCE = 0.1

local function copy_board(board)
  local n = Game.SIZE
  local out = {}
  for r = 1, n do
    out[r] = {}
    for c = 1, n do
      out[r][c] = board[r][c]
    end
  end
  return out
end

function Game.new_board()
  local n = Game.SIZE
  local board = {}
  for r = 1, n do
    board[r] = {}
    for c = 1, n do
      board[r][c] = 0
    end
  end
  return board
end

function Game.empty_cells(board)
  local cells = {}
  local n = Game.SIZE
  for r = 1, n do
    for c = 1, n do
      if board[r][c] == 0 then
        cells[#cells + 1] = { r = r, c = c }
      end
    end
  end
  return cells
end

-- rng() -> float in [0,1). Optional rng_int(max) -> 1..max
function Game.spawn(board, rng, rng_int)
  local cells = Game.empty_cells(board)
  if #cells == 0 then return false end
  local idx
  if rng_int then
    idx = rng_int(#cells)
  else
    local u = rng and rng() or math.random()
    idx = math.floor(u * #cells) + 1
    if idx > #cells then idx = #cells end
  end
  local cell = cells[idx]
  local u2 = rng and rng() or math.random()
  board[cell.r][cell.c] = (u2 < Game.SPAWN_FOUR_CHANCE) and 4 or 2
  return true
end

-- Slide one row left (index 1 = left). Returns new row, score gained, moved?
function Game.slide_row_left(row)
  local n = Game.SIZE
  local compact = {}
  for i = 1, n do
    if row[i] ~= 0 then compact[#compact + 1] = row[i] end
  end
  local score = 0
  local i = 1
  local merged = {}
  while i <= #compact do
    if i < #compact and compact[i] == compact[i + 1] then
      local v = compact[i] * 2
      merged[#merged + 1] = v
      score = score + v
      i = i + 2
    else
      merged[#merged + 1] = compact[i]
      i = i + 1
    end
  end
  local out = {}
  for k = 1, n do out[k] = merged[k] or 0 end
  local moved = false
  for k = 1, n do
    if out[k] ~= row[k] then moved = true break end
  end
  return out, score, moved
end

local function rotate_left(board)
  local n = Game.SIZE
  local out = Game.new_board()
  for r = 1, n do
    for c = 1, n do
      out[n - c + 1][r] = board[r][c]
    end
  end
  return out
end

-- dir: "left" | "right" | "up" | "down"
-- Returns new_board, score_delta, moved
function Game.move(board, dir)
  local rots = 0
  if dir == "left" then
    rots = 0
  elseif dir == "down" then
    rots = 3
  elseif dir == "right" then
    rots = 2
  elseif dir == "up" then
    rots = 1
  else
    return copy_board(board), 0, false
  end
  local working = copy_board(board)
  for _ = 1, rots do working = rotate_left(working) end
  local n = Game.SIZE
  local score = 0
  local moved = false
  for r = 1, n do
    local nrow, sc, mv = Game.slide_row_left(working[r])
    working[r] = nrow
    score = score + sc
    if mv then moved = true end
  end
  local back = (4 - rots) % 4
  for _ = 1, back do working = rotate_left(working) end
  return working, score, moved
end

function Game.is_game_over(board)
  if #Game.empty_cells(board) > 0 then return false end
  local n = Game.SIZE
  for r = 1, n do
    for c = 1, n do
      local v = board[r][c]
      if c < n and board[r][c + 1] == v then return false end
      if r < n and board[r + 1][c] == v then return false end
    end
  end
  return true
end

function Game.max_tile(board)
  local m = 0
  local n = Game.SIZE
  for r = 1, n do
    for c = 1, n do
      if board[r][c] > m then m = board[r][c] end
    end
  end
  return m
end

function Game.serialize(board, score, best, game_over)
  local n = Game.SIZE
  local parts = { tostring(score or 0), tostring(best or 0), game_over and "1" or "0" }
  for r = 1, n do
    for c = 1, n do
      parts[#parts + 1] = tostring(board[r][c])
    end
  end
  return table.concat(parts, ",")
end

function Game.deserialize(s)
  if type(s) ~= "string" or s == "" then return nil end
  local vals = {}
  for token in string.gmatch(s, "[^,]+") do
    vals[#vals + 1] = tonumber(token)
  end
  local n = Game.SIZE
  if #vals ~= 3 + n * n then return nil end
  local board = Game.new_board()
  local k = 4
  for r = 1, n do
    for c = 1, n do
      board[r][c] = vals[k] or 0
      k = k + 1
    end
  end
  return {
    board = board,
    score = vals[1] or 0,
    best = vals[2] or 0,
    game_over = vals[3] == 1,
  }
end
