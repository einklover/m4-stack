-- com.m4.gomoku — 15x15 五子棋 vs CPU on 480x800 e-ink.

sys.load("game.lua")

dirty = false
frame_changed = true
cpu_due = 0
CPU_DELAY_MS = 400

local W, H = 480, 800
local TITLE_H = 36
local BTN_H = 52
local N = Game.N

local function now_ms()
  if type(sys) == "table" and type(sys.millis) == "function" then
    return sys.millis()
  end
  return 0
end

local function layout()
  local by = H - BTN_H
  -- 15 lines, stones sit on intersections (Go).
  CELL = math.max(12, math.min(
    math.floor((W - 28) / (N - 1)),
    math.floor((by - TITLE_H - 28) / (N - 1))))
  GRID = CELL * (N - 1)
  local rad = math.floor(CELL * 0.42)
  GRID_X = math.floor((W - GRID) / 2)
  GRID_Y = TITLE_H + math.floor((by - 8 - TITLE_H - GRID) / 2)
  if GRID_X < rad + 4 then GRID_X = rad + 4 end
  if GRID_Y < TITLE_H + rad then GRID_Y = TITLE_H + rad end
  STONE_R = rad
  BTN = { new = { x = 8, y = by, w = W - 16, h = BTN_H } }
end

local function pt(r, c)
  return GRID_X + (c - 1) * CELL, GRID_Y + (r - 1) * CELL
end

local function fill_disk(cx, cy, rad)
  local r2 = rad * rad
  for dy = -rad, rad do
    local t = r2 - dy * dy
    if t >= 0 then
      local w = math.floor(math.sqrt(t) + 0.5)
      if w >= 0 then
        gui.fillRect(cx - w, cy + dy, w * 2 + 1, 1)
      end
    end
  end
end

local function punch_disk(cx, cy, rad)
  if type(gui.fillRectDither) ~= "function" then return end
  local r2 = rad * rad
  for dy = -rad, rad do
    local t = r2 - dy * dy
    if t >= 0 then
      local w = math.floor(math.sqrt(t) + 0.5)
      gui.fillRectDither(cx - w, cy + dy, w * 2 + 1, 1, "white")
    end
  end
end

local function persist()
  if type(fs) ~= "table" or type(fs.writeFile) ~= "function" then return end
  pcall(function()
    fs.writeFile("state.csv", Game.serialize({
      cur_r = cur_r, cur_c = cur_c, current = current,
      active = active, won = won, board = board,
    }))
  end)
end

function restart()
  board = Game.new_board()
  cur_r = math.floor((N + 1) / 2)
  cur_c = cur_r
  current = Game.HUMAN
  active = true
  won = "0"
  cpu_due = 0
  dirty = false
  persist()
  frame_changed = true
end

local function try_restore()
  if type(fs) ~= "table" or type(fs.readFile) ~= "function" then return false end
  local ok, data = pcall(function() return fs.readFile("state.csv") end)
  if not ok or type(data) ~= "string" or data == "" then return false end
  local st = Game.deserialize(data)
  if not st then return false end
  board, cur_r, cur_c = st.board, st.cur_r, st.cur_c
  current, active, won = st.current, st.active, st.won
  cpu_due = 0
  dirty = false
  return true
end

function init()
  layout()
  if not try_restore() then
    restart()
  else
    frame_changed = true
    if active and current == Game.CPU then
      cpu_due = now_ms() + CPU_DELAY_MS
      dirty = true
    end
  end
end

local function schedule_cpu()
  cpu_due = now_ms() + CPU_DELAY_MS
  dirty = true
end

local function after_stone(r, c)
  if Game.line_at(board, r, c) then
    active = false
    won = tostring(board[r][c])
  elseif Game.is_full(board) then
    active = false
    won = "d"
  end
end

local function play_cpu()
  if not active or current ~= Game.CPU then
    dirty = false
    cpu_due = 0
    return
  end
  local r, c = Game.cpu_move(board)
  board[r][c] = Game.CPU
  cur_r, cur_c = r, c
  after_stone(r, c)
  if active then current = Game.HUMAN end
  persist()
  frame_changed = true
  dirty = false
  cpu_due = 0
end

function draw()
  local cpu_played = false
  if active and current == Game.CPU and cpu_due ~= 0 then
    if now_ms() >= cpu_due then
      play_cpu()
      cpu_played = true
    else
      dirty = true
    end
  end

  gui.clear()
  local st
  if won == "1" then st = "黑胜"
  elseif won == "2" then st = "白胜"
  elseif won == "d" then st = "和棋"
  elseif current == Game.CPU then st = "白思考"
  else st = "黑下"
  end
  gui.drawText(16, 8, 8, "五子棋  " .. st)

  for i = 0, N - 1 do
    local x = GRID_X + i * CELL
    local y = GRID_Y + i * CELL
    gui.drawLine(x, GRID_Y, x, GRID_Y + GRID)
    gui.drawLine(GRID_X, y, GRID_X + GRID, y)
  end
  for _, h in ipairs({ 4, 8, 12 }) do
    for _, v in ipairs({ 4, 8, 12 }) do
      local hx, hy = pt(h, v)
      gui.fillRect(hx - 1, hy - 1, 3, 3)
    end
  end
  for r = 1, N do
    for c = 1, N do
      local v = board[r][c]
      if v ~= 0 then
        local x, y = pt(r, c)
        if v == Game.BLACK then
          fill_disk(x, y, STONE_R)
        else
          fill_disk(x, y, STONE_R)
          punch_disk(x, y, math.max(2, STONE_R - 2))
        end
      end
    end
  end
  local cx, cy = pt(cur_r, cur_c)
  gui.drawRect(cx - STONE_R - 2, cy - STONE_R - 2, STONE_R * 2 + 5, STONE_R * 2 + 5)

  gui.drawRect(BTN.new.x, BTN.new.y, BTN.new.w, BTN.new.h)
  local lab = "new"
  local tw = gui.textWidth(12, lab)
  gui.drawText(12, BTN.new.x + math.floor((BTN.new.w - tw) / 2),
    BTN.new.y + math.floor((BTN.new.h - gui.lineHeight(12)) / 2), lab)

  if cpu_played then
    frame_changed = true
  else
    frame_changed = false
  end
end

local function human_place(r, c)
  if not active or current ~= Game.HUMAN then
    frame_changed = false
    return
  end
  if not Game.in_bounds(r, c) or board[r][c] ~= 0 then
    frame_changed = false
    return
  end
  board[r][c] = Game.HUMAN
  cur_r, cur_c = r, c
  after_stone(r, c)
  if active then
    current = Game.CPU
    persist()
    schedule_cpu()
  else
    persist()
  end
  frame_changed = true
end

function onKey(key)
  if key == "confirm" then
    if not active then restart() return end
    human_place(cur_r, cur_c)
    return
  end
  local nr, nc = cur_r, cur_c
  if key == "up" then nr = nr - 1
  elseif key == "down" then nr = nr + 1
  elseif key == "left" then nc = nc - 1
  elseif key == "right" then nc = nc + 1
  else return
  end
  if nr < 1 then nr = 1 end
  if nr > N then nr = N end
  if nc < 1 then nc = 1 end
  if nc > N then nc = N end
  if nr ~= cur_r or nc ~= cur_c then
    cur_r, cur_c = nr, nc
    frame_changed = true
  else
    frame_changed = false
  end
end

function onTouch(x, y, phase)
  if phase and phase ~= "tap" then return end
  if x >= BTN.new.x and x < BTN.new.x + BTN.new.w
      and y >= BTN.new.y and y < BTN.new.y + BTN.new.h then
    restart()
    return
  end
  local half = math.floor(CELL / 2)
  if x < GRID_X - half or y < GRID_Y - half
      or x > GRID_X + GRID + half or y > GRID_Y + GRID + half then
    return
  end
  local c = math.floor((x - GRID_X) / CELL + 0.5) + 1
  local r = math.floor((y - GRID_Y) / CELL + 0.5) + 1
  if not active then restart() return end
  human_place(r, c)
end
