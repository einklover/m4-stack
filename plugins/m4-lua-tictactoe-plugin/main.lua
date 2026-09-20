-- com.m4.tictactoe — classic 3x3 vs CPU for 480x800 e-ink.
-- Rules from ReKindle tictactoe.html (human X, CPU O, easy/hard).
-- CPU plays in the same key/touch handler (no delayed timer on host).

sys.load("game.lua")

dirty = false
frame_changed = true
cpu_due = 0
CPU_DELAY_MS = 800

local W = 480
local GRID_X, GRID_Y = 20, 88
local GRID = 440
local GAP = 8
local CELL = math.floor((GRID - GAP * 4) / 3)

local BTN = {
  easy = { x = 20,  y = 620, w = 140, h = 56 },
  hard = { x = 170, y = 620, w = 140, h = 56 },
  new  = { x = 320, y = 620, w = 140, h = 56 },
}

local function rng_int(max)
  if max < 1 then return 1 end
  return math.random(1, max)
end

local function persist()
  if type(fs) ~= "table" or type(fs.writeFile) ~= "function" then return end
  local ok, err = pcall(function()
    fs.writeFile("state.csv", Game.serialize(board, difficulty, current, active, cursor))
  end)
  if not ok then
    log("ttt persist failed: " .. tostring(err))
  end
end

local function try_restore()
  if type(fs) ~= "table" or type(fs.readFile) ~= "function" then return false end
  local ok, data = pcall(function() return fs.readFile("state.csv") end)
  if not ok or type(data) ~= "string" or data == "" then return false end
  local st = Game.deserialize(data)
  if not st then return false end
  board = st.board
  difficulty = st.difficulty
  current = st.current
  active = st.active
  cursor = st.cursor
  win_line = Game.check_win(board)
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

function restart()
  board = Game.new_board()
  current = Game.HUMAN
  active = true
  cursor = 5
  win_line = nil
  cpu_due = 0
  dirty = false
  persist()
  frame_changed = true
end

function init()
  seed_rng()
  difficulty = "hard"
  if not try_restore() then
    restart()
  else
    frame_changed = true
    if active and current == Game.CPU then
      schedule_cpu()
    end
  end
end

local function now_ms()
  if type(sys) == "table" and type(sys.millis) == "function" then
    return sys.millis()
  end
  return 0
end

function schedule_cpu()
  cpu_due = now_ms() + CPU_DELAY_MS
  dirty = true
end

local function after_place()
  win_line = Game.check_win(board)
  if win_line then
    active = false
    persist()
    frame_changed = true
    return
  end
  if Game.is_draw(board) then
    active = false
    persist()
    frame_changed = true
    return
  end
  persist()
  frame_changed = true
end

local function play_cpu()
  if not active or current ~= Game.CPU then return end
  local i = Game.cpu_move(board, difficulty, rng_int)
  if i ~= -1 then
    Game.place(board, i, Game.CPU)
  end
  win_line = Game.check_win(board)
  if win_line or Game.is_draw(board) then
    active = false
  else
    current = Game.HUMAN
  end
  persist()
  frame_changed = true
  dirty = false
  cpu_due = 0
end

local function maybe_cpu()
  if not active or current ~= Game.CPU or cpu_due == 0 then return end
  if now_ms() < cpu_due then
    dirty = true
    frame_changed = false
    return
  end
  play_cpu()
end

local function human_place(index)
  if not Game.can_place(board, index, active, current) then
    frame_changed = false
    return
  end
  Game.place(board, index, Game.HUMAN)
  cursor = index
  after_place()
  if not active then return end
  current = Game.CPU
  persist()
  schedule_cpu()
end

local function hit(b, x, y)
  return x >= b.x and x < b.x + b.w and y >= b.y and y < b.y + b.h
end

local function cell_at(x, y)
  for r = 1, 3 do
    for c = 1, 3 do
      local i = (r - 1) * 3 + c
      local cx = GRID_X + GAP + (c - 1) * (CELL + GAP)
      local cy = GRID_Y + GAP + (r - 1) * (CELL + GAP)
      if x >= cx and x < cx + CELL and y >= cy and y < cy + CELL then
        return i
      end
    end
  end
  return nil
end

local function draw_btn(b, label, on)
  gui.drawRect(b.x, b.y, b.w, b.h)
  if on then
    gui.drawRect(b.x + 3, b.y + 3, b.w - 6, b.h - 6)
  end
  local tw = gui.textWidth(16, label)
  local x = b.x + math.floor((b.w - tw) / 2)
  local y = b.y + math.floor((b.h - gui.lineHeight(16)) / 2)
  gui.drawText(16, x, y, label)
end

local function hatch(x, y, w, h)
  local step = 8
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

local function in_win(i)
  if not win_line then return false end
  return win_line[1] == i or win_line[2] == i or win_line[3] == i
end

local function status_text()
  if not active then
    if win_line then
      if board[win_line[1]] == Game.HUMAN then return "X wins  confirm/new" end
      return "CPU wins  confirm/new"
    end
    return "draw  confirm/new"
  end
  if current == Game.CPU then return "CPU…" end
  if difficulty == "easy" then return "your turn (X)  easy" end
  return "your turn (X)  hard"
end

function draw()
  maybe_cpu()
  gui.clear()
  gui.drawText(16, 16, 12, "Tic-Tac-Toe")
  local st = status_text()
  gui.drawText(12, 16, 44, st)
  gui.drawLine(16, 72, W - 16, 72)

  gui.drawRect(GRID_X, GRID_Y, GRID, GRID)
  for r = 1, 3 do
    for c = 1, 3 do
      local i = (r - 1) * 3 + c
      local x = GRID_X + GAP + (c - 1) * (CELL + GAP)
      local y = GRID_Y + GAP + (r - 1) * (CELL + GAP)
      gui.drawRect(x, y, CELL, CELL)
      if in_win(i) then
        hatch(x + 4, y + 4, CELL - 8, CELL - 8)
      end
      if cursor == i then
        gui.drawRect(x + 6, y + 6, CELL - 12, CELL - 12)
      end
      local mark = board[i]
      if mark then
        local tw = gui.textWidth(16, mark)
        local th = gui.lineHeight(16)
        gui.drawText(16, x + math.floor((CELL - tw) / 2), y + math.floor((CELL - th) / 2), mark)
      end
    end
  end

  draw_btn(BTN.easy, "easy", difficulty == "easy")
  draw_btn(BTN.hard, "hard", difficulty == "hard")
  draw_btn(BTN.new, "new", false)

  gui.drawText(10, 20, 700, "keys U/D/L/R  confirm place")
  gui.drawText(10, 20, 724, "or tap a cell")
  dirty = false
end

local function move_cursor(dr, dc)
  local r = math.floor((cursor - 1) / 3) + 1
  local c = ((cursor - 1) % 3) + 1
  r = r + dr
  c = c + dc
  if r < 1 then r = 1 end
  if r > 3 then r = 3 end
  if c < 1 then c = 1 end
  if c > 3 then c = 3 end
  local n = (r - 1) * 3 + c
  if n ~= cursor then
    cursor = n
    frame_changed = true
  else
    frame_changed = false
  end
end

function onKey(key)
  if key == "confirm" then
    if not active then
      restart()
      return
    end
    human_place(cursor)
    return
  end
  if key == "up" then move_cursor(-1, 0)
  elseif key == "down" then move_cursor(1, 0)
  elseif key == "left" then move_cursor(0, -1)
  elseif key == "right" then move_cursor(0, 1)
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
  if hit(BTN.easy, x, y) then
    difficulty = "easy"
    restart()
    return
  end
  if hit(BTN.hard, x, y) then
    difficulty = "hard"
    restart()
    return
  end
  local i = cell_at(x, y)
  if i then
    if not active then
      restart()
      return
    end
    human_place(i)
  end
end
