-- com.m4.hanoi — ReKindle Tower of Hanoi on 480x800 e-ink.

sys.load("game.lua")

dirty = false
frame_changed = true

local W, H = 480, 800
local TITLE_H = 36
local BTN_H = 52

local function layout()
  local by = H - BTN_H
  PLAY_Y = TITLE_H
  PLAY_H = by - 8 - PLAY_Y
  local bw = math.floor((W - 16) / 6)
  BTN = {
    d3  = { x = 8, y = by, w = bw, h = BTN_H },
    d4  = { x = 8 + bw, y = by, w = bw, h = BTN_H },
    d5  = { x = 8 + bw * 2, y = by, w = bw, h = BTN_H },
    d6  = { x = 8 + bw * 3, y = by, w = bw, h = BTN_H },
    d7  = { x = 8 + bw * 4, y = by, w = bw, h = BTN_H },
    new = { x = 8 + bw * 5, y = by, w = bw, h = BTN_H },
  }
end

local function persist()
  if type(fs) ~= "table" or type(fs.writeFile) ~= "function" then return end
  pcall(function()
    fs.writeFile("state.csv", Game.serialize({
      n = n, moves = moves, sel = sel, cursor = cursor, won = won, stacks = stacks,
    }))
  end)
end

function restart(new_n)
  n = new_n or n or 3
  if n < 3 then n = 3 end
  if n > 7 then n = 7 end
  stacks = Game.new_stacks(n)
  moves = 0
  sel = 0
  cursor = 1
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
  n, moves, sel, cursor, won, stacks = st.n, st.moves, st.sel, st.cursor, st.won, st.stacks
  if sel < 0 then sel = 0 end
  if cursor < 1 or cursor > 3 then cursor = 1 end
  return true
end

function init()
  layout()
  if not try_restore() then
    restart(3)
  else
    frame_changed = true
  end
end

local function tap_peg(p)
  if won then
    restart(n)
    return
  end
  cursor = p
  if sel == 0 then
    if #stacks[p] > 0 then
      sel = p
      persist()
      frame_changed = true
    else
      frame_changed = false
    end
    return
  end
  if sel == p then
    sel = 0
    persist()
    frame_changed = true
    return
  end
  if Game.move(stacks, sel, p) then
    moves = moves + 1
    sel = 0
    if Game.is_win(stacks, n) then won = true end
    persist()
    frame_changed = true
  else
    sel = 0
    persist()
    frame_changed = true
  end
end

local function hit(b, x, y)
  return x >= b.x and x < b.x + b.w and y >= b.y and y < b.y + b.h
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

function draw()
  gui.clear()
  local minm = Game.min_moves(n)
  local title = (won and "WIN  " or "") .. "hanoi  " .. moves .. "/" .. minm
  gui.drawText(16, 8, 8, title)

  local zone = math.floor(W / 3)
  local peg_x = { math.floor(zone / 2), math.floor(W / 2), math.floor(W - zone / 2) }
  local base_y = PLAY_Y + PLAY_H - 16
  local peg_h = math.floor(PLAY_H * 0.55)
  local peg_top = base_y - peg_h
  gui.fillRect(16, base_y, W - 32, 8)
  for p = 1, 3 do
    gui.fillRect(peg_x[p] - 4, peg_top, 8, peg_h)
    if cursor == p then
      gui.drawRect(zone * (p - 1) + 6, PLAY_Y + 4, zone - 12, PLAY_H - 28)
    end
  end

  local max_w = zone - 24
  local min_w = 28
  local disk_h = math.max(18, math.floor((peg_h - 12) / (n + 1)))
  for p = 1, 3 do
    local stack = stacks[p]
    for i = 1, #stack do
      local size = stack[i]
      local dw
      if n == 1 then
        dw = min_w
      else
        dw = min_w + math.floor((max_w - min_w) * (size - 1) / (n - 1))
      end
      local x = peg_x[p] - math.floor(dw / 2)
      local y = base_y - i * disk_h
      local floating = (p == sel and i == #stack)
      if floating then
        y = peg_top - disk_h - 4
      end
      gui.fillRect(x, y, dw, disk_h - 3)
      if floating then
        gui.drawRect(x - 2, y - 2, dw + 4, disk_h + 1)
      end
    end
  end

  draw_btn(BTN.d3, "3", n == 3)
  draw_btn(BTN.d4, "4", n == 4)
  draw_btn(BTN.d5, "5", n == 5)
  draw_btn(BTN.d6, "6", n == 6)
  draw_btn(BTN.d7, "7", n == 7)
  draw_btn(BTN.new, "new", false)
  frame_changed = false
end

function onKey(key)
  if key == "left" then
    cursor = cursor - 1
    if cursor < 1 then cursor = 1 end
    frame_changed = true
    return
  end
  if key == "right" then
    cursor = cursor + 1
    if cursor > 3 then cursor = 3 end
    frame_changed = true
    return
  end
  if key == "confirm" then
    tap_peg(cursor)
    return
  end
  if key == "up" or key == "down" then
    frame_changed = false
  end
end

function onTouch(x, y, phase)
  if phase and phase ~= "tap" then return end
  if hit(BTN.d3, x, y) then restart(3) return end
  if hit(BTN.d4, x, y) then restart(4) return end
  if hit(BTN.d5, x, y) then restart(5) return end
  if hit(BTN.d6, x, y) then restart(6) return end
  if hit(BTN.d7, x, y) then restart(7) return end
  if hit(BTN.new, x, y) then restart(n) return end
  if y >= PLAY_Y and y < PLAY_Y + PLAY_H then
    local p = math.floor(x / math.floor(W / 3)) + 1
    if p < 1 then p = 1 end
    if p > 3 then p = 3 end
    tap_peg(p)
  end
end
