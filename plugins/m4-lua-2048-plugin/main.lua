-- com.m4.game2048 — minimal playable 2048 for 480x800 e-ink.
-- Host APIs: init/draw/onKey/onTouch, gui.*, sys.millis, fs.*, math.random.
-- Refresh: set frame_changed only when the board or overlay actually changes.
-- Host still full-refreshes on every Key/Touch; Tick/Draw honor frame_changed.

sys.load("game.lua")

screen = "game"
dirty = false
frame_changed = true
move_count = 0
FULL_FLASH_EVERY = 10  -- hint only; host has no partial refresh API yet

local function seed_rng()
  local t = 1
  if type(sys) == "table" and type(sys.millis) == "function" then
    t = sys.millis()
  end
  math.randomseed(t % 2147483646 + 1)
  math.random()
  math.random()
end

local function rng()
  return math.random()
end

local function persist()
  if type(fs) ~= "table" or type(fs.writeFile) ~= "function" then return end
  local ok, err = pcall(function()
    fs.writeFile("state.csv", Game.serialize(board, score, best, game_over))
  end)
  if not ok then
    log("2048 persist failed: " .. tostring(err))
  end
end

local function try_restore()
  if type(fs) ~= "table" or type(fs.readFile) ~= "function" then return false end
  local ok, data = pcall(function() return fs.readFile("state.csv") end)
  if not ok or type(data) ~= "string" or data == "" then return false end
  local st = Game.deserialize(data)
  if not st then return false end
  board = st.board
  score = st.score
  best = st.best
  game_over = st.game_over
  return true
end

function restart()
  board = Game.new_board()
  score = 0
  game_over = false
  move_count = 0
  Game.spawn(board, rng)
  Game.spawn(board, rng)
  persist()
  dirty = false
  frame_changed = true
end

function init()
  seed_rng()
  score = 0
  best = 0
  game_over = false
  board = Game.new_board()
  if not try_restore() then
    restart()
  else
    frame_changed = true
  end
end

local function apply_move(dir)
  if game_over then return end
  local nextb, delta, moved = Game.move(board, dir)
  if not moved then
    frame_changed = false
    return
  end
  board = nextb
  score = score + delta
  if score > best then best = score end
  Game.spawn(board, rng)
  game_over = Game.is_game_over(board)
  move_count = move_count + 1
  persist()
  frame_changed = true
end

-- Layout (480x800)
local W, H = 480, 800
local GRID_X, GRID_Y = 20, 88
local GRID = 440
local GAP = 8
local CELL = math.floor((GRID - GAP * 5) / 4)

local BTN = {
  up    = { x = 180, y = 560, w = 120, h = 56 },
  left  = { x = 52,  y = 624, w = 120, h = 56 },
  down  = { x = 180, y = 688, w = 120, h = 56 },
  right = { x = 308, y = 624, w = 120, h = 56 },
  new   = { x = 308, y = 560, w = 120, h = 56 },
}

local function hit(b, x, y)
  return x >= b.x and x < b.x + b.w and y >= b.y and y < b.y + b.h
end

local function draw_btn(b, label)
  gui.drawRect(b.x, b.y, b.w, b.h)
  local tw = gui.textWidth(16, label)
  local x = b.x + math.floor((b.w - tw) / 2)
  local y = b.y + math.floor((b.h - gui.lineHeight(16)) / 2)
  gui.drawText(16, x, y, label)
end

local function hatch_cell(x, y, w, h)
  local step = 6
  for d = 0, w + h, step do
    local x1 = x + d
    local y1 = y
    local x2 = x
    local y2 = y + d
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

function draw()
  gui.clear()
  gui.drawText(16, 16, 12, "2048")
  local score_s = "score " .. tostring(score)
  local best_s = "best " .. tostring(best)
  gui.drawText(12, 16, 44, score_s)
  local bw = gui.textWidth(12, best_s)
  gui.drawText(12, W - 16 - bw, 44, best_s)
  gui.drawLine(16, 72, W - 16, 72)

  gui.drawRect(GRID_X, GRID_Y, GRID, GRID)
  for r = 1, 4 do
    for c = 1, 4 do
      local x = GRID_X + GAP + (c - 1) * (CELL + GAP)
      local y = GRID_Y + GAP + (r - 1) * (CELL + GAP)
      local v = board[r][c]
      gui.drawRect(x, y, CELL, CELL)
      if v ~= 0 then
        if v >= 64 then
          hatch_cell(x + 2, y + 2, CELL - 4, CELL - 4)
        end
        local s = tostring(v)
        local font = (v >= 1024) and 12 or 16
        local tw = gui.textWidth(font, s)
        local th = gui.lineHeight(font)
        gui.drawText(font, x + math.floor((CELL - tw) / 2), y + math.floor((CELL - th) / 2), s)
      end
    end
  end

  draw_btn(BTN.up, "U")
  draw_btn(BTN.left, "L")
  draw_btn(BTN.down, "D")
  draw_btn(BTN.right, "R")
  draw_btn(BTN.new, "new")

  if game_over then
    gui.drawRect(20, 750, 440, 40)
    gui.drawText(12, 32, 760, "game over  confirm/new to restart")
  else
    gui.drawText(10, 20, 760, "keys U/D/L/R  or tap pad")
  end

  -- Do not call gui.refresh(): host displayBuffer() after draw.
  dirty = false
end

function onKey(key)
  if key == "confirm" then
    if game_over then
      restart()
    end
    return
  end
  if key == "left" or key == "right" or key == "up" or key == "down" then
    apply_move(key)
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
  if game_over then
    restart()
    return
  end
  if hit(BTN.up, x, y) then apply_move("up")
  elseif hit(BTN.down, x, y) then apply_move("down")
  elseif hit(BTN.left, x, y) then apply_move("left")
  elseif hit(BTN.right, x, y) then apply_move("right")
  end
end
