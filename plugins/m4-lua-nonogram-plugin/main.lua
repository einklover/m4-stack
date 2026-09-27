-- com.m4.nonogram — 数织 on 480x800 e-ink.
-- One full refresh only when frame_changed is set. No animation.

sys.load("game.lua")

dirty = false
frame_changed = true

local W, H = 480, 800
local screen = "play"
local play
local lay
local BTN

local LABELS = {
  exit = "退出",
  help = "帮助",
  undo = "撤销",
  hint = "提示",
  reset = "重开",
  prev = "上关",
  next = "下关",
}

local function relayout()
  local lv = Game.level(play.level)
  lay = Game.layout(lv.n, W, H)
  BTN = Game.buttons(W, H)
end

local function persist()
  if type(fs) ~= "table" or type(fs.writeFile) ~= "function" then return end
  pcall(function()
    fs.writeFile("state.csv", Game.serialize(play))
  end)
end

local function mark()
  frame_changed = true
  persist()
end

function restart_level()
  Game.reset_grid(play)
  screen = "play"
  mark()
end

local function change_level(delta)
  local n = Game.level_count()
  local i = play.level + delta
  if i < 1 then i = n end
  if i > n then i = 1 end
  Game.set_level(play, i)
  screen = "play"
  relayout()
  mark()
end

local function try_restore()
  if type(fs) ~= "table" or type(fs.readFile) ~= "function" then return false end
  local ok, data = pcall(function() return fs.readFile("state.csv") end)
  if not ok or type(data) ~= "string" or data == "" then return false end
  local st = Game.deserialize(data)
  if not st then return false end
  play = st
  return true
end

function init()
  play = Game.new_play(1)
  if not try_restore() then
    play = Game.new_play(1)
  end
  relayout()
  screen = "play"
  frame_changed = true
end

local function leave()
  if type(sys) == "table" and type(sys.exit) == "function" then
    sys.exit()
  end
  frame_changed = false
end

local function do_cycle(r, c)
  if screen ~= "play" then return end
  if Game.cycle(play, r, c) then
    mark()
  else
    frame_changed = false
  end
end

function onKey(key)
  if key == "back" then
    if screen == "help" then
      screen = "play"
      frame_changed = true
      return
    end
    leave()
    return
  end
  if screen == "help" then
    if key == "confirm" then
      screen = "play"
      frame_changed = true
    else
      frame_changed = false
    end
    return
  end
  if key == "confirm" then
    if play.won then
      change_level(1)
      return
    end
    do_cycle(play.r, play.c)
    return
  end
  local dr, dc = 0, 0
  if key == "left" then dc = -1
  elseif key == "right" then dc = 1
  elseif key == "up" then dr = -1
  elseif key == "down" then dr = 1
  else
    frame_changed = false
    return
  end
  local lv = Game.level(play.level)
  local r = play.r + dr
  local c = play.c + dc
  if r < 1 then r = 1 end
  if c < 1 then c = 1 end
  if r > lv.n then r = lv.n end
  if c > lv.n then c = lv.n end
  if r ~= play.r or c ~= play.c then
    play.r, play.c = r, c
    mark()
  else
    frame_changed = false
  end
end

function onTouch(x, y, phase)
  if phase and phase ~= "tap" then
    frame_changed = false
    return
  end
  if screen == "help" then
    screen = "play"
    frame_changed = true
    return
  end
  if Game.hit(BTN.exit, x, y) then leave() return end
  if Game.hit(BTN.help, x, y) then
    screen = "help"
    frame_changed = true
    return
  end
  if Game.hit(BTN.undo, x, y) then
    if Game.undo(play) then mark() else frame_changed = false end
    return
  end
  if Game.hit(BTN.hint, x, y) then
    if Game.hint(play) then mark() else frame_changed = false end
    return
  end
  if Game.hit(BTN.reset, x, y) then restart_level() return end
  if Game.hit(BTN.prev, x, y) then change_level(-1) return end
  if Game.hit(BTN.next, x, y) then change_level(1) return end
  local r, c = Game.cell_at(lay, x, y)
  if not r then
    frame_changed = false
    return
  end
  do_cycle(r, c)
end

local function draw_clues()
  local lv = Game.level(play.level)
  local rows, cols = Game.clues(lv)
  local font = 12
  local lh = gui.lineHeight(font)
  for c = 1, lv.n do
    local runs = cols[c]
    local lines = #runs == 0 and { "0" } or {}
    if #runs > 0 then
      for i = 1, #runs do lines[i] = tostring(runs[i]) end
    end
    local x = lay.grid_x + (c - 1) * lay.cell
    local y0 = lay.grid_y - #lines * lh - 1
    for i = 1, #lines do
      local tw = gui.textWidth(font, lines[i])
      local tx = x + math.floor((lay.cell - tw) / 2)
      gui.drawText(font, tx, y0 + (i - 1) * lh, lines[i])
    end
  end
  for r = 1, lv.n do
    local text = Game.clue_text(rows[r])
    local tw = gui.textWidth(font, text)
    local y = lay.grid_y + (r - 1) * lay.cell + math.floor((lay.cell - lh) / 2)
    local x = lay.grid_x - tw - 3
    if x < 1 then x = 1 end
    gui.drawText(font, x, y, text)
  end
end

local function draw_buttons()
  for _, name in ipairs({ "exit", "help", "undo", "hint", "reset", "prev", "next" }) do
    local b = BTN[name]
    gui.drawRect(b.x, b.y, b.w, b.h)
    local lab = LABELS[name]
    local tw = gui.textWidth(12, lab)
    local th = gui.lineHeight(12)
    gui.drawText(12, b.x + math.floor((b.w - tw) / 2), b.y + math.floor((b.h - th) / 2), lab)
  end
end

function draw()
  gui.clear()
  if screen == "help" then
    gui.drawText(16, 16, 16, "数织 帮助")
    local lines = {
      "点格循环: 空 -> 涂黑 -> X",
      "方向键移动光标, 确认落子",
      "撤销一步, 重开清空本关",
      "提示填一格正确颜色",
      "上关/下关切换题目",
      "返回键: 帮助回棋盘, 否则退出",
      "黑格与题面一致即过关",
      "进度保存在应用目录",
    }
    for i = 1, #lines do
      gui.drawText(12, 16, 56 + (i - 1) * 28, lines[i])
    end
    gui.drawText(12, 16, 56 + #lines * 28 + 8, "点屏幕关闭帮助")
    frame_changed = false
    return
  end

  local lv = Game.level(play.level)
  local title = (play.won and "完成 " or "") .. "数织 " .. lv.name
    .. " " .. tostring(play.level) .. "/" .. tostring(Game.level_count())
  gui.drawText(16, 8, 6, title)
  draw_clues()
  gui.drawRect(lay.grid_x - 1, lay.grid_y - 1, lay.grid_w + 2, lay.grid_h + 2)
  local n = lv.n
  for r = 1, n do
    for c = 1, n do
      local x = lay.grid_x + (c - 1) * lay.cell
      local y = lay.grid_y + (r - 1) * lay.cell
      gui.drawRect(x, y, lay.cell, lay.cell)
      local v = play.grid[(r - 1) * n + c]
      if v == Game.FILLED then
        local m = math.max(2, math.floor(lay.cell / 8))
        gui.fillRect(x + m, y + m, lay.cell - m * 2, lay.cell - m * 2)
      elseif v == Game.MARK then
        local m = math.max(3, math.floor(lay.cell / 5))
        gui.drawRect(x + m, y + m, lay.cell - m * 2, lay.cell - m * 2)
      end
      if r == play.r and c == play.c then
        gui.drawRect(x + 1, y + 1, lay.cell - 2, lay.cell - 2)
      end
    end
  end
  draw_buttons()
  frame_changed = false
end
