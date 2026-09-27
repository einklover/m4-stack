-- com.m4.connect — 连连看 on 480x800 e-ink. Static frames only.

sys.load("game.lua")

dirty = false
frame_changed = true

local W, H = 480, 800
local TITLE_H = 36
local BTN_H = 46

local LABELS = {
  "O", "S", "T", "P", "X", "H",
  "D", "U", "Z", "L", "N", "A",
}

local function layout()
  local by = H - BTN_H * 2 - 4
  CELL = math.max(12, math.min(
    math.floor((W - 16) / st.cols),
    math.floor((by - TITLE_H - 8) / st.rows)))
  GRID_W, GRID_H = st.cols * CELL, st.rows * CELL
  GRID_X = math.floor((W - GRID_W) / 2)
  GRID_Y = TITLE_H + math.floor((by - TITLE_H - GRID_H) / 2)
  local bw = math.floor((W - 16 - 12) / 4)
  local function btn(col, row)
    return {
      x = 8 + col * (bw + 4),
      y = by + row * (BTN_H + 4),
      w = bw,
      h = BTN_H,
    }
  end
  BTN = {
    easy = btn(0, 0),
    med = btn(1, 0),
    hint = btn(2, 0),
    undo = btn(3, 0),
    mix = btn(0, 1),
    new = btn(1, 1),
    help = btn(2, 1),
    exit = btn(3, 1),
  }
end

local function persist()
  if type(fs) ~= "table" or type(fs.writeFile) ~= "function" then return end
  pcall(function() fs.writeFile("state.csv", Game.serialize(st)) end)
end

function restart(level, seed)
  st = Game.new_game(level or (st and st.level) or "easy", seed or os_seed())
  help_on = false
  last_path = nil
  layout()
  persist()
  frame_changed = true
end

function os_seed()
  if type(sys) == "table" and type(sys.millis) == "function" then
    local ok, n = pcall(sys.millis)
    if ok and type(n) == "number" then return math.floor(n) % 100000 + 1 end
  end
  return 1
end

local function try_restore()
  if type(fs) ~= "table" or type(fs.readFile) ~= "function" then return false end
  local ok, data = pcall(function() return fs.readFile("state.csv") end)
  if not ok or type(data) ~= "string" or data == "" then return false end
  local loaded = Game.deserialize(data)
  if not loaded then return false end
  st = loaded
  return true
end

function init()
  help_on = false
  last_path = nil
  if not try_restore() then
    st = Game.new_game("easy", 1)
  end
  layout()
  frame_changed = true
end

local function hit(b, x, y)
  return b and x >= b.x and x < b.x + b.w and y >= b.y and y < b.y + b.h
end

local function draw_glyph(kind, x, y, s)
  local m = 6
  local x0, y0 = x + m, y + m
  local x1, y1 = x + s - m, y + s - m
  local cx = x + math.floor(s / 2)
  local cy = y + math.floor(s / 2)
  if kind == 1 then
    gui.drawRect(x0 + 2, y0 + 2, (x1 - x0) - 4, (y1 - y0) - 4)
  elseif kind == 2 then
    gui.drawRect(x0, y0, x1 - x0, y1 - y0)
    gui.drawRect(x0 + 4, y0 + 4, x1 - x0 - 8, y1 - y0 - 8)
  elseif kind == 3 then
    gui.drawLine(cx, y0, x1, y1)
    gui.drawLine(x1, y1, x0, y1)
    gui.drawLine(x0, y1, cx, y0)
  elseif kind == 4 then
    gui.drawLine(cx, y0, cx, y1)
    gui.drawLine(x0, cy, x1, cy)
  elseif kind == 5 then
    gui.drawLine(x0, y0, x1, y1)
    gui.drawLine(x0, y1, x1, y0)
  elseif kind == 6 then
    gui.drawLine(x0, y0 + 4, x1, y0 + 4)
    gui.drawLine(x0, cy, x1, cy)
    gui.drawLine(x0, y1 - 4, x1, y1 - 4)
  elseif kind == 7 then
    gui.drawLine(cx, y0, x1, cy)
    gui.drawLine(x1, cy, cx, y1)
    gui.drawLine(cx, y1, x0, cy)
    gui.drawLine(x0, cy, cx, y0)
  elseif kind == 8 then
    gui.drawLine(x0, y0, x0, y1)
    gui.drawLine(x0, y1, x1, y1)
    gui.drawLine(x1, y1, x1, y0)
  elseif kind == 9 then
    gui.drawLine(x0, y0, x1, y0)
    gui.drawLine(x1, y0, x0, y1)
    gui.drawLine(x0, y1, x1, y1)
  elseif kind == 10 then
    gui.drawLine(x0, y0, x0, y1)
    gui.drawLine(x0, y1, x1, y1)
  elseif kind == 11 then
    gui.drawLine(x0 + 6, y0, x0 + 6, y1)
    gui.drawLine(x1 - 6, y0, x1 - 6, y1)
    gui.drawLine(x0, y0 + 6, x1, y0 + 6)
    gui.drawLine(x0, y1 - 6, x1, y1 - 6)
  else
    gui.drawLine(cx, y0, cx, y1)
    gui.drawLine(x0, cy, x1, cy)
    gui.drawLine(x0, y0, x1, y1)
    gui.drawLine(x0, y1, x1, y0)
  end
  local lab = LABELS[kind] or "?"
  local font = s >= 40 and 12 or 10
  local tw = gui.textWidth(font, lab)
  gui.drawText(font, x + s - tw - 3, y + 2, lab)
end

local function draw_btn(b, text)
  gui.drawRect(b.x, b.y, b.w, b.h)
  local font = 12
  local tw = gui.textWidth(font, text)
  local th = gui.lineHeight(font)
  gui.drawText(font, b.x + math.floor((b.w - tw) / 2), b.y + math.floor((b.h - th) / 2), text)
end

function draw()
  gui.clear()
  local left = Game.remaining(st.board) / 2
  local title = (st.won and "WIN  " or "") .. "连连看  " .. st.level .. "  " .. tostring(left)
  gui.drawText(16, 12, 8, title)
  gui.drawRect(GRID_X - 2, GRID_Y - 2, GRID_W + 4, GRID_H + 4)
  for r = 1, st.rows do
    for c = 1, st.cols do
      local x = GRID_X + (c - 1) * CELL
      local y = GRID_Y + (r - 1) * CELL
      local v = st.board[r][c]
      if v ~= 0 then
        gui.drawRect(x + 1, y + 1, CELL - 2, CELL - 2)
        draw_glyph(v, x, y, CELL)
      end
      if st.cursor[1] == r and st.cursor[2] == c then
        gui.drawRect(x + 3, y + 3, CELL - 6, CELL - 6)
      end
      if st.sel and st.sel[1] == r and st.sel[2] == c then
        gui.fillRect(x + 2, y + 2, CELL - 4, 3)
      end
    end
  end
  if last_path and #last_path >= 2 then
    for i = 2, #last_path do
      local a, b = last_path[i - 1], last_path[i]
      local function center(p)
        local rr, cc = p[1], p[2]
        if rr < 1 then rr = 1 end
        if cc < 1 then cc = 1 end
        if rr > st.rows then rr = st.rows end
        if cc > st.cols then cc = st.cols end
        local ox, oy = 0, 0
        if p[1] < 1 then oy = -CELL end
        if p[1] > st.rows then oy = CELL end
        if p[2] < 1 then ox = -CELL end
        if p[2] > st.cols then ox = CELL end
        return GRID_X + (cc - 1) * CELL + math.floor(CELL / 2) + ox,
          GRID_Y + (rr - 1) * CELL + math.floor(CELL / 2) + oy
      end
      local x1, y1 = center(a)
      local x2, y2 = center(b)
      gui.drawLine(x1, y1, x2, y2)
    end
  end
  draw_btn(BTN.easy, "easy")
  draw_btn(BTN.med, "med")
  draw_btn(BTN.hint, "hint")
  draw_btn(BTN.undo, "undo")
  draw_btn(BTN.mix, "mix")
  draw_btn(BTN.new, "new")
  draw_btn(BTN.help, "help")
  draw_btn(BTN.exit, "exit")
  if help_on then
    gui.fillRectDither(24, 80, W - 48, 420, "white")
    gui.drawRect(24, 80, W - 48, 420)
    local lines = {
      "连连看  Shisen-Sho",
      "Same mark, path bends <= 2",
      "Path may use the outside border",
      "Pad: move   confirm: pick",
      "Tap a tile to pick / match",
      "hint undo mix new help exit",
      "Hardware back also leaves",
      "Tap help again to close",
    }
    for i = 1, #lines do
      gui.drawText(12, 40, 100 + (i - 1) * 36, lines[i])
    end
  end
  frame_changed = false
end

local function move_cursor(dr, dc)
  local r = st.cursor[1] + dr
  local c = st.cursor[2] + dc
  if r < 1 then r = st.rows end
  if r > st.rows then r = 1 end
  if c < 1 then c = st.cols end
  if c > st.cols then c = 1 end
  st.cursor = { r, c }
  frame_changed = true
end

function onKey(key)
  if help_on then
    help_on = false
    frame_changed = true
    return
  end
  if st.won and key == "confirm" then
    restart(st.level, os_seed())
    return
  end
  if key == "left" then move_cursor(0, -1)
  elseif key == "right" then move_cursor(0, 1)
  elseif key == "up" then move_cursor(-1, 0)
  elseif key == "down" then move_cursor(1, 0)
  elseif key == "confirm" then
    local how, path = Game.pick(st, st.cursor[1], st.cursor[2])
    if how == "match" or how == "win" then last_path = path else last_path = nil end
    persist()
    frame_changed = true
  else
    frame_changed = false
  end
end

function onTouch(x, y, phase)
  if phase and phase ~= "tap" then
    frame_changed = false
    return
  end
  if help_on then
    help_on = false
    frame_changed = true
    return
  end
  if hit(BTN.help, x, y) then
    help_on = true
    frame_changed = true
    return
  end
  if hit(BTN.exit, x, y) then
    frame_changed = true
    if type(sys) == "table" and type(sys.exit) == "function" then sys.exit() end
    return
  end
  if hit(BTN.easy, x, y) then restart("easy", os_seed()) return end
  if hit(BTN.med, x, y) then restart("medium", os_seed()) return end
  if hit(BTN.new, x, y) then restart(st.level, os_seed()) return end
  if hit(BTN.undo, x, y) then
    if Game.undo(st) then last_path = nil persist() frame_changed = true
    else frame_changed = false end
    return
  end
  if hit(BTN.hint, x, y) then
    local r1, c1, r2, c2 = Game.use_hint(st)
    if r1 then
      last_path = Game.route(st.rows, st.cols, st.board, r1, c1, r2, c2)
      persist()
      frame_changed = true
    else
      frame_changed = false
    end
    return
  end
  if hit(BTN.mix, x, y) then
    if Game.shuffle(st, os_seed()) then
      last_path = nil
      persist()
      frame_changed = true
    else
      frame_changed = false
    end
    return
  end
  if x < GRID_X or y < GRID_Y or x >= GRID_X + GRID_W or y >= GRID_Y + GRID_H then
    frame_changed = false
    return
  end
  local c = math.floor((x - GRID_X) / CELL) + 1
  local r = math.floor((y - GRID_Y) / CELL) + 1
  local how, path = Game.pick(st, r, c)
  if how == "match" or how == "win" then last_path = path else last_path = nil end
  persist()
  frame_changed = how ~= "outside"
end
