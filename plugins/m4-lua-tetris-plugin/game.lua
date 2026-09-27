-- Classic Tetris (ReKindle tetris.html / "Blocks").
-- Arena 12x20. Pieces ILJOTSZ. Drop 1000ms. Line score 10,20,40,80.
-- Cells: 0 empty, 1..7 piece ids. Positions 0-based like ReKindle.

Game = Game or {}

Game.COLS = 12
Game.ROWS = 20
Game.DROP_MS = 400
Game.PIECES = "ILJOTSZ"

function Game.empty_row()
  local r = {}
  for x = 1, Game.COLS do r[x] = 0 end
  return r
end

function Game.new_arena()
  local a = {}
  for y = 1, Game.ROWS do a[y] = Game.empty_row() end
  return a
end

function Game.copy_matrix(m)
  local out = {}
  for y = 1, #m do
    out[y] = {}
    for x = 1, #m[y] do
      out[y][x] = m[y][x]
    end
  end
  return out
end

function Game.create_piece(typ)
  if typ == "I" then
    return { {0,1,0,0}, {0,1,0,0}, {0,1,0,0}, {0,1,0,0} }
  elseif typ == "L" then
    return { {0,2,0}, {0,2,0}, {0,2,2} }
  elseif typ == "J" then
    return { {0,3,0}, {0,3,0}, {3,3,0} }
  elseif typ == "O" then
    return { {4,4}, {4,4} }
  elseif typ == "Z" then
    return { {5,5,0}, {0,5,5}, {0,0,0} }
  elseif typ == "S" then
    return { {0,6,6}, {6,6,0}, {0,0,0} }
  elseif typ == "T" then
    return { {0,7,0}, {7,7,7}, {0,0,0} }
  end
  return { {1} }
end

function Game.collide(arena, matrix, px, py)
  for y = 1, #matrix do
    for x = 1, #matrix[y] do
      if matrix[y][x] ~= 0 then
        local ax = px + (x - 1)
        local ay = py + (y - 1)
        if ay < 0 or ay >= Game.ROWS or ax < 0 or ax >= Game.COLS then
          return true
        end
        if arena[ay + 1][ax + 1] ~= 0 then
          return true
        end
      end
    end
  end
  return false
end

function Game.merge(arena, matrix, px, py)
  for y = 1, #matrix do
    for x = 1, #matrix[y] do
      local v = matrix[y][x]
      if v ~= 0 then
        arena[py + (y - 1) + 1][px + (x - 1) + 1] = v
      end
    end
  end
end

function Game.rotate(matrix, dir)
  local n = #matrix
  for y = 1, n do
    for x = 1, y - 1 do
      matrix[x][y], matrix[y][x] = matrix[y][x], matrix[x][y]
    end
  end
  if dir > 0 then
    for y = 1, n do
      local row = matrix[y]
      local i, j = 1, #row
      while i < j do
        row[i], row[j] = row[j], row[i]
        i = i + 1
        j = j - 1
      end
    end
  else
    local i, j = 1, n
    while i < j do
      matrix[i], matrix[j] = matrix[j], matrix[i]
      i = i + 1
      j = j - 1
    end
  end
end

function Game.spawn_x(matrix)
  return math.floor(Game.COLS / 2) - math.floor(#matrix[1] / 2)
end

-- rng_int(n) -> 1..n
function Game.random_type(rng_int)
  local i = 1
  if rng_int then i = rng_int(7) end
  return string.sub(Game.PIECES, i, i)
end

function Game.try_move(arena, matrix, px, py, dx, dy)
  local nx, ny = px + dx, py + dy
  if Game.collide(arena, matrix, nx, ny) then
    return px, py, false
  end
  return nx, ny, true
end

-- Soft drop one cell. Returns nx,ny,locked
function Game.drop(arena, matrix, px, py)
  local nx, ny, ok = Game.try_move(arena, matrix, px, py, 0, 1)
  if ok then return nx, ny, false end
  return px, py, true
end

-- Fall until collision; returns final x,y (not merged).
function Game.hard_drop(arena, matrix, px, py)
  while true do
    local nx, ny, ok = Game.try_move(arena, matrix, px, py, 0, 1)
    if not ok then return px, py end
    px, py = nx, ny
  end
end

function Game.try_rotate(arena, matrix, px, py, dir)
  Game.rotate(matrix, dir)
  local pos = px
  local offset = 1
  while Game.collide(arena, matrix, px, py) do
    px = px + offset
    if offset > 0 then
      offset = -(offset + 1)
    else
      offset = -(offset - 1)
    end
    if math.abs(offset) > #matrix[1] then
      Game.rotate(matrix, -dir)
      return pos, py, false
    end
  end
  return px, py, true
end

-- Returns new score. Mutates arena.
function Game.sweep(arena, score)
  local rowCount = 1
  local y = Game.ROWS
  while y >= 1 do
    local full = true
    for x = 1, Game.COLS do
      if arena[y][x] == 0 then
        full = false
        break
      end
    end
    if full then
      table.remove(arena, y)
      table.insert(arena, 1, Game.empty_row())
      score = score + rowCount * 10
      rowCount = rowCount * 2
    else
      y = y - 1
    end
  end
  return score
end

function Game.serialize(arena, score, best, over, px, py, matrix)
  local parts = { tostring(score or 0), tostring(best or 0), over and "1" or "0",
                  tostring(px or 0), tostring(py or 0) }
  local mh = matrix and #matrix or 0
  local mw = (mh > 0) and #matrix[1] or 0
  parts[#parts + 1] = tostring(mh)
  parts[#parts + 1] = tostring(mw)
  if matrix then
    for y = 1, mh do
      for x = 1, mw do
        parts[#parts + 1] = tostring(matrix[y][x] or 0)
      end
    end
  end
  for y = 1, Game.ROWS do
    for x = 1, Game.COLS do
      parts[#parts + 1] = tostring(arena[y][x] or 0)
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
  if #f < 7 then return nil end
  local score = tonumber(f[1])
  local best = tonumber(f[2])
  if not score or not best then return nil end
  local over = f[3] == "1"
  local px = tonumber(f[4])
  local py = tonumber(f[5])
  local mh = tonumber(f[6])
  local mw = tonumber(f[7])
  if not px or not py or not mh or not mw then return nil end
  local i = 8
  local matrix = nil
  if mh > 0 and mw > 0 then
    matrix = {}
    for y = 1, mh do
      matrix[y] = {}
      for x = 1, mw do
        local v = tonumber(f[i])
        if not v then return nil end
        matrix[y][x] = v
        i = i + 1
      end
    end
  end
  if #f - i + 1 ~= Game.ROWS * Game.COLS then return nil end
  local arena = {}
  for y = 1, Game.ROWS do
    arena[y] = {}
    for x = 1, Game.COLS do
      local v = tonumber(f[i])
      if not v then return nil end
      arena[y][x] = v
      i = i + 1
    end
  end
  return {
    arena = arena,
    score = score,
    best = best,
    over = over,
    px = px,
    py = py,
    matrix = matrix,
  }
end
