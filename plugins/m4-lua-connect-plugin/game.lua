-- 连连看 / Shisen-Sho. Pure rules, no host APIs.
-- Two equal tiles clear when an orthogonal path through empty cells or the
-- one-cell outside border turns at most twice. V1 deals each row as horizontal
-- neighbor pairs and only shuffles which symbol each pair shows.

Game = Game or {}

Game.LEVELS = {
  easy = { rows = 4, cols = 6, kinds = 6 },
  medium = { rows = 6, cols = 8, kinds = 12 },
}

local DIRS = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } }

function Game.rng(seed)
  local s = math.floor(tonumber(seed) or 1) % 2147483647
  if s <= 0 then s = s + 2147483646 end
  return function()
    s = (s * 48271) % 2147483647
    return s
  end
end

function Game.blank(rows, cols)
  local b = {}
  for r = 1, rows do
    b[r] = {}
    for c = 1, cols do b[r][c] = 0 end
  end
  return b
end

function Game.copy_board(board)
  local b = {}
  for r = 1, #board do
    b[r] = {}
    for c = 1, #board[r] do b[r][c] = board[r][c] end
  end
  return b
end

-- Geometric route. Endpoints are passable even when occupied.
-- Returns a cell list (includes both ends) or nil. Turns <= 2.
function Game.route(rows, cols, board, r1, c1, r2, c2)
  if r1 == r2 and c1 == c2 then return nil end
  if r1 < 1 or c1 < 1 or r2 < 1 or c2 < 1 then return nil end
  if r1 > rows or r2 > rows or c1 > cols or c2 > cols then return nil end

  local function open(r, c)
    if r == r1 and c == c1 then return true end
    if r == r2 and c == c2 then return true end
    if r < 0 or c < 0 or r > rows + 1 or c > cols + 1 then return false end
    if r < 1 or c < 1 or r > rows or c > cols then return true end
    return board[r][c] == 0
  end

  local seen = {}
  local function mark(r, c, dir, turns)
    local k = ((r + 1) * 40 + (c + 1)) * 20 + dir * 4 + turns
    if seen[k] then return false end
    seen[k] = true
    return true
  end

  local q = { { r = r1, c = c1, dir = 0, turns = 0, prev = nil } }
  mark(r1, c1, 0, 0)
  local head = 1
  while head <= #q do
    local n = q[head]
    head = head + 1
    if n.dir ~= 0 and n.r == r2 and n.c == c2 then
      local pts = {}
      local cur = n
      while cur do
        pts[#pts + 1] = { cur.r, cur.c }
        cur = cur.prev
      end
      local out = {}
      for i = #pts, 1, -1 do out[#out + 1] = pts[i] end
      return out
    end
    for d = 1, 4 do
      local nr, nc = n.r + DIRS[d][1], n.c + DIRS[d][2]
      if open(nr, nc) and not (nr == r1 and nc == c1) then
        local turns = n.turns
        if n.dir ~= 0 and n.dir ~= d then turns = turns + 1 end
        if turns <= 2 and mark(nr, nc, d, turns) then
          q[#q + 1] = { r = nr, c = nc, dir = d, turns = turns, prev = n }
        end
      end
    end
  end
  return nil
end

function Game.bends(path)
  if not path or #path < 2 then return nil end
  local bends, prev = 0, nil
  for i = 2, #path do
    local dr = path[i][1] - path[i - 1][1]
    local dc = path[i][2] - path[i - 1][2]
    if dr ~= 0 and dc ~= 0 then return nil end
    if math.abs(dr) + math.abs(dc) ~= 1 then return nil end
    local dir = dr * 3 + dc
    if prev and prev ~= dir then bends = bends + 1 end
    prev = dir
  end
  return bends
end

function Game.can_link(rows, cols, board, r1, c1, r2, c2)
  if r1 < 1 or c1 < 1 or r2 < 1 or c2 < 1 then return false end
  if r1 > rows or r2 > rows or c1 > cols or c2 > cols then return false end
  local v = board[r1][c1]
  if v == 0 or v ~= board[r2][c2] then return false end
  if r1 == r2 and c1 == c2 then return false end
  return Game.route(rows, cols, board, r1, c1, r2, c2) ~= nil
end

function Game.hint(rows, cols, board)
  local buckets = {}
  for r = 1, rows do
    for c = 1, cols do
      local v = board[r][c]
      if v ~= 0 then
        local list = buckets[v]
        if not list then
          list = {}
          buckets[v] = list
        end
        list[#list + 1] = { r, c }
      end
    end
  end
  local kinds = {}
  for v in pairs(buckets) do kinds[#kinds + 1] = v end
  table.sort(kinds)
  for i = 1, #kinds do
    local cells = buckets[kinds[i]]
    for a = 1, #cells do
      for b = a + 1, #cells do
        local p = cells[a]
        local q = cells[b]
        if Game.route(rows, cols, board, p[1], p[2], q[1], q[2]) then
          return p[1], p[2], q[1], q[2]
        end
      end
    end
  end
  return nil
end

local function take_kind(quota, rng)
  local opts = {}
  for k = 1, #quota do
    if quota[k] >= 2 then opts[#opts + 1] = k end
  end
  if #opts == 0 then return nil end
  local k = opts[(rng() % #opts) + 1]
  quota[k] = quota[k] - 2
  return k
end

local function remove_at(list, idx)
  list[idx] = list[#list]
  list[#list] = nil
end

-- V1 deal: each row is fixed horizontal pairs (r,1)-(r,2), (r,3)-(r,4), ...
-- A seeded shuffle permutes pair labels only. Cells are not moved, and there
-- is no placement search. Adjacent equals always link, so the deal is solvable.
function Game.construct(rows, cols, kinds, seed)
  local total = rows * cols
  if cols % 2 ~= 0 or total % 2 ~= 0 or kinds < 1
      or (total % kinds) ~= 0 or ((total / kinds) % 2) ~= 0 then
    return nil, "bad size"
  end
  local pairs_per = (total / kinds) / 2
  local labels = {}
  for k = 1, kinds do
    for _ = 1, pairs_per do labels[#labels + 1] = k end
  end
  local rng = Game.rng(seed)
  for i = #labels, 2, -1 do
    local j = (rng() % i) + 1
    labels[i], labels[j] = labels[j], labels[i]
  end
  local board = Game.blank(rows, cols)
  local placed = {}
  local n = 1
  for r = 1, rows do
    for c = 1, cols, 2 do
      local kind = labels[n]
      n = n + 1
      board[r][c] = kind
      board[r][c + 1] = kind
      placed[#placed + 1] = { r, c, r, c + 1, kind }
    end
  end
  local proof = {}
  for i = #placed, 1, -1 do proof[#proof + 1] = placed[i] end
  return board, proof, false
end

function Game.remaining(board)
  local n = 0
  for r = 1, #board do
    for c = 1, #board[r] do
      if board[r][c] ~= 0 then n = n + 1 end
    end
  end
  return n
end

function Game.counts(board)
  local t = {}
  for r = 1, #board do
    for c = 1, #board[r] do
      local v = board[r][c]
      if v ~= 0 then t[v] = (t[v] or 0) + 1 end
    end
  end
  return t
end

function Game.apply_proof(rows, cols, board, proof)
  local b = Game.copy_board(board)
  for i = 1, #proof do
    local p = proof[i]
    if not Game.can_link(rows, cols, b, p[1], p[2], p[3], p[4]) then
      return false
    end
    if b[p[1]][p[2]] ~= p[5] or b[p[3]][p[4]] ~= p[5] then return false end
    b[p[1]][p[2]] = 0
    b[p[3]][p[4]] = 0
  end
  return Game.remaining(b) == 0
end

local function fresh(level, seed)
  local spec = Game.LEVELS[level]
  if not spec then return nil end
  local board, proof, fallback = Game.construct(spec.rows, spec.cols, spec.kinds, seed)
  if not board then return nil end
  return {
    level = level,
    seed = math.floor(tonumber(seed) or 1),
    rows = spec.rows,
    cols = spec.cols,
    kinds = spec.kinds,
    board = board,
    proof = proof,
    fallback = fallback and true or false,
    sel = nil,
    cursor = { 1, 1 },
    undo = {},
    moves = 0,
    hints = 0,
    shuffles = 0,
    won = false,
  }
end

function Game.new_game(level, seed)
  return fresh(level or "easy", seed or 1)
end

function Game.clear_pair(st, r1, c1, r2, c2)
  local v = st.board[r1][c1]
  st.board[r1][c1] = 0
  st.board[r2][c2] = 0
  st.undo[#st.undo + 1] = { r1, c1, r2, c2, v }
  if #st.undo > 48 then table.remove(st.undo, 1) end
  st.moves = st.moves + 1
  st.sel = nil
  st.proof = nil
  if Game.remaining(st.board) == 0 then st.won = true end
  return v
end

function Game.undo(st)
  if st.won or #st.undo == 0 then return false end
  local p = st.undo[#st.undo]
  st.undo[#st.undo] = nil
  st.board[p[1]][p[2]] = p[5]
  st.board[p[3]][p[4]] = p[5]
  st.moves = st.moves + 1
  st.sel = nil
  return true
end

function Game.shuffle(st, salt)
  if st.won then return false end
  local rows, cols = st.rows, st.cols
  local cells, vals = {}, {}
  for r = 1, rows do
    for c = 1, cols do
      local v = st.board[r][c]
      if v ~= 0 then
        cells[#cells + 1] = { r, c }
        vals[#vals + 1] = v
      end
    end
  end
  if #vals < 2 then return false end
  local backup = Game.copy_board(st.board)
  local rng = Game.rng((st.seed or 1) + (salt or 1) * 9973 + (st.shuffles + 1) * 17)
  for _ = 1, 8 do
    for i = #vals, 2, -1 do
      local j = (rng() % i) + 1
      vals[i], vals[j] = vals[j], vals[i]
    end
    for r = 1, rows do
      for c = 1, cols do st.board[r][c] = 0 end
    end
    local quota = {}
    for k = 1, st.kinds do quota[k] = 0 end
    for i = 1, #vals do quota[vals[i]] = quota[vals[i]] + 1 end
    local empties = {}
    for i = 1, #cells do empties[i] = { cells[i][1], cells[i][2] } end
    local placed, stuck = {}, false
    while #empties >= 2 do
      local n = #empties
      local anchor = (rng() % n) + 1
      local found = nil
      local guard = 0
      for step = 0, n - 1 do
        guard = guard + 1
        if guard > 64 then break end
        local j = ((anchor + step) % n) + 1
        if j ~= anchor then
          local a, b = empties[anchor], empties[j]
          if Game.route(rows, cols, st.board, a[1], a[2], b[1], b[2]) then
            found = j
            break
          end
        end
      end
      if not found then
        stuck = true
        break
      end
      local kind = take_kind(quota, rng)
      if not kind then
        stuck = true
        break
      end
      local a, b = empties[anchor], empties[found]
      st.board[a[1]][a[2]] = kind
      st.board[b[1]][b[2]] = kind
      placed[#placed + 1] = { a[1], a[2], b[1], b[2], kind }
      if found > anchor then
        remove_at(empties, found)
        remove_at(empties, anchor)
      else
        remove_at(empties, anchor)
        remove_at(empties, found)
      end
    end
    if not stuck and #empties == 0 then
      local check = Game.copy_board(st.board)
      local ok = true
      for i = #placed, 1, -1 do
        local p = placed[i]
        if not Game.can_link(rows, cols, check, p[1], p[2], p[3], p[4]) then
          ok = false
          break
        end
        check[p[1]][p[2]] = 0
        check[p[3]][p[4]] = 0
      end
      if ok and Game.remaining(check) == 0 then
        local proof = {}
        for i = #placed, 1, -1 do proof[#proof + 1] = placed[i] end
        st.shuffles = st.shuffles + 1
        st.sel = nil
        st.proof = proof
        return true
      end
    end
  end
  st.board = backup
  return false
end

function Game.pick(st, r, c)
  if st.won then return "won" end
  if r < 1 or c < 1 or r > st.rows or c > st.cols then return "outside" end
  st.cursor = { r, c }
  if st.board[r][c] == 0 then
    st.sel = nil
    return "empty"
  end
  if st.sel and st.sel[1] == r and st.sel[2] == c then
    st.sel = nil
    return "deselect"
  end
  if not st.sel then
    st.sel = { r, c }
    return "select"
  end
  local r1, c1 = st.sel[1], st.sel[2]
  if Game.can_link(st.rows, st.cols, st.board, r1, c1, r, c) then
    local path = Game.route(st.rows, st.cols, st.board, r1, c1, r, c)
    Game.clear_pair(st, r1, c1, r, c)
    return st.won and "win" or "match", path
  end
  st.sel = { r, c }
  return "reject"
end

function Game.use_hint(st)
  if st.won then return nil end
  local r1, c1, r2, c2 = Game.hint(st.rows, st.cols, st.board)
  if not r1 then return nil end
  st.hints = st.hints + 1
  st.sel = { r1, c1 }
  st.cursor = { r2, c2 }
  return r1, c1, r2, c2
end

function Game.serialize(st)
  local tiles = {}
  for r = 1, st.rows do
    for c = 1, st.cols do tiles[#tiles + 1] = tostring(st.board[r][c]) end
  end
  local undos = {}
  for i = 1, #st.undo do
    undos[i] = table.concat(st.undo[i], ",")
  end
  local sr, sc = 0, 0
  if st.sel then sr, sc = st.sel[1], st.sel[2] end
  return table.concat({
    st.level,
    tostring(st.seed),
    tostring(st.moves),
    tostring(st.hints),
    tostring(st.shuffles),
    st.won and "1" or "0",
    tostring(sr),
    tostring(sc),
    tostring(st.cursor[1]),
    tostring(st.cursor[2]),
    table.concat(tiles, ","),
    table.concat(undos, "|"),
  }, ";")
end

function Game.deserialize(s)
  if type(s) ~= "string" or s == "" then return nil end
  local parts = {}
  for tok in string.gmatch(s, "[^;]+") do parts[#parts + 1] = tok end
  -- undo may be empty; gmatch drops empty fields. Split manually.
  parts = {}
  local cur = ""
  for i = 1, #s + 1 do
    local ch = i <= #s and s:sub(i, i) or ";"
    if ch == ";" then
      parts[#parts + 1] = cur
      cur = ""
    else
      cur = cur .. ch
    end
  end
  if #parts < 12 then return nil end
  local level = parts[1]
  local spec = Game.LEVELS[level]
  if not spec then return nil end
  local nums = {}
  for tok in string.gmatch(parts[11], "[^,]+") do nums[#nums + 1] = tonumber(tok) end
  if #nums ~= spec.rows * spec.cols then return nil end
  local board = Game.blank(spec.rows, spec.cols)
  local i = 1
  for r = 1, spec.rows do
    for c = 1, spec.cols do
      local v = nums[i]
      if not v or v < 0 or v > spec.kinds then return nil end
      board[r][c] = v
      i = i + 1
    end
  end
  local undo = {}
  if parts[12] ~= "" then
    for tok in string.gmatch(parts[12], "[^|]+") do
      local a, b, c, d, v = tok:match("^(%d+),(%d+),(%d+),(%d+),(%d+)$")
      a, b, c, d, v = tonumber(a), tonumber(b), tonumber(c), tonumber(d), tonumber(v)
      if not a then return nil end
      undo[#undo + 1] = { a, b, c, d, v }
    end
  end
  local sr, sc = tonumber(parts[7]), tonumber(parts[8])
  local sel = nil
  if sr and sc and sr >= 1 and sc >= 1 then sel = { sr, sc } end
  return {
    level = level,
    seed = tonumber(parts[2]) or 1,
    rows = spec.rows,
    cols = spec.cols,
    kinds = spec.kinds,
    board = board,
    proof = nil,
    fallback = false,
    sel = sel,
    cursor = { tonumber(parts[9]) or 1, tonumber(parts[10]) or 1 },
    undo = undo,
    moves = tonumber(parts[3]) or 0,
    hints = tonumber(parts[4]) or 0,
    shuffles = tonumber(parts[5]) or 0,
    won = parts[6] == "1",
  }
end
