-- Classic 3x3 Tic-Tac-Toe (ReKindle tictactoe.html rules).
-- Human X, CPU O. Easy = random empty cell; Hard = minimax.
-- Indices 1..9 row-major. Empty is nil.

Game = Game or {}

Game.HUMAN = "X"
Game.CPU = "O"

-- Same eight lines as ReKindle WINNING_COMBOS, 1-based.
Game.WIN = {
  {1, 2, 3}, {4, 5, 6}, {7, 8, 9},
  {1, 4, 7}, {2, 5, 8}, {3, 6, 9},
  {1, 5, 9}, {3, 5, 7},
}

function Game.new_board()
  return { nil, nil, nil, nil, nil, nil, nil, nil, nil }
end

function Game.copy_board(board)
  local out = {}
  for i = 1, 9 do
    out[i] = board[i]
  end
  return out
end

function Game.empty_cells(board)
  local cells = {}
  for i = 1, 9 do
    if not board[i] then
      cells[#cells + 1] = i
    end
  end
  return cells
end

function Game.check_win(board)
  for i = 1, #Game.WIN do
    local c = Game.WIN[i]
    local a, b, d = board[c[1]], board[c[2]], board[c[3]]
    if a and a == b and a == d then
      return c
    end
  end
  return nil
end

function Game.is_draw(board)
  if Game.check_win(board) then return false end
  for i = 1, 9 do
    if not board[i] then return false end
  end
  return true
end

function Game.can_place(board, index, active, current)
  if not active then return false end
  if type(index) ~= "number" or index < 1 or index > 9 then return false end
  if board[index] then return false end
  if current ~= Game.HUMAN then return false end
  return true
end

function Game.place(board, index, player)
  board[index] = player
end

-- rng_int(n) -> 1..n
function Game.random_move(board, rng_int)
  local cells = Game.empty_cells(board)
  if #cells == 0 then return -1 end
  local i = 1
  if rng_int then
    i = rng_int(#cells)
  end
  return cells[i]
end

local function minimax(board, depth, maximizing)
  local win = Game.check_win(board)
  if win then
    if board[win[1]] == Game.CPU then
      return 10 - depth
    end
    return depth - 10
  end
  if Game.is_draw(board) then
    return 0
  end
  if maximizing then
    local best = -1000
    for i = 1, 9 do
      if not board[i] then
        board[i] = Game.CPU
        local s = minimax(board, depth + 1, false)
        board[i] = nil
        if s > best then best = s end
      end
    end
    return best
  end
  local best = 1000
  for i = 1, 9 do
    if not board[i] then
      board[i] = Game.HUMAN
      local s = minimax(board, depth + 1, true)
      board[i] = nil
      if s < best then best = s end
    end
  end
  return best
end

-- Immediate win/block, then opening book. Full minimax only when the
-- remaining tree fits the host instruction budget (~2e6 / 8s).
local function winning_move(board, player)
  for i = 1, 9 do
    if not board[i] then
      board[i] = player
      local win = Game.check_win(board)
      board[i] = nil
      if win then return i end
    end
  end
  return nil
end

local OPENING = { 5, 1, 3, 7, 9, 2, 4, 6, 8 }

function Game.best_move(board)
  local win = winning_move(board, Game.CPU)
  if win then return win end
  local block = winning_move(board, Game.HUMAN)
  if block then return block end

  local empty = Game.empty_cells(board)
  if #empty > 6 then
    for i = 1, #OPENING do
      local c = OPENING[i]
      if not board[c] then return c end
    end
    return empty[1] or -1
  end

  local best_score = -1000
  local move = -1
  for i = 1, 9 do
    if not board[i] then
      board[i] = Game.CPU
      local score = minimax(board, 0, false)
      board[i] = nil
      if score > best_score then
        best_score = score
        move = i
      end
    end
  end
  return move
end

function Game.cpu_move(board, difficulty, rng_int)
  if difficulty == "easy" then
    return Game.random_move(board, rng_int)
  end
  return Game.best_move(board)
end

function Game.serialize(board, difficulty, current, active, cursor)
  local parts = { difficulty or "hard", current or Game.HUMAN, active and "1" or "0", tostring(cursor or 5) }
  for i = 1, 9 do
    parts[#parts + 1] = board[i] or "."
  end
  return table.concat(parts, ",")
end

function Game.deserialize(s)
  if type(s) ~= "string" or s == "" then return nil end
  local f = {}
  for token in string.gmatch(s, "[^,]+") do
    f[#f + 1] = token
  end
  if #f ~= 13 then return nil end
  local difficulty = f[1]
  if difficulty ~= "easy" and difficulty ~= "hard" then return nil end
  local current = f[2]
  if current ~= Game.HUMAN and current ~= Game.CPU then return nil end
  local active = f[3] == "1"
  local cursor = tonumber(f[4])
  if not cursor or cursor < 1 or cursor > 9 then return nil end
  local board = {}
  for i = 1, 9 do
    local v = f[4 + i]
    if v == "." then
      board[i] = nil
    elseif v == Game.HUMAN or v == Game.CPU then
      board[i] = v
    else
      return nil
    end
  end
  return {
    board = board,
    difficulty = difficulty,
    current = current,
    active = active,
    cursor = cursor,
  }
end
