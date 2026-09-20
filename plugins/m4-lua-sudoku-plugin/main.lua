-- com.m4.sudoku — ReKindle sudoku on 480x800 e-ink.

sys.load("game.lua")

dirty = false
frame_changed = true

local W, H = 480, 800
local TITLE_H = 36
local BTN_H = 48

local function rng_int(max)
  if max < 1 then return 1 end
  return math.random(1, max)
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
  CELL = math.max(12, math.floor((W - 16) / 9))
  GRID = CELL * 9
  GRID_X = math.floor((W - GRID) / 2)
  GRID_Y = TITLE_H
  local by = H - BTN_H
  local pad_top = GRID_Y + GRID + 4
  local pad_h = by - 4 - pad_top
  local pw = math.floor((W - 16) / 3)
  local ph = math.floor(pad_h / 3)
  PAD = {}
  for n = 1, 9 do
    local rr = math.floor((n - 1) / 3)
    local cc = (n - 1) % 3
    PAD[n] = {
      x = 8 + cc * pw,
      y = pad_top + rr * ph,
      w = pw - 2,
      h = ph - 2,
    }
  end
  local bw = math.floor((W - 16) / 7)
  BTN = {
    check = { x = 8, y = by, w = bw, h = BTN_H },
    solve = { x = 8 + bw, y = by, w = bw, h = BTN_H },
    x     = { x = 8 + bw * 2, y = by, w = bw, h = BTN_H },
    easy  = { x = 8 + bw * 3, y = by, w = bw, h = BTN_H },
    med   = { x = 8 + bw * 4, y = by, w = bw, h = BTN_H },
    hard  = { x = 8 + bw * 5, y = by, w = bw, h = BTN_H },
    new   = { x = 8 + bw * 6, y = by, w = bw, h = BTN_H },
  }
end

local function persist()
  if type(fs) ~= "table" or type(fs.writeFile) ~= "function" then return end
  pcall(function()
    fs.writeFile("state.csv", Game.serialize({
      diff = diff, sel = sel, msg = msg, won = won,
      sol = sol, cur = cur, fixed = fixed,
    }))
  end)
end

function restart(new_diff)
  if new_diff then diff = new_diff end
  local spec = Game.DIFF[diff] or Game.DIFF.easy
  sol, cur, fixed = Game.new_puzzle(spec.remove, rng_int)
  sel = Game.next_free(fixed, 81)
  won = false
  msg = diff
  err_at = {}
  persist()
  frame_changed = true
end

local function try_restore()
  if type(fs) ~= "table" or type(fs.readFile) ~= "function" then return false end
  local ok, data = pcall(function() return fs.readFile("state.csv") end)
  if not ok or type(data) ~= "string" or data == "" then return false end
  local st = Game.deserialize(data)
  if not st or not Game.DIFF[st.diff] then return false end
  diff, sel, msg, won = st.diff, st.sel, st.msg, st.won
  sol, cur, fixed = st.sol, st.cur, st.fixed
  err_at = {}
  return true
end

function init()
  seed_rng()
  layout()
  diff = "easy"
  if not try_restore() then
    restart("easy")
  else
    frame_changed = true
  end
end

local function set_num(n)
  if won or not sel or fixed[sel] then
    frame_changed = false
    return
  end
  cur[sel] = n
  err_at[sel] = nil
  if Game.is_win(cur, sol) then
    won = true
    msg = "WIN"
  else
    msg = diff
  end
  persist()
  frame_changed = true
end

local function do_check()
  err_at = {}
  local errs = Game.errors(cur, sol, fixed)
  for _, i in ipairs(errs) do err_at[i] = true end
  if #errs > 0 then
    msg = "errors"
  elseif Game.has_empty(cur) then
    msg = "ok so far"
  else
    msg = "perfect"
    won = Game.is_win(cur, sol)
  end
  persist()
  frame_changed = true
end

local function do_solve()
  if won or not sel then
    frame_changed = false
    return
  end
  if fixed[sel] then
    sel = Game.next_free(fixed, sel)
    frame_changed = true
    persist()
    return
  end
  cur[sel] = sol[sel]
  err_at[sel] = nil
  if Game.is_win(cur, sol) then
    won = true
    msg = "WIN"
  else
    sel = Game.next_free(fixed, sel)
  end
  persist()
  frame_changed = true
end

local function hit(b, x, y)
  return x >= b.x and x < b.x + b.w and y >= b.y and y < b.y + b.h
end

local function draw_btn(b, label, on, font)
  font = font or 12
  gui.drawRect(b.x, b.y, b.w, b.h)
  if on then
    gui.drawRect(b.x + 2, b.y + 2, b.w - 4, b.h - 4)
  end
  local tw = gui.textWidth(font, label)
  local x = b.x + math.floor((b.w - tw) / 2)
  local y = b.y + math.floor((b.h - gui.lineHeight(font)) / 2)
  gui.drawText(font, x, y, label)
end

function draw()
  gui.clear()
  local title = won and "WIN  sudoku" or ("sudoku  " .. tostring(msg or diff))
  gui.drawText(16, 8, 8, title)
  gui.drawRect(GRID_X - 1, GRID_Y - 1, GRID + 2, GRID + 2)
  for k = 0, 9 do
    local thick = (k % 3 == 0)
    local x = GRID_X + k * CELL
    local y = GRID_Y + k * CELL
    if thick then
      gui.fillRect(x, GRID_Y, 2, GRID)
      gui.fillRect(GRID_X, y, GRID, 2)
    else
      gui.drawLine(x, GRID_Y, x, GRID_Y + GRID)
      gui.drawLine(GRID_X, y, GRID_X + GRID, y)
    end
  end
  for r = 1, 9 do
    for c = 1, 9 do
      local i = Game.idx(r, c)
      local x = GRID_X + (c - 1) * CELL
      local y = GRID_Y + (r - 1) * CELL
      if i == sel then
        gui.drawRect(x + 3, y + 3, CELL - 6, CELL - 6)
      end
      local v = cur[i]
      if v ~= 0 then
        local lab = tostring(v)
        local font = 16
        local tw = gui.textWidth(font, lab)
        local th = gui.lineHeight(font)
        gui.drawText(font, x + math.floor((CELL - tw) / 2), y + math.floor((CELL - th) / 2), lab)
        if err_at[i] then
          gui.drawRect(x + 6, y + 6, CELL - 12, CELL - 12)
        end
      end
    end
  end
  for n = 1, 9 do
    draw_btn(PAD[n], tostring(n), false, 16)
  end
  draw_btn(BTN.check, "chk", false)
  draw_btn(BTN.solve, "sol", false)
  draw_btn(BTN.x, "X", false)
  draw_btn(BTN.easy, "E", diff == "easy")
  draw_btn(BTN.med, "M", diff == "medium")
  draw_btn(BTN.hard, "H", diff == "hard")
  draw_btn(BTN.new, "new", false)
  frame_changed = false
end

local function move_sel(dr, dc)
  local r = math.floor((sel - 1) / 9) + 1
  local c = (sel - 1) % 9 + 1
  r = r + dr
  c = c + dc
  if r < 1 then r = 1 end
  if r > 9 then r = 9 end
  if c < 1 then c = 1 end
  if c > 9 then c = 9 end
  local nsel = Game.idx(r, c)
  if nsel ~= sel then
    sel = nsel
    frame_changed = true
  else
    frame_changed = false
  end
end

function onKey(key)
  if key == "confirm" then
    if won then
      restart(diff)
      return
    end
    if fixed[sel] then
      sel = Game.next_free(fixed, sel)
      persist()
      frame_changed = true
      return
    end
    local v = cur[sel]
    if v >= 9 then
      set_num(0)
    else
      set_num(v + 1)
    end
    return
  end
  if key == "up" then move_sel(-1, 0)
  elseif key == "down" then move_sel(1, 0)
  elseif key == "left" then move_sel(0, -1)
  elseif key == "right" then move_sel(0, 1)
  end
end

function onTouch(x, y, phase)
  if phase and phase ~= "tap" then return end
  if hit(BTN.new, x, y) then restart(diff) return end
  if hit(BTN.easy, x, y) then restart("easy") return end
  if hit(BTN.med, x, y) then restart("medium") return end
  if hit(BTN.hard, x, y) then restart("hard") return end
  if hit(BTN.check, x, y) then do_check() return end
  if hit(BTN.solve, x, y) then do_solve() return end
  if hit(BTN.x, x, y) then set_num(0) return end
  for n = 1, 9 do
    if hit(PAD[n], x, y) then set_num(n) return end
  end
  if x >= GRID_X and x < GRID_X + GRID and y >= GRID_Y and y < GRID_Y + GRID then
    local c = math.floor((x - GRID_X) / CELL) + 1
    local r = math.floor((y - GRID_Y) / CELL) + 1
    if r >= 1 and r <= 9 and c >= 1 and c <= 9 then
      sel = Game.idx(r, c)
      persist()
      frame_changed = true
    end
  end
end
