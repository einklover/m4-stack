-- com.m4.huarong — 华容道 横刀立马 on 480x800 e-ink.

sys.load("game.lua")

dirty = false
frame_changed = true

local W, H = 480, 800
local TITLE_H = 36
local BTN_H = 52

local function layout()
  local by = H - BTN_H
  CELL = math.max(12, math.min(
    math.floor((W - 16) / Game.COLS),
    math.floor((by - TITLE_H - 12) / Game.ROWS)))
  GRID_W, GRID_H = Game.COLS * CELL, Game.ROWS * CELL
  GRID_X = math.floor((W - GRID_W) / 2)
  GRID_Y = TITLE_H + math.floor((by - 8 - TITLE_H - GRID_H) / 2)
  BTN = {
    new = { x = 8, y = by, w = W - 16, h = BTN_H },
  }
end

local function persist()
  if type(fs) ~= "table" or type(fs.writeFile) ~= "function" then return end
  pcall(function()
    fs.writeFile("state.csv", Game.serialize({
      sel = sel, moves = moves, won = won, pieces = pieces,
    }))
  end)
end

function restart()
  pieces = Game.classic()
  sel = 1
  moves = 0
  won = false
  persist()
  frame_changed = true
end

local function try_restore()
  if type(fs) ~= "table" or type(fs.readFile) ~= "function" then return false end
  local ok, data = pcall(function() return fs.readFile("state.csv") end)
  if not ok or type(data) ~= "string" or data == "" then return false end
  local st = Game.deserialize(data)
  if not st then return false end
  sel, moves, won, pieces = st.sel, st.moves, st.won, st.pieces
  if not Game.find(pieces, sel) then sel = 1 end
  return true
end

function init()
  layout()
  if not try_restore() then
    restart()
  else
    frame_changed = true
  end
end

local function try_slide(dr, dc)
  if won then
    restart()
    return
  end
  if Game.move(pieces, sel, dr, dc) then
    moves = moves + 1
    if Game.is_win(pieces) then won = true end
    persist()
    frame_changed = true
  else
    frame_changed = false
  end
end

local function cycle_sel(dir)
  local ids = {}
  for i = 1, #pieces do ids[i] = pieces[i].id end
  table.sort(ids)
  local idx = 1
  for i = 1, #ids do
    if ids[i] == sel then idx = i break end
  end
  idx = idx + dir
  if idx < 1 then idx = #ids end
  if idx > #ids then idx = 1 end
  sel = ids[idx]
  persist()
  frame_changed = true
end

local function hit(b, x, y)
  return x >= b.x and x < b.x + b.w and y >= b.y and y < b.y + b.h
end

function draw()
  gui.clear()
  local title = (won and "WIN  " or "") .. "华容道  " .. tostring(moves)
  gui.drawText(16, 8, 8, title)
  gui.drawRect(GRID_X - 2, GRID_Y - 2, GRID_W + 4, GRID_H + 4)
  -- bottom exit gap marker
  local ex = GRID_X + CELL
  gui.fillRect(ex, GRID_Y + GRID_H, CELL * 2, 4)

  for _, p in ipairs(pieces) do
    local x = GRID_X + (p.c - 1) * CELL
    local y = GRID_Y + (p.r - 1) * CELL
    local w, h = p.w * CELL - 3, p.h * CELL - 3
    gui.drawRect(x, y, w, h)
    if p.id == 1 then
      gui.drawRect(x + 2, y + 2, w - 4, h - 4)
    end
    if p.id == sel then
      gui.drawRect(x + 5, y + 5, w - 10, h - 10)
    end
    local font = (p.w >= 2 or p.h >= 2) and 16 or 12
    local tw = gui.textWidth(font, p.name)
    local th = gui.lineHeight(font)
    gui.drawText(font, x + math.floor((w - tw) / 2), y + math.floor((h - th) / 2), p.name)
  end

  gui.drawRect(BTN.new.x, BTN.new.y, BTN.new.w, BTN.new.h)
  local lab = "new  横刀立马"
  local tw = gui.textWidth(12, lab)
  gui.drawText(12, BTN.new.x + math.floor((BTN.new.w - tw) / 2),
    BTN.new.y + math.floor((BTN.new.h - gui.lineHeight(12)) / 2), lab)
  frame_changed = false
end

function onKey(key)
  if key == "confirm" then
    if won then restart() return end
    cycle_sel(1)
    return
  end
  if key == "left" then try_slide(0, -1)
  elseif key == "right" then try_slide(0, 1)
  elseif key == "up" then try_slide(-1, 0)
  elseif key == "down" then try_slide(1, 0)
  end
end

function onTouch(x, y, phase)
  if phase and phase ~= "tap" then return end
  if hit(BTN.new, x, y) then restart() return end
  if x < GRID_X or y < GRID_Y or x >= GRID_X + GRID_W or y >= GRID_Y + GRID_H then
    return
  end
  local c = math.floor((x - GRID_X) / CELL) + 1
  local r = math.floor((y - GRID_Y) / CELL) + 1
  local p = Game.at(pieces, r, c)
  if p then
    if p.id == sel then
      frame_changed = false
      return
    end
    sel = p.id
    persist()
    frame_changed = true
    return
  end
  -- empty cell: slide selected one step toward it if adjacent
  local cur = Game.find(pieces, sel)
  if not cur then
    frame_changed = false
    return
  end
  local dr, dc = 0, 0
  if r < cur.r then dr = -1
  elseif r >= cur.r + cur.h then dr = 1
  elseif c < cur.c then dc = -1
  elseif c >= cur.c + cur.w then dc = 1
  end
  if dr ~= 0 or dc ~= 0 then
    try_slide(dr, dc)
  else
    frame_changed = false
  end
end
