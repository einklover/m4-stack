-- com.m4.minesweeper — ReKindle minesweeper on 480x800 e-ink.

sys.load("game.lua")

dirty = false
frame_changed = true

local W, H = 480, 800
local TITLE_H = 36
local BTN_H = 52

local function rng_int(max)
  if max < 1 then return 1 end
  return math.random(1, max)
end

local function now_ms()
  if type(sys) == "table" and type(sys.millis) == "function" then
    return sys.millis()
  end
  return 0
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

local function layout()
  local max_w = math.floor((W - 8) / cols)
  local max_h = math.floor((H - TITLE_H - BTN_H - 8) / rows)
  CELL = math.max(12, math.min(max_w, max_h))
  CELL_W, CELL_H = CELL, CELL
  GRID_W, GRID_H = cols * CELL, rows * CELL
  GRID_X = math.floor((W - GRID_W) / 2)
  GRID_Y = TITLE_H + math.floor((H - TITLE_H - BTN_H - 4 - GRID_H) / 2)
  local by = H - BTN_H
  local bw = math.floor((W - 16) / 5)
  BTN = {
    mode = { x = 8, y = by, w = bw, h = BTN_H },
    easy = { x = 8 + bw, y = by, w = bw, h = BTN_H },
    med  = { x = 8 + bw * 2, y = by, w = bw, h = BTN_H },
    hard = { x = 8 + bw * 3, y = by, w = bw, h = BTN_H },
    new  = { x = 8 + bw * 4, y = by, w = bw, h = BTN_H },
  }
end

local function persist()
  if type(fs) ~= "table" or type(fs.writeFile) ~= "function" then return end
  pcall(function()
    fs.writeFile("state.csv", Game.serialize({
      diff = diff, rows = rows, cols = cols, mines = mines,
      first = first, over = over, won = won, mode = mode,
      cursor_r = cursor_r, cursor_c = cursor_c, grid = grid,
    }))
  end)
end

function restart(new_diff)
  if new_diff then diff = new_diff end
  local d = Game.DIFF[diff] or Game.DIFF.medium
  rows, cols, mines = d.rows, d.cols, d.mines
  grid = Game.new_grid(rows, cols)
  first = true
  over = false
  won = false
  mode = "dig"
  cursor_r = math.floor((rows + 1) / 2)
  cursor_c = math.floor((cols + 1) / 2)
  layout()
  persist()
  frame_changed = true
end

local function try_restore()
  if type(fs) ~= "table" or type(fs.readFile) ~= "function" then return false end
  local ok, data = pcall(function() return fs.readFile("state.csv") end)
  if not ok or type(data) ~= "string" or data == "" then return false end
  local st = Game.deserialize(data)
  if not st then return false end
  local spec = Game.DIFF[st.diff]
  if not spec or st.rows ~= spec.rows or st.cols ~= spec.cols then
    return false
  end
  diff, rows, cols, mines = st.diff, st.rows, st.cols, st.mines
  first, over, won, mode = st.first, st.over, st.won, st.mode
  cursor_r, cursor_c, grid = st.cursor_r, st.cursor_c, st.grid
  layout()
  return true
end

function init()
  seed_rng()
  diff = "medium"
  if not try_restore() then
    restart("medium")
  else
    frame_changed = true
  end
end

local function act_cell(r, c)
  if over then return end
  cursor_r, cursor_c = r, c
  if mode == "flag" then
    if Game.toggle_flag(grid, r, c) then
      persist()
      frame_changed = true
    else
      frame_changed = false
    end
    return
  end
  if first then
    Game.place_mines(grid, rows, cols, mines, r, c, rng_int)
    first = false
  end
  local res = Game.reveal(grid, rows, cols, r, c)
  if res == "noop" then
    frame_changed = false
    return
  end
  if res == "dead" then
    over = true
    won = false
  elseif Game.is_win(grid, rows, cols, mines) then
    over = true
    won = true
  end
  persist()
  frame_changed = true
end

local function punch_white(x, y, w, h)
  if w <= 0 or h <= 0 then return end
  if type(gui.fillRectDither) == "function" then
    gui.fillRectDither(x, y, w, h, "white")
  end
end

-- Unrevealed: solid black with a white pixel every 4px (screen-aligned).
local function fill_unrevealed(x, y, w, h)
  gui.fillRect(x, y, w, h)
  local x1, y1 = x + w - 1, y + h - 1
  local yy = y + ((4 - (y % 4)) % 4)
  while yy <= y1 do
    local xx = x + ((4 - (x % 4)) % 4)
    while xx <= x1 do
      punch_white(xx, yy, 1, 1)
      xx = xx + 4
    end
    yy = yy + 4
  end
end

local function cell_font(cw, ch)
  local lh10 = gui.lineHeight(10)
  local lh12 = gui.lineHeight(12)
  if lh12 + 2 <= ch and gui.textWidth(12, "8") + 2 <= cw then
    return 12
  end
  if lh10 + 2 <= ch then
    return 10
  end
  return 10
end

local function draw_in_cell(font, x, y, cw, ch, lab)
  local tw = gui.textWidth(font, lab)
  local th = gui.lineHeight(font)
  local tx = x + math.floor((cw - tw) / 2)
  local ty = y + math.floor((ch - th) / 2)
  if tx < x + 1 then tx = x + 1 end
  if ty < y + 1 then ty = y + 1 end
  if tx + tw > x + cw - 1 then tx = x + cw - 1 - tw end
  if ty + th > y + ch - 1 then ty = y + ch - 1 - th end
  gui.drawText(font, tx, ty, lab)
end

local function draw_btn(b, label, on)
  gui.drawRect(b.x, b.y, b.w, b.h)
  if on then
    gui.drawRect(b.x + 2, b.y + 2, b.w - 4, b.h - 4)
  end
  local tw = gui.textWidth(12, label)
  local x = b.x + math.floor((b.w - tw) / 2)
  local y = b.y + math.floor((b.h - gui.lineHeight(12)) / 2)
  gui.drawText(12, x, y, label)
end

local function hit(b, x, y)
  return x >= b.x and x < b.x + b.w and y >= b.y and y < b.y + b.h
end

function draw()
  gui.clear()
  local left = mines - Game.flags_used(grid, rows, cols)
  local st = left .. " mines  " .. mode
  if over then
    st = (won and "WIN  " or "DEAD  ") .. st
  end
  gui.drawText(16, 8, 8, st)
  gui.drawRect(GRID_X - 1, GRID_Y - 1, GRID_W + 2, GRID_H + 2)
  local font = cell_font(CELL_W, CELL_H)
  for r = 1, rows do
    for c = 1, cols do
      local cell = grid[r][c]
      local x = GRID_X + (c - 1) * CELL_W
      local y = GRID_Y + (r - 1) * CELL_H
      gui.drawRect(x, y, CELL_W - 1, CELL_H - 1)
      if cell.shown then
        if cell.mine then
          gui.fillRect(x + 2, y + 2, CELL_W - 5, CELL_H - 5)
          draw_in_cell(font, x, y, CELL_W - 1, CELL_H - 1, "*")
        elseif cell.n > 0 then
          draw_in_cell(font, x, y, CELL_W - 1, CELL_H - 1, tostring(cell.n))
        end
      else
        fill_unrevealed(x + 1, y + 1, CELL_W - 3, CELL_H - 3)
        if cell.flag then
          local tw = gui.textWidth(font, "F")
          local th = gui.lineHeight(font)
          punch_white(x + math.floor((CELL_W - tw) / 2) - 1,
                      y + math.floor((CELL_H - th) / 2) - 1,
                      tw + 2, th + 2)
          draw_in_cell(font, x, y, CELL_W - 1, CELL_H - 1, "F")
        end
      end
      if cursor_r == r and cursor_c == c then
        if cell.shown then
          gui.drawRect(x + 3, y + 3, CELL_W - 7, CELL_H - 7)
        else
          punch_white(x + 3, y + 3, CELL_W - 7, 1)
          punch_white(x + 3, y + CELL_H - 8, CELL_W - 7, 1)
          punch_white(x + 3, y + 3, 1, CELL_H - 7)
          punch_white(x + CELL_W - 8, y + 3, 1, CELL_H - 7)
        end
      end
    end
  end
  draw_btn(BTN.mode, mode, true)
  draw_btn(BTN.easy, "10", diff == "easy")
  draw_btn(BTN.med, "40", diff == "medium")
  draw_btn(BTN.hard, "99", diff == "hard")
  draw_btn(BTN.new, "new", false)
  -- Tick honors this; leaving it true re-flashes the whole e-ink every 2s.
  frame_changed = false
end

local function move_cursor(dr, dc)
  local nr = cursor_r + dr
  local nc = cursor_c + dc
  if nr < 1 then nr = 1 end
  if nr > rows then nr = rows end
  if nc < 1 then nc = 1 end
  if nc > cols then nc = cols end
  if nr ~= cursor_r or nc ~= cursor_c then
    cursor_r, cursor_c = nr, nc
    frame_changed = true
  else
    frame_changed = false
  end
end

function onKey(key)
  if key == "confirm" then
    if over then
      restart(diff)
      return
    end
    act_cell(cursor_r, cursor_c)
    return
  end
  if key == "up" then move_cursor(-1, 0)
  elseif key == "down" then move_cursor(1, 0)
  elseif key == "left" then move_cursor(0, -1)
  elseif key == "right" then move_cursor(0, 1)
  end
end

function onTouch(x, y, phase)
  -- Host still displayBuffer on every Touch; ignore press/up so one tap
  -- does not dig/flag three times (and flash three full frames).
  if phase and phase ~= "tap" then
    return
  end
  if hit(BTN.new, x, y) then
    restart(diff)
    return
  end
  if hit(BTN.easy, x, y) then
    restart("easy")
    return
  end
  if hit(BTN.med, x, y) then
    restart("medium")
    return
  end
  if hit(BTN.hard, x, y) then
    restart("hard")
    return
  end
  if hit(BTN.mode, x, y) then
    mode = (mode == "dig") and "flag" or "dig"
    persist()
    frame_changed = true
    return
  end
  if x >= GRID_X and x < GRID_X + GRID_W and y >= GRID_Y and y < GRID_Y + GRID_H then
    local c = math.floor((x - GRID_X) / CELL_W) + 1
    local r = math.floor((y - GRID_Y) / CELL_H) + 1
    if Game.in_bounds(rows, cols, r, c) then
      if over then
        restart(diff)
        return
      end
      act_cell(r, c)
    end
  end
end
