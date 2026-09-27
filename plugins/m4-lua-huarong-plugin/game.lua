-- 华容道 / Klotski. Classic 横刀立马 on a 4x5 board.
-- Cao Cao is 2x2; win when his top-left is row 4, col 2 (bottom exit).

Game = Game or {}
Game.ROWS, Game.COLS = 5, 4

function Game.classic()
  -- id, name, w, h, r, c (top-left, 1-indexed)
  return {
    { id = 1, name = "曹", w = 2, h = 2, r = 1, c = 2 },
    { id = 2, name = "关", w = 2, h = 1, r = 3, c = 2 },
    { id = 3, name = "赵", w = 1, h = 2, r = 1, c = 1 },
    { id = 4, name = "黄", w = 1, h = 2, r = 1, c = 4 },
    { id = 5, name = "张", w = 1, h = 2, r = 3, c = 1 },
    { id = 6, name = "马", w = 1, h = 2, r = 3, c = 4 },
    { id = 7, name = "卒", w = 1, h = 1, r = 4, c = 2 },
    { id = 8, name = "卒", w = 1, h = 1, r = 4, c = 3 },
    { id = 9, name = "卒", w = 1, h = 1, r = 5, c = 1 },
    { id = 10, name = "卒", w = 1, h = 1, r = 5, c = 4 },
  }
end

function Game.find(pieces, id)
  for i = 1, #pieces do
    if pieces[i].id == id then return pieces[i] end
  end
  return nil
end

function Game.grid(pieces)
  local g = {}
  for r = 1, Game.ROWS do
    g[r] = {}
    for c = 1, Game.COLS do g[r][c] = 0 end
  end
  for _, p in ipairs(pieces) do
    for dr = 0, p.h - 1 do
      for dc = 0, p.w - 1 do
        g[p.r + dr][p.c + dc] = p.id
      end
    end
  end
  return g
end

function Game.can_move(pieces, id, dr, dc)
  if (dr ~= 0 and dc ~= 0) or (dr == 0 and dc == 0) then return false end
  if math.abs(dr) + math.abs(dc) ~= 1 then return false end
  local p = Game.find(pieces, id)
  if not p then return false end
  local g = Game.grid(pieces)
  local nr, nc = p.r + dr, p.c + dc
  if nr < 1 or nc < 1 or nr + p.h - 1 > Game.ROWS or nc + p.w - 1 > Game.COLS then
    return false
  end
  for rr = 0, p.h - 1 do
    for cc = 0, p.w - 1 do
      local occ = g[nr + rr][nc + cc]
      if occ ~= 0 and occ ~= id then return false end
    end
  end
  return true
end

function Game.move(pieces, id, dr, dc)
  if not Game.can_move(pieces, id, dr, dc) then return false end
  local p = Game.find(pieces, id)
  p.r = p.r + dr
  p.c = p.c + dc
  return true
end

function Game.is_win(pieces)
  local cao = Game.find(pieces, 1)
  return cao and cao.r == 4 and cao.c == 2
end

function Game.at(pieces, r, c)
  if r < 1 or c < 1 or r > Game.ROWS or c > Game.COLS then return nil end
  local g = Game.grid(pieces)
  local id = g[r][c]
  if id == 0 then return nil end
  return Game.find(pieces, id)
end

function Game.serialize(st)
  local t = { tostring(st.sel), tostring(st.moves), st.won and "1" or "0" }
  for _, p in ipairs(st.pieces) do
    t[#t + 1] = table.concat({ p.id, p.r, p.c }, ",")
  end
  return table.concat(t, ";")
end

function Game.deserialize(s)
  if type(s) ~= "string" then return nil end
  local parts = {}
  for tok in s:gmatch("[^;]+") do parts[#parts + 1] = tok end
  if #parts < 13 then return nil end
  local pieces = Game.classic()
  local byid = {}
  for _, p in ipairs(pieces) do byid[p.id] = p end
  for i = 4, #parts do
    local id, r, c = parts[i]:match("^(%d+),(%d+),(%d+)$")
    id, r, c = tonumber(id), tonumber(r), tonumber(c)
    if not id or not byid[id] then return nil end
    byid[id].r, byid[id].c = r, c
  end
  return {
    sel = tonumber(parts[1]),
    moves = tonumber(parts[2]),
    won = parts[3] == "1",
    pieces = pieces,
  }
end
