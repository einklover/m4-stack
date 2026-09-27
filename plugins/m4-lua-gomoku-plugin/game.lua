-- Freestyle gomoku: 15x15, five in a row. Black (1) first, White (2).
-- No ReKindle original; Connect4's 5-in-row is a different game.

Game = Game or {}
Game.N = 15
Game.BLACK, Game.WHITE = 1, 2
Game.HUMAN, Game.CPU = 1, 2

function Game.new_board()
  local b = {}
  for r = 1, Game.N do
    b[r] = {}
    for c = 1, Game.N do b[r][c] = 0 end
  end
  return b
end

function Game.in_bounds(r, c)
  return r >= 1 and r <= Game.N and c >= 1 and c <= Game.N
end

function Game.count_dir(board, r, c, dr, dc, who)
  local n = 0
  local rr, cc = r + dr, c + dc
  while Game.in_bounds(rr, cc) and board[rr][cc] == who do
    n = n + 1
    rr, cc = rr + dr, cc + dc
  end
  return n
end

function Game.line_at(board, r, c)
  local who = board[r][c]
  if who == 0 then return false end
  local dirs = { { 1, 0 }, { 0, 1 }, { 1, 1 }, { 1, -1 } }
  for i = 1, 4 do
    local dr, dc = dirs[i][1], dirs[i][2]
    local n = 1 + Game.count_dir(board, r, c, dr, dc, who)
      + Game.count_dir(board, r, c, -dr, -dc, who)
    if n >= 5 then return true, who end
  end
  return false
end

function Game.is_full(board)
  for r = 1, Game.N do
    for c = 1, Game.N do
      if board[r][c] == 0 then return false end
    end
  end
  return true
end

function Game.candidates(board)
  local seen, out = {}, {}
  local has = false
  for r = 1, Game.N do
    for c = 1, Game.N do
      if board[r][c] ~= 0 then
        has = true
        for dr = -1, 1 do
          for dc = -1, 1 do
            local rr, cc = r + dr, c + dc
            if Game.in_bounds(rr, cc) and board[rr][cc] == 0 then
              local k = rr * 32 + cc
              if not seen[k] then
                seen[k] = true
                out[#out + 1] = { rr, cc }
              end
            end
          end
        end
      end
    end
  end
  if not has then
    local m = math.floor((Game.N + 1) / 2)
    out[1] = { m, m }
  end
  return out
end

local function would_win(board, r, c, who)
  board[r][c] = who
  local ok = Game.line_at(board, r, c)
  board[r][c] = 0
  return ok
end

-- Open-ended run length score for a hypothetical stone.
function Game.cell_score(board, r, c, who)
  local opp = 3 - who
  local s = 0
  local dirs = { { 1, 0 }, { 0, 1 }, { 1, 1 }, { 1, -1 } }
  for i = 1, 4 do
    local dr, dc = dirs[i][1], dirs[i][2]
    local a = Game.count_dir(board, r, c, dr, dc, who)
    local b = Game.count_dir(board, r, c, -dr, -dc, who)
    local run = a + b
    if run >= 4 then s = s + 100000
    elseif run == 3 then s = s + 5000
    elseif run == 2 then s = s + 200
    elseif run == 1 then s = s + 20
    end
    local oa = Game.count_dir(board, r, c, dr, dc, opp)
    local ob = Game.count_dir(board, r, c, -dr, -dc, opp)
    local orun = oa + ob
    if orun >= 4 then s = s + 80000
    elseif orun == 3 then s = s + 4000
    elseif orun == 2 then s = s + 80
    end
  end
  local m = math.floor((Game.N + 1) / 2)
  s = s + (8 - math.abs(r - m) - math.abs(c - m))
  return s
end

function Game.cpu_move(board)
  local cand = Game.candidates(board)
  for i = 1, #cand do
    local r, c = cand[i][1], cand[i][2]
    if would_win(board, r, c, Game.CPU) then return r, c end
  end
  for i = 1, #cand do
    local r, c = cand[i][1], cand[i][2]
    if would_win(board, r, c, Game.HUMAN) then return r, c end
  end
  local br, bc, best = cand[1][1], cand[1][2], -1
  for i = 1, #cand do
    local r, c = cand[i][1], cand[i][2]
    local sc = Game.cell_score(board, r, c, Game.CPU)
    if sc > best then
      best = sc
      br, bc = r, c
    end
  end
  return br, bc
end

function Game.serialize(st)
  local t = {
    tostring(st.cur_r), tostring(st.cur_c),
    tostring(st.current), st.active and "1" or "0",
    st.won or "0",
  }
  local cells = {}
  for r = 1, Game.N do
    for c = 1, Game.N do
      cells[#cells + 1] = tostring(st.board[r][c])
    end
  end
  return table.concat(t, ",") .. "\n" .. table.concat(cells, "")
end

function Game.deserialize(s)
  if type(s) ~= "string" then return nil end
  local hdr, body = s:match("^([^\n]+)\n(.+)$")
  if not hdr or not body or #body < Game.N * Game.N then return nil end
  local cr, cc, cur, act, won = hdr:match("^(%d+),(%d+),(%d+),([01]),([^,]+)$")
  if not cr then return nil end
  local board = Game.new_board()
  local k = 1
  for r = 1, Game.N do
    for c = 1, Game.N do
      board[r][c] = tonumber(body:sub(k, k)) or 0
      k = k + 1
    end
  end
  return {
    cur_r = tonumber(cr), cur_c = tonumber(cc),
    current = tonumber(cur),
    active = act == "1",
    won = won,
    board = board,
  }
end
