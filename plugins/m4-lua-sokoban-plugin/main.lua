-- com.m4.sokoban — 推箱子 on 480x800 e-ink. Static frames only.

sys.load("game.lua")

dirty = false
frame_changed = true

local W, H = 480, 800
local TITLE_H = 36
local BTN_H = 48
local UNDO_MAX = 80
local PICK_PAGE_SIZE = 8
local pick_page, pick_cursor = 1, 1

local mode = "play" -- play | pick | help
local st, prog, undo_stack
local CELL, GRID_X, GRID_Y, GRID_W, GRID_H
local BTN = {}

local function layout()
  local by = H - BTN_H * 2 - 4
  local rows = st and st.rows or 8
  local cols = st and st.cols or 8
  CELL = math.max(18, math.min(
    math.floor((W - 16) / cols),
    math.floor((by - TITLE_H - 28) / rows)))
  GRID_W, GRID_H = cols * CELL, rows * CELL
  GRID_X = math.floor((W - GRID_W) / 2)
  GRID_Y = TITLE_H + 24 + math.floor((by - TITLE_H - 24 - GRID_H) / 2)
  local gap = 4
  local bw = math.floor((W - 16 - gap * 3) / 4)
  local y1 = H - BTN_H * 2 - 2
  local y2 = H - BTN_H
  local labels = {
    { id = "undo", t = "撤销" },
    { id = "reset", t = "重开" },
    { id = "pick", t = "选关" },
    { id = "help", t = "帮助" },
  }
  for i = 1, 4 do
    BTN[labels[i].id] = {
      x = 8 + (i - 1) * (bw + gap), y = y1, w = bw, h = BTN_H - 4, t = labels[i].t,
    }
  end
  local dirs = {
    { id = "left", t = "左" },
    { id = "up", t = "上" },
    { id = "down", t = "下" },
    { id = "right", t = "右" },
  }
  for i = 1, 4 do
    BTN[dirs[i].id] = {
      x = 8 + (i - 1) * (bw + gap), y = y2, w = bw, h = BTN_H - 2, t = dirs[i].t,
    }
  end
  BTN.back = { x = 8, y = H - BTN_H, w = W - 16, h = BTN_H - 2, t = "返回" }
  BTN.prev = { x = 12, y = 572, w = 218, h = 58, t = "上一章" }
  BTN.nextpage = { x = 250, y = 572, w = 218, h = 58, t = "下一章" }
  BTN.nextlevel = { x = 8, y = H - BTN_H * 2 - 2, w = W - 16, h = BTN_H - 4, t = "下一关" }
  BTN.winpick = { x = 8, y = H - BTN_H, w = W - 16, h = BTN_H - 2, t = "返回选关" }
end

local function persist()
  if type(fs) ~= "table" or type(fs.writeFile) ~= "function" then return end
  pcall(function()
    fs.writeFile("microban_progress.csv", Game.serialize_progress(prog))
  end)
end

local function fresh_level(index)
  st = Game.load(index)
  undo_stack = {}
  prog.level = index
end

local function advance_level()
  if st.index < #Game.LEVELS then
    fresh_level(st.index + 1)
    layout()
    persist()
  else
    pick_cursor = st.index
    pick_page = math.floor((pick_cursor - 1) / PICK_PAGE_SIZE) + 1
    mode = "pick"
  end
  frame_changed = true
end

local function push_undo()
  undo_stack[#undo_stack + 1] = Game.snapshot(st)
  if #undo_stack > UNDO_MAX then
    table.remove(undo_stack, 1)
  end
end

function restart()
  fresh_level(prog.level)
  persist()
  frame_changed = true
end

local function try_restore()
  prog = Game.blank_progress()
  if type(fs) ~= "table" or type(fs.readFile) ~= "function" then return end
  local ok, data = pcall(function() return fs.readFile("microban_progress.csv") end)
  if ok and type(data) == "string" and data ~= "" then
    prog = Game.deserialize_progress(data)
  end
end

function init()
  try_restore()
  fresh_level(prog.level)
  layout()
  frame_changed = true
end

local function after_move()
  if Game.won(st) then
    Game.note_clear(prog, st.index, st.moves, st.pushes)
    persist()
  end
  frame_changed = true
end

local function try_dir(dr, dc)
  if mode ~= "play" then
    frame_changed = false
    return
  end
  if Game.won(st) then
    frame_changed = false
    return
  end
  push_undo()
  local kind = Game.step(st, dr, dc)
  if not kind then
    undo_stack[#undo_stack] = nil
    frame_changed = false
    return
  end
  after_move()
end

local function do_undo()
  if mode ~= "play" then return end
  local snap = undo_stack[#undo_stack]
  if not snap then
    frame_changed = false
    return
  end
  undo_stack[#undo_stack] = nil
  Game.restore(st, snap)
  frame_changed = true
end

local function hit(b, x, y)
  return b and x >= b.x and x < b.x + b.w and y >= b.y and y < b.y + b.h
end

local function draw_btn(b)
  gui.drawRect(b.x, b.y, b.w, b.h)
  local font = 12
  local tw = gui.textWidth(font, b.t)
  local th = gui.lineHeight(font)
  gui.drawText(font, b.x + math.floor((b.w - tw) / 2),
    b.y + math.floor((b.h - th) / 2), b.t)
end

local function cell_xy(r, c)
  return GRID_X + (c - 1) * CELL, GRID_Y + (r - 1) * CELL
end

local function draw_play()
  local title = "推箱子  " .. tostring(st.index) .. "/" .. tostring(#Game.LEVELS)
  if Game.won(st) then title = "过关  " .. title end
  if Game.deadlocked(st) and not Game.won(st) then title = "死角  " .. title end
  gui.drawText(16, 12, 8, title)
  local bm = prog.best_m[st.index] or 0
  local sub = "步 " .. tostring(st.moves) .. "  推 " .. tostring(st.pushes)
  if bm > 0 then sub = sub .. "  最佳 " .. tostring(bm) end
  gui.drawText(12, 12, 28, sub)

  gui.drawRect(GRID_X - 2, GRID_Y - 2, GRID_W + 4, GRID_H + 4)
  for r = 1, st.rows do
    for c = 1, st.cols do
      local k = Game.key(r, c)
      local x, y = cell_xy(r, c)
      if st.walls[k] then
        gui.fillRect(x, y, CELL, CELL)
      else
        if st.goals[k] then
          local m = math.max(2, math.floor(CELL / 5))
          gui.drawRect(x + m, y + m, CELL - m * 2, CELL - m * 2)
        end
        if st.crates[k] then
          local m = math.max(1, math.floor(CELL / 8))
          gui.drawRect(x + m, y + m, CELL - m * 2, CELL - m * 2)
          gui.drawRect(x + m * 3, y + m * 3, CELL - m * 6, CELL - m * 6)
        end
      end
    end
  end
  local px, py = cell_xy(st.pr, st.pc)
  local m = math.max(3, math.floor(CELL / 4))
  gui.fillRect(px + m, py + m, CELL - m * 2, CELL - m * 2)

  if Game.won(st) then
    if st.index == #Game.LEVELS then BTN.nextlevel.t = "全部通关" else BTN.nextlevel.t = "下一关" end
    draw_btn(BTN.nextlevel)
    draw_btn(BTN.winpick)
  else
    draw_btn(BTN.undo)
    draw_btn(BTN.reset)
    draw_btn(BTN.pick)
    draw_btn(BTN.help)
    draw_btn(BTN.left)
    draw_btn(BTN.up)
    draw_btn(BTN.down)
    draw_btn(BTN.right)
  end
end

local function draw_pick()
  local first = (pick_page - 1) * PICK_PAGE_SIZE + 1
  local last = math.min(first + PICK_PAGE_SIZE - 1, #Game.LEVELS)
  gui.drawText(16, 12, 8, (Game.CHAPTERS[pick_page] or "选关") .. "  " .. tostring(pick_page) .. "/3")
  local y, row_h = 62, 56
  for i = first, last do
    local mark = prog.cleared[i] == 1 and "已过" or "  "
    local best = prog.best_m[i] > 0 and ("  最佳" .. tostring(prog.best_m[i])) or ""
    local label = mark .. "  " .. tostring(i) .. "/24 关" .. best
    gui.drawRect(12, y, W - 24, row_h - 6)
    if i == pick_cursor then gui.drawRect(16, y + 4, W - 32, row_h - 14) end
    gui.drawText(16, 24, y + 12, label)
    y = y + row_h
  end
  if pick_page > 1 then draw_btn(BTN.prev) end
  if last < #Game.LEVELS then draw_btn(BTN.nextpage) end
  draw_btn(BTN.back)
end

local function draw_help()
  gui.drawText(16, 12, 8, "帮助")
  local lines = {
    "把箱子推到目标框上。",
    "方向键或底部上下左右走一步。",
    "点玩家旁边的空格也走一步。",
    "箱子不能穿墙，也不能连推。",
    "非目标死角上的箱子是死局。",
    "撤销最多 80 步。重开本关。",
    "过关记录步数和推动次数。",
    "选关或帮助时点返回。",
    "系统返回键离开本应用。",
  }
  local y = 52
  for i = 1, #lines do
    gui.drawText(16, 16, y, lines[i])
    y = y + 36
  end
  draw_btn(BTN.back)
end

function draw()
  gui.clear()
  if mode == "pick" then draw_pick()
  elseif mode == "help" then draw_help()
  else draw_play() end
  frame_changed = false
end

local DIRK = {
  left = { 0, -1 }, right = { 0, 1 }, up = { -1, 0 }, down = { 1, 0 },
}

function onKey(key)
  if mode == "pick" then
    if key == "back" then mode = "play"
    elseif key == "left" and pick_page > 1 then
      pick_page = pick_page - 1
      pick_cursor = (pick_page - 1) * PICK_PAGE_SIZE + 1
    elseif key == "right" and pick_page * PICK_PAGE_SIZE < #Game.LEVELS then
      pick_page = pick_page + 1
      pick_cursor = (pick_page - 1) * PICK_PAGE_SIZE + 1
    elseif key == "up" then
      pick_cursor = math.max((pick_page - 1) * PICK_PAGE_SIZE + 1, pick_cursor - 1)
    elseif key == "down" then
      pick_cursor = math.min(pick_page * PICK_PAGE_SIZE, #Game.LEVELS, pick_cursor + 1)
    elseif key == "confirm" then
      fresh_level(pick_cursor)
      persist()
      mode = "play"
      layout()
    end
    frame_changed = true
    return
  elseif mode == "help" then
    if key == "back" or key == "confirm" then mode = "play" end
    frame_changed = true
    return
  end
  if key == "back" then frame_changed = false; return end
  if key == "confirm" then
    if Game.won(st) then advance_level() else do_undo() end
    return
  end
  local d = DIRK[key]
  if d then try_dir(d[1], d[2]) end
end

local function pick_at(y)
  local y0, row_h = 62, 56
  if y < y0 or y >= y0 + PICK_PAGE_SIZE * row_h then return nil end
  local row = math.floor((y - y0) / row_h)
  if (y - y0) % row_h >= row_h - 6 then return nil end
  local i = (pick_page - 1) * PICK_PAGE_SIZE + row + 1
  if i > #Game.LEVELS then return nil end
  return i
end

function onTouch(x, y, phase)
  if phase and phase ~= "tap" then
    frame_changed = false
    return
  end
  if mode ~= "play" then
    if hit(BTN.back, x, y) then
      mode = "play"
      frame_changed = true
      return
    end
    if mode == "pick" then
      if hit(BTN.prev, x, y) and pick_page > 1 then
        pick_page = pick_page - 1
        pick_cursor = (pick_page - 1) * PICK_PAGE_SIZE + 1
        frame_changed = true
        return
      end
      if hit(BTN.nextpage, x, y) and pick_page * PICK_PAGE_SIZE < #Game.LEVELS then
        pick_page = pick_page + 1
        pick_cursor = (pick_page - 1) * PICK_PAGE_SIZE + 1
        frame_changed = true
        return
      end
      local i = pick_at(y)
      if i and x >= 12 and x < W - 12 then
        pick_cursor = i
        fresh_level(i)
        persist()
        mode = "play"
        layout()
        frame_changed = true
        return
      end
    end
    frame_changed = false
    return
  end
  if Game.won(st) then
    if hit(BTN.nextlevel, x, y) then advance_level(); return end
    if hit(BTN.winpick, x, y) then
      pick_cursor = st.index
      pick_page = math.floor((pick_cursor - 1) / PICK_PAGE_SIZE) + 1
      mode = "pick"
      frame_changed = true
      return
    end
    frame_changed = false
    return
  end
  if hit(BTN.undo, x, y) then do_undo() return end
  if hit(BTN.reset, x, y) then restart() return end
  if hit(BTN.pick, x, y) then
    pick_cursor = st.index
    pick_page = math.floor((pick_cursor - 1) / PICK_PAGE_SIZE) + 1
    mode = "pick"
    frame_changed = true
    return
  end
  if hit(BTN.help, x, y) then mode = "help" frame_changed = true return end
  for name, d in pairs(DIRK) do
    if hit(BTN[name], x, y) then try_dir(d[1], d[2]) return end
  end
  if x >= GRID_X and y >= GRID_Y and x < GRID_X + GRID_W and y < GRID_Y + GRID_H then
    local c = math.floor((x - GRID_X) / CELL) + 1
    local r = math.floor((y - GRID_Y) / CELL) + 1
    local dr, dc = r - st.pr, c - st.pc
    if math.abs(dr) + math.abs(dc) == 1 then
      try_dir(dr, dc)
      return
    end
  end
  frame_changed = false
end
