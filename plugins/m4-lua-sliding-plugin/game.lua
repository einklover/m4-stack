-- Sliding-number puzzle (8 / 15 / 24). Original Lua.
-- Board is a 1-based row-major list of length n*n. Value 0 is the blank.
-- Solved order is 1,2,...,n*n-1,0.
-- Scrambles are walks of legal blank moves from solved, so each deal is solvable.

Game = Game or {}

Game.STEPS = { [3] = 40, [4] = 80, [5] = 120 }

local DIRS = {
  { -1, 0 },
  { 1, 0 },
  { 0, -1 },
  { 0, 1 },
}

function Game.solved(n)
  local b = {}
  local last = n * n
  for i = 1, last - 1 do b[i] = i end
  b[last] = 0
  return b
end

function Game.clone(board)
  local b = {}
  for i = 1, #board do b[i] = board[i] end
  return b
end

function Game.blank_at(board)
  for i = 1, #board do
    if board[i] == 0 then return i end
  end
  return nil
end

function Game.rc(n, i)
  return math.floor((i - 1) / n) + 1, ((i - 1) % n) + 1
end

function Game.idx(n, r, c)
  if r < 1 or c < 1 or r > n or c > n then return nil end
  return (r - 1) * n + c
end

function Game.adjacent(n, a, b)
  if not a or not b or a == b then return false end
  local ar, ac = Game.rc(n, a)
  local br, bc = Game.rc(n, b)
  return (ar == br and math.abs(ac - bc) == 1) or (ac == bc and math.abs(ar - br) == 1)
end

function Game.boards_equal(a, b)
  if not a or not b or #a ~= #b then return false end
  for i = 1, #a do
    if a[i] ~= b[i] then return false end
  end
  return true
end

-- Slide the tile at `index` into the blank when the two cells share an edge.
function Game.slide_index(board, n, index)
  if type(board) ~= "table" or type(n) ~= "number" or type(index) ~= "number" then
    return false
  end
  if index ~= math.floor(index) or index < 1 or index > n * n then return false end
  local blank = Game.blank_at(board)
  if not blank or not Game.adjacent(n, index, blank) then return false end
  board[blank], board[index] = board[index], board[blank]
  return true
end

-- Move the blank by one orthogonal step. Returns true when the step lands on the board.
function Game.move_blank(board, n, dr, dc)
  if dr ~= 0 and dc ~= 0 then return false end
  if math.abs(dr) + math.abs(dc) ~= 1 then return false end
  local blank = Game.blank_at(board)
  if not blank then return false end
  local r, c = Game.rc(n, blank)
  local dest = Game.idx(n, r + dr, c + dc)
  if not dest then return false end
  return Game.slide_index(board, n, dest)
end

function Game.is_win(board, n)
  if type(board) ~= "table" or #board ~= n * n then return false end
  for i = 1, n * n - 1 do
    if board[i] ~= i then return false end
  end
  return board[n * n] == 0
end

function Game.rng_next(state)
  state = (state * 1103515245 + 12345) % 2147483648
  if state < 1 then state = 1 end
  return state
end

function Game.legal_dirs(board, n, ban_dr, ban_dc)
  local blank = Game.blank_at(board)
  local r, c = Game.rc(n, blank)
  local out = {}
  for i = 1, 4 do
    local dr, dc = DIRS[i][1], DIRS[i][2]
    local banned = ban_dr ~= nil and dr == ban_dr and dc == ban_dc
    if not banned and Game.idx(n, r + dr, c + dc) then
      out[#out + 1] = { dr, dc }
    end
  end
  if #out == 0 then
    for i = 1, 4 do
      local dr, dc = DIRS[i][1], DIRS[i][2]
      if Game.idx(n, r + dr, c + dc) then
        out[#out + 1] = { dr, dc }
      end
    end
  end
  return out
end

-- Walk `steps` legal blank moves from solved. Avoids the immediate reverse when another move exists.
function Game.scramble(n, seed, steps)
  if n ~= 3 and n ~= 4 and n ~= 5 then return nil end
  steps = steps or Game.STEPS[n]
  seed = math.floor(tonumber(seed) or 1)
  if seed < 1 then seed = 1 end
  local board = Game.solved(n)
  local moves = {}
  local rng = seed
  local prev_dr, prev_dc = nil, nil
  for _ = 1, steps do
    local ban_dr = prev_dr and -prev_dr or nil
    local ban_dc = prev_dc and -prev_dc or nil
    local opts = Game.legal_dirs(board, n, ban_dr, ban_dc)
    rng = Game.rng_next(rng)
    local pick = opts[((rng - 1) % #opts) + 1]
    if not Game.move_blank(board, n, pick[1], pick[2]) then return nil end
    moves[#moves + 1] = { pick[1], pick[2] }
    prev_dr, prev_dc = pick[1], pick[2]
  end
  return board, moves
end

function Game.apply_moves(board, n, moves)
  local b = Game.clone(board)
  for i = 1, #moves do
    local m = moves[i]
    if not Game.move_blank(b, n, m[1], m[2]) then return nil end
  end
  return b
end

function Game.inverse_moves(moves)
  local inv = {}
  for i = #moves, 1, -1 do
    inv[#inv + 1] = { -moves[i][1], -moves[i][2] }
  end
  return inv
end

function Game.inversion_count(board)
  local inv = 0
  for i = 1, #board do
    local a = board[i]
    if a ~= 0 then
      for j = i + 1, #board do
        local b = board[j]
        if b ~= 0 and b < a then inv = inv + 1 end
      end
    end
  end
  return inv
end

-- Odd width: even inversion count (blank ignored).
-- Even width: inversion count + blank row counted from the bottom is odd.
function Game.solvable(board, n)
  if type(board) ~= "table" or #board ~= n * n then return false end
  if n ~= 3 and n ~= 4 and n ~= 5 then return false end
  local seen = {}
  for i = 1, #board do
    local v = board[i]
    if type(v) ~= "number" or v < 0 or v >= n * n or seen[v] then return false end
    seen[v] = true
  end
  local inv = Game.inversion_count(board)
  if n % 2 == 1 then return inv % 2 == 0 end
  local br = Game.rc(n, Game.blank_at(board))
  local from_bottom = n - br + 1
  return (inv + from_bottom) % 2 == 1
end

function Game.home_rc(n, value)
  if value < 1 or value >= n * n then return nil end
  return Game.rc(n, value)
end

-- Monochrome diamond fragment. u,v are sample cells inside the tile's home square.
function Game.fragment_ink(n, value, u, v, res)
  local hr, hc = Game.home_rc(n, value)
  if not hr then return false end
  local gx = (hc - 1) * res + u + 0.5
  local gy = (hr - 1) * res + v + 0.5
  local cx = n * res / 2
  local cy = n * res / 2
  return math.abs(gx - cx) + math.abs(gy - cy) <= n * res * 0.38
end

function Game.ink_count(n, res)
  res = res or 8
  local nink = 0
  for value = 1, n * n - 1 do
    for v = 0, res - 1 do
      for u = 0, res - 1 do
        if Game.fragment_ink(n, value, u, v, res) then nink = nink + 1 end
      end
    end
  end
  return nink
end

local function split_pipe(s)
  local parts = {}
  local cur = ""
  for i = 1, #s do
    local ch = s:sub(i, i)
    if ch == "|" then
      parts[#parts + 1] = cur
      cur = ""
    else
      cur = cur .. ch
    end
  end
  parts[#parts + 1] = cur
  return parts
end

function Game.serialize(st)
  local tiles = {}
  for i = 1, #st.board do tiles[i] = tostring(st.board[i]) end
  local undo = {}
  local stack = st.undo or {}
  for i = 1, #stack do
    undo[i] = tostring(stack[i][1]) .. ":" .. tostring(stack[i][2])
  end
  local undo_s = #undo > 0 and table.concat(undo, ",") or "-"
  return table.concat({
    tostring(st.n),
    st.picture and "1" or "0",
    tostring(st.seed),
    tostring(st.steps),
    tostring(st.moves),
    st.won and "1" or "0",
    tostring(st.cr),
    tostring(st.cc),
    table.concat(tiles, ","),
    undo_s,
  }, "|")
end

function Game.deserialize(s)
  if type(s) ~= "string" then return nil end
  local parts = split_pipe(s)
  if #parts ~= 10 then return nil end
  local n = tonumber(parts[1])
  if n ~= 3 and n ~= 4 and n ~= 5 then return nil end
  local seed = tonumber(parts[3])
  local steps = tonumber(parts[4])
  local moves = tonumber(parts[5])
  local cr = tonumber(parts[7])
  local cc = tonumber(parts[8])
  if not seed or not steps or not moves or not cr or not cc then return nil end
  if cr < 1 or cc < 1 or cr > n or cc > n then return nil end
  local board = {}
  for tok in parts[9]:gmatch("[^,]+") do
    board[#board + 1] = tonumber(tok)
  end
  if #board ~= n * n or not Game.blank_at(board) then return nil end
  local seen = {}
  for i = 1, #board do
    local v = board[i]
    if not v or v < 0 or v >= n * n or seen[v] then return nil end
    seen[v] = true
  end
  local undo = {}
  if parts[10] ~= "-" then
    for tok in parts[10]:gmatch("[^,]+") do
      local dr, dc = tok:match("^([%-]?%d+):([%-]?%d+)$")
      dr, dc = tonumber(dr), tonumber(dc)
      if not dr or not dc then return nil end
      if not ((math.abs(dr) + math.abs(dc) == 1) and (dr == 0 or dc == 0)) then return nil end
      undo[#undo + 1] = { dr, dc }
    end
  end
  return {
    n = n,
    picture = parts[2] == "1",
    seed = seed,
    steps = steps,
    moves = moves,
    won = parts[6] == "1",
    cr = cr,
    cc = cc,
    board = board,
    undo = undo,
  }
end

function Game.fresh(n, seed, picture)
  local steps = Game.STEPS[n]
  local board = Game.scramble(n, seed, steps)
  if not board then return nil end
  local br, bc = Game.rc(n, Game.blank_at(board))
  return {
    n = n,
    picture = picture and true or false,
    seed = seed,
    steps = steps,
    moves = 0,
    won = Game.is_win(board, n),
    cr = br,
    cc = bc,
    board = board,
    undo = {},
  }
end

-- Record a player slide of the tile at index. Blank delta is pushed for undo.
function Game.play_index(st, index)
  if st.won then return false end
  local blank = Game.blank_at(st.board)
  if not Game.adjacent(st.n, index, blank) then return false end
  local br, bc = Game.rc(st.n, blank)
  local tr, tc = Game.rc(st.n, index)
  local dr, dc = tr - br, tc - bc
  if not Game.slide_index(st.board, st.n, index) then return false end
  st.undo[#st.undo + 1] = { dr, dc }
  st.moves = st.moves + 1
  st.won = Game.is_win(st.board, st.n)
  return true
end

function Game.play_cursor(st)
  local index = Game.idx(st.n, st.cr, st.cc)
  return Game.play_index(st, index)
end

function Game.undo(st)
  if not st.undo or #st.undo == 0 then return false end
  local m = st.undo[#st.undo]
  if not Game.move_blank(st.board, st.n, -m[1], -m[2]) then return false end
  st.undo[#st.undo] = nil
  if st.moves > 0 then st.moves = st.moves - 1 end
  st.won = false
  return true
end
