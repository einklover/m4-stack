-- com.m4.tetris — ReKindle Blocks on 480x800 e-ink.
-- Gravity uses dirty Tick (~50ms pump) + sys.millis; drop every DROP_MS.
-- Drop button/key hard-drops to the floor (soft one-cell was invisible on e-ink).

sys.load("game.lua")

dirty = false
frame_changed = true
skip_gravity = false

local W, H = 480, 800
local COLS, ROWS = Game.COLS, Game.ROWS
local TITLE_H = 36
local BTN_H = 56
local CELL = math.floor(math.min((W - 8) / COLS, (H - TITLE_H - BTN_H - 8) / ROWS))
local GRID_W, GRID_H = COLS * CELL, ROWS * CELL
local GRID_X = math.floor((W - GRID_W) / 2)
local GRID_Y = TITLE_H
local BTN_Y = GRID_Y + GRID_H + 4
local BTN_W = math.floor((W - 24) / 5)

local BTN = {
  rot   = { x = 8,              y = BTN_Y, w = BTN_W, h = BTN_H },
  left  = { x = 8 + BTN_W,      y = BTN_Y, w = BTN_W, h = BTN_H },
  drop  = { x = 8 + BTN_W * 2,  y = BTN_Y, w = BTN_W, h = BTN_H },
  right = { x = 8 + BTN_W * 3,  y = BTN_Y, w = BTN_W, h = BTN_H },
  new   = { x = 8 + BTN_W * 4,  y = BTN_Y, w = BTN_W, h = BTN_H },
}

local function now_ms()
  if type(sys) == "table" and type(sys.millis) == "function" then
    return sys.millis()
  end
  return 0
end

local function rng_int(max)
  if max < 1 then return 1 end
  return math.random(1, max)
end

local function persist()
  if type(fs) ~= "table" or type(fs.writeFile) ~= "function" then return end
  pcall(function()
    fs.writeFile("state.csv", Game.serialize(arena, score, best, over, px, py, matrix))
  end)
end

local function spawn()
  matrix = Game.create_piece(Game.random_type(rng_int))
  py = 0
  px = Game.spawn_x(matrix)
  if Game.collide(arena, matrix, px, py) then
    over = true
    dirty = false
    if score > best then best = score end
  end
  persist()
  frame_changed = true
end

function restart()
  arena = Game.new_arena()
  score = 0
  over = false
  drop_due = now_ms() + Game.DROP_MS
  dirty = true
  spawn()
end

local function try_restore()
  if type(fs) ~= "table" or type(fs.readFile) ~= "function" then return false end
  local ok, data = pcall(function() return fs.readFile("state.csv") end)
  if not ok or type(data) ~= "string" or data == "" then return false end
  local st = Game.deserialize(data)
  if not st then return false end
  arena = st.arena
  score = st.score
  best = st.best
  over = st.over
  px, py, matrix = st.px, st.py, st.matrix
  if not matrix then spawn() end
  return true
end

local function seed_rng()
  local t = 1
  if type(sys) == "table" and type(sys.millis) == "function" then
    t = sys.millis()
  end
  math.randomseed(t % 2147483646 + 1)
  math.random()
  math.random()
end

function init()
  seed_rng()
  best = 0
  arena = Game.new_arena()
  score = 0
  over = false
  if not try_restore() then
    restart()
  else
    drop_due = now_ms() + Game.DROP_MS
    dirty = not over
    frame_changed = true
  end
end

local function lock_piece()
  Game.merge(arena, matrix, px, py)
  score = Game.sweep(arena, score)
  if score > best then best = score end
  spawn()
  drop_due = now_ms() + Game.DROP_MS
end

local function do_soft_drop()
  if over then return end
  skip_gravity = true
  local nx, ny, locked = Game.drop(arena, matrix, px, py)
  px, py = nx, ny
  if locked then
    lock_piece()
  else
    frame_changed = true
    persist()
  end
  drop_due = now_ms() + Game.DROP_MS
end

local function do_hard_drop()
  if over or not matrix then return end
  skip_gravity = true
  px, py = Game.hard_drop(arena, matrix, px, py)
  lock_piece()
  frame_changed = true
end

local function maybe_gravity()
  if skip_gravity then
    skip_gravity = false
    dirty = not over
    return
  end
  if over then
    dirty = false
    return
  end
  dirty = true
  if now_ms() < drop_due then
    frame_changed = false
    return
  end
  do_soft_drop()
end

local function do_move(dx)
  if over then return end
  skip_gravity = true
  local nx, ny, ok = Game.try_move(arena, matrix, px, py, dx, 0)
  if ok then
    px, py = nx, ny
    frame_changed = true
    persist()
  else
    frame_changed = false
  end
end

local function do_rotate()
  if over then return end
  skip_gravity = true
  local nx, ny, ok = Game.try_rotate(arena, matrix, px, py, 1)
  if ok then
    px, py = nx, ny
    frame_changed = true
    persist()
  else
    frame_changed = false
  end
end

local function hatch(x, y, w, h)
  local step = 6
  for d = 0, w + h, step do
    local x1, y1, x2, y2 = x + d, y, x, y + d
    if x1 > x + w then
      y1 = y + (x1 - (x + w))
      x1 = x + w
    end
    if y2 > y + h then
      x2 = x + (y2 - (y + h))
      y2 = y + h
    end
    if x2 >= x and x1 <= x + w then
      gui.drawLine(x1, y1, x2, y2)
    end
  end
end

local function cell_xy(c, r)
  return GRID_X + (c - 1) * CELL, GRID_Y + (r - 1) * CELL
end

local function draw_btn(b, label)
  gui.drawRect(b.x, b.y, b.w, b.h)
  local tw = gui.textWidth(16, label)
  local x = b.x + math.floor((b.w - tw) / 2)
  local y = b.y + math.floor((b.h - gui.lineHeight(16)) / 2)
  gui.drawText(16, x, y, label)
end

local function hit(b, x, y)
  return x >= b.x and x < b.x + b.w and y >= b.y and y < b.y + b.h
end

function draw()
  maybe_gravity()
  gui.clear()
  local st = tostring(score) .. "/" .. tostring(best)
  if over then st = "over " .. st end
  gui.drawText(16, 8, 8, "Tetris  " .. st)
  gui.drawRect(GRID_X - 1, GRID_Y - 1, GRID_W + 2, GRID_H + 2)
  for r = 1, ROWS do
    for c = 1, COLS do
      local v = arena[r][c]
      if v ~= 0 then
        local x, y = cell_xy(c, r)
        gui.drawRect(x, y, CELL - 1, CELL - 1)
        hatch(x + 2, y + 2, CELL - 5, CELL - 5)
      end
    end
  end
  if matrix and not over then
    for y = 1, #matrix do
      for x = 1, #matrix[y] do
        if matrix[y][x] ~= 0 then
          local c = px + (x - 1) + 1
          local r = py + (y - 1) + 1
          if r >= 1 and r <= ROWS and c >= 1 and c <= COLS then
            local dx, dy = cell_xy(c, r)
            gui.drawRect(dx, dy, CELL - 1, CELL - 1)
          end
        end
      end
    end
  end
  draw_btn(BTN.rot, "rot")
  draw_btn(BTN.left, "<")
  draw_btn(BTN.drop, "drop")
  draw_btn(BTN.right, ">")
  draw_btn(BTN.new, "new")
end

function onKey(key)
  if over then
    if key == "confirm" then restart() end
    return
  end
  if key == "left" then do_move(-1)
  elseif key == "right" then do_move(1)
  elseif key == "down" then do_hard_drop()
  elseif key == "up" or key == "confirm" then do_rotate()
  end
end

function onTouch(x, y, phase)
  if phase and phase ~= "tap" and phase ~= "up" and phase ~= "press" then
    return
  end
  if hit(BTN.new, x, y) then
    restart()
    return
  end
  if over then
    restart()
    return
  end
  if hit(BTN.left, x, y) then do_move(-1)
  elseif hit(BTN.right, x, y) then do_move(1)
  elseif hit(BTN.drop, x, y) then do_hard_drop()
  elseif hit(BTN.rot, x, y) then do_rotate()
  elseif x >= GRID_X and x < GRID_X + GRID_W and y >= GRID_Y and y < GRID_Y + GRID_H then
    do_hard_drop()
  end
end
