-- com.m4.sliding — 8/15/24 puzzle on 480x800 e-ink.

sys.load("game.lua")

dirty = false
frame_changed = true

local W, H = 480, 800
local TITLE_H = 36
local BTN_H = 48

local function layout()
  local by = H - BTN_H * 2
  CELL = math.max(12, math.min(
    math.floor((W - 16) / n),
    math.floor((by - TITLE_H - 8) / n)))
  GRID_W, GRID_H = n * CELL, n * CELL
  GRID_X = math.floor((W - GRID_W) / 2)
  GRID_Y = TITLE_H + math.floor((by - TITLE_H - GRID_H) / 2)
  local gap = 6
  local bw = math.floor((W - 16 - gap * 3) / 4)
  local function row(y, names)
    local t = {}
    for i = 1, 4 do
      t[names[i]] = { x = 8 + (i - 1) * (bw + gap), y = y, w = bw, h = BTN_H - 4 }
    end
    return t
  end
  BTN = row(by, { "s3", "s4", "s5", "pic" })
  local lower = row(by + BTN_H, { "same", "new", "undo", "help" })
  for k, v in pairs(lower) do BTN[k] = v end
end

local function snapshot()
  return {
    n = n, picture = picture, seed = seed, steps = steps,
    moves = moves, won = won, cr = cr, cc = cc,
    board = board, undo = undo,
  }
end

local function persist()
  if type(fs) ~= "table" or type(fs.writeFile) ~= "function" then return end
  pcall(function()
    fs.writeFile("state.csv", Game.serialize(snapshot()))
  end)
end

local function apply_state(st)
  n, picture, seed, steps = st.n, st.picture, st.seed, st.steps
  moves, won, cr, cc = st.moves, st.won, st.cr, st.cc
  board, undo = st.board, st.undo
end

function restart_same()
  local st = Game.fresh(n, seed, picture)
  if not st then return end
  st.steps = steps
  local board2 = Game.scramble(n, seed, steps)
  if board2 then
    st.board = board2
    st.won = Game.is_win(board2, n)
    local br, bc = Game.rc(n, Game.blank_at(board2))
    st.cr, st.cc = br, bc
  end
  apply_state(st)
  help = false
  persist()
  frame_changed = true
end

function new_puzzle()
  seed = seed + 1
  if seed > 2000000000 then seed = 1 end
  local st = Game.fresh(n, seed, picture)
  if not st then return end
  apply_state(st)
  help = false
  persist()
  frame_changed = true
end

local function set_size(next_n)
  if next_n == n and not help then
    frame_changed = false
    return
  end
  n = next_n
  local st = Game.fresh(n, seed, picture)
  apply_state(st)
  layout()
  help = false
  persist()
  frame_changed = true
end

local function toggle_picture()
  picture = not picture
  help = false
  persist()
  frame_changed = true
end

local function try_restore()
  if type(fs) ~= "table" or type(fs.readFile) ~= "function" then return false end
  local ok, data = pcall(function() return fs.readFile("state.csv") end)
  if not ok or type(data) ~= "string" or data == "" then return false end
  local st = Game.deserialize(data)
  if not st then return false end
  apply_state(st)
  return true
end

function init()
  n = 4
  help = false
  layout()
  if not try_restore() then
    seed = 1
    picture = false
    local st = Game.fresh(4, 1, false)
    apply_state(st)
    persist()
  end
  layout()
  frame_changed = true
end

local function hit(b, x, y)
  return b and x >= b.x and x < b.x + b.w and y >= b.y and y < b.y + b.h
end

local function draw_btn(b, label, on)
  gui.drawRect(b.x, b.y, b.w, b.h)
  if on then gui.drawRect(b.x + 3, b.y + 3, b.w - 6, b.h - 6) end
  local font = 12
  local tw = gui.textWidth(font, label)
  local th = gui.lineHeight(font)
  gui.drawText(font, b.x + math.floor((b.w - tw) / 2), b.y + math.floor((b.h - th) / 2), label)
end

local function draw_number(x, y, value)
  local font = CELL >= 72 and 16 or 12
  local label = tostring(value)
  local tw = gui.textWidth(font, label)
  local th = gui.lineHeight(font)
  gui.drawText(font, x + math.floor((CELL - tw) / 2), y + math.floor((CELL - th) / 2), label)
end

local function draw_fragment(x, y, value)
  local res = 10
  local step = math.floor(CELL / res)
  if step < 1 then step = 1 end
  local pad = math.floor((CELL - step * res) / 2)
  for v = 0, res - 1 do
    local run = nil
    for u = 0, res do
      local ink = u < res and Game.fragment_ink(n, value, u, v, res)
      if ink then
        if not run then run = u end
      elseif run then
        gui.fillRect(x + pad + run * step, y + pad + v * step, (u - run) * step, math.max(1, step - 1))
        run = nil
      end
    end
  end
end

function draw()
  gui.clear()
  if help then
    gui.drawText(16, 16, 12, "滑动拼图")
    local lines = {
      "方向键移动光标，确认滑动邻接块",
      "点邻接空格的块即可滑动",
      "重开：同一随机种子",
      "新局：下一局  撤销：退一步",
      "3x3 是 8 谜，4x4 是 15 谜，5x5 是 24 谜",
      "图案是几何菱形碎片，无照片",
      "返回键退出到主屏",
    }
    for i = 1, #lines do
      gui.drawText(12, 16, 56 + (i - 1) * 36, lines[i])
    end
    draw_btn(BTN.help, "关闭", true)
    frame_changed = false
    return
  end

  local kind = n == 3 and "8" or (n == 4 and "15" or "24")
  local title = (won and "WIN  " or "") .. "滑动拼图 " .. kind .. "  " .. tostring(moves)
  gui.drawText(16, 12, 8, title)
  gui.drawRect(GRID_X - 2, GRID_Y - 2, GRID_W + 4, GRID_H + 4)

  for r = 1, n do
    for c = 1, n do
      local x = GRID_X + (c - 1) * CELL
      local y = GRID_Y + (r - 1) * CELL
      local value = board[(r - 1) * n + c]
      gui.drawRect(x, y, CELL, CELL)
      if value ~= 0 then
        if picture then draw_fragment(x, y, value) else draw_number(x, y, value) end
      end
      if r == cr and c == cc then
        gui.drawRect(x + 4, y + 4, CELL - 8, CELL - 8)
      end
    end
  end

  draw_btn(BTN.s3, "3x3", n == 3)
  draw_btn(BTN.s4, "4x4", n == 4)
  draw_btn(BTN.s5, "5x5", n == 5)
  draw_btn(BTN.pic, picture and "数字" or "图案", false)
  draw_btn(BTN.same, "重开", false)
  draw_btn(BTN.new, "新局", false)
  draw_btn(BTN.undo, "撤销", false)
  draw_btn(BTN.help, "说明", false)
  frame_changed = false
end

local function move_cursor(dr, dc)
  local nr = cr + dr
  local nc = cc + dc
  if nr < 1 or nc < 1 or nr > n or nc > n then
    frame_changed = false
    return
  end
  cr, cc = nr, nc
  persist()
  frame_changed = true
end

local function live()
  return {
    n = n, board = board, undo = undo,
    moves = moves, won = won, cr = cr, cc = cc,
  }
end

local function commit_play(st, ok)
  if not ok then
    frame_changed = false
    return
  end
  moves, won = st.moves, st.won
  persist()
  frame_changed = true
end

function onKey(key)
  if help then
    help = false
    frame_changed = true
    return
  end
  if key == "confirm" then
    if won then restart_same() return end
    local st = live()
    commit_play(st, Game.play_cursor(st))
    return
  end
  if key == "left" then move_cursor(0, -1)
  elseif key == "right" then move_cursor(0, 1)
  elseif key == "up" then move_cursor(-1, 0)
  elseif key == "down" then move_cursor(1, 0)
  else
    frame_changed = false
  end
end

function onTouch(x, y, phase)
  if phase and phase ~= "tap" then return end
  if help then
    help = false
    frame_changed = true
    return
  end
  if hit(BTN.s3, x, y) then set_size(3) return end
  if hit(BTN.s4, x, y) then set_size(4) return end
  if hit(BTN.s5, x, y) then set_size(5) return end
  if hit(BTN.pic, x, y) then toggle_picture() return end
  if hit(BTN.same, x, y) then restart_same() return end
  if hit(BTN.new, x, y) then new_puzzle() return end
  if hit(BTN.undo, x, y) then
    local st = live()
    commit_play(st, Game.undo(st))
    return
  end
  if hit(BTN.help, x, y) then
    help = true
    frame_changed = true
    return
  end
  if x < GRID_X or y < GRID_Y or x >= GRID_X + GRID_W or y >= GRID_Y + GRID_H then
    frame_changed = false
    return
  end
  local c = math.floor((x - GRID_X) / CELL) + 1
  local r = math.floor((y - GRID_Y) / CELL) + 1
  cr, cc = r, c
  if won then
    persist()
    frame_changed = true
    return
  end
  local index = Game.idx(n, r, c)
  local blank = Game.blank_at(board)
  if Game.adjacent(n, index, blank) then
    local st = live()
    commit_play(st, Game.play_index(st, index))
  else
    persist()
    frame_changed = true
  end
end
