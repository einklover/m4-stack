-- com.m4.bulls — 几A几B on 480x800 e-ink. No animation. Refresh only on change.

sys.load("game.lua")

dirty = false
frame_changed = true

local W, H = 480, 800
local TITLE_H = 36

local function layout()
  local pad_top = 300
  local gap = 6
  local cols = 3
  local bw = math.floor((W - 16 - gap * (cols - 1)) / cols)
  local bh = 52
  BTN = {}
  local function place(row, labels, y)
    local n = #labels
    local w = math.floor((W - 16 - gap * (n - 1)) / n)
    for i = 1, n do
      BTN[#BTN + 1] = {
        x = 8 + (i - 1) * (w + gap),
        y = y,
        w = w,
        h = bh,
        row = row,
        col = i,
        label = labels[i],
      }
    end
  end
  place(1, { "1", "2", "3" }, pad_top)
  place(2, { "4", "5", "6" }, pad_top + (bh + gap))
  place(3, { "7", "8", "9" }, pad_top + 2 * (bh + gap))
  place(4, { "C", "0", "DEL" }, pad_top + 3 * (bh + gap))
  place(5, { "3位", "4位", "5位" }, pad_top + 4 * (bh + gap))
  place(6, { "hint", "ok", "new" }, pad_top + 5 * (bh + gap))
  place(7, { "help" }, pad_top + 6 * (bh + gap))
end

local function persist()
  if type(fs) ~= "table" or type(fs.writeFile) ~= "function" then return end
  pcall(function()
    fs.writeFile("state.csv", Game.serialize(S))
  end)
end

local function try_restore()
  if type(fs) ~= "table" or type(fs.readFile) ~= "function" then return false end
  local ok, data = pcall(function() return fs.readFile("state.csv") end)
  if not ok or type(data) ~= "string" or data == "" then return false end
  local st = Game.deserialize(data)
  if not st then return false end
  S = st
  return true
end

function init()
  layout()
  if not try_restore() then
    S = Game.new_state(4, 1)
    persist()
  end
  frame_changed = true
end

local function hit(b, x, y)
  return x >= b.x and x < b.x + b.w and y >= b.y and y < b.y + b.h
end

local function act_button(b)
  S.focus_r, S.focus_c = b.row, b.col
  if S.help then
    S.help = false
    frame_changed = true
    persist()
    return
  end
  if S.won and b.row ~= 5 and b.label ~= "new" and b.label ~= "help" then
    Game.restart(S, S.len)
    frame_changed = true
    persist()
    return
  end
  local status = Game.activate(S)
  if status == "bad" then
    frame_changed = false
    return
  end
  frame_changed = true
  persist()
end

function draw()
  gui.clear()
  local title = "几A几B  " .. tostring(S.len) .. "位  无前导0"
  if S.won then title = "WIN  " .. title end
  gui.drawText(16, 8, 6, title)
  local stat = "猜" .. tostring(S.guesses or 0)
    .. "  胜" .. tostring(S.wins or 0)
    .. "  弃" .. tostring(S.giveups or 0)
    .. "  最佳" .. tostring(S.best or 0)
  gui.drawText(12, 8, 28, stat)

  local entry = S.entry or ""
  local shown = entry
  while #shown < S.len do shown = shown .. "_" end
  gui.drawText(16, 16, 52, shown)
  local hint = (S.hint and S.hint ~= "") and ("不含 " .. S.hint) or "提示: 不含的数字"
  gui.drawText(12, 220, 56, hint)

  local y = 84
  local hist = S.history or {}
  local start = 1
  if #hist > 8 then start = #hist - 7 end
  for i = start, #hist do
    local h = hist[i]
    gui.drawText(12, 16, y, h.g .. "   " .. Game.mark(h.a, h.b))
    y = y + 24
  end
  if #hist == 0 then
    gui.drawText(12, 16, y, "输入 " .. tostring(S.len) .. " 个不同数字")
  end

  for i = 1, #BTN do
    local b = BTN[i]
    local on = (b.row == S.focus_r and b.col == S.focus_c)
    gui.drawRect(b.x, b.y, b.w, b.h)
    if on then gui.drawRect(b.x + 3, b.y + 3, b.w - 6, b.h - 6) end
    local font = 16
    if #b.label > 2 then font = 12 end
    local tw = gui.textWidth(font, b.label)
    local th = gui.lineHeight(font)
    gui.drawText(font, b.x + math.floor((b.w - tw) / 2),
      b.y + math.floor((b.h - th) / 2), b.label)
  end

  if S.help then
    gui.fillRect(24, 180, W - 48, 280)
    gui.drawText(16, 40, 196, "几A几B")
    gui.drawText(12, 40, 230, "A 数字对且位置对")
    gui.drawText(12, 40, 254, "B 数字对但位置错")
    gui.drawText(12, 40, 278, "每位不同 不能以0开头")
    gui.drawText(12, 40, 302, "方向键移动 确认键按下")
    gui.drawText(12, 40, 326, "赢之前不显示答案")
    gui.drawText(12, 40, 350, "机身返回键离开")
    gui.drawText(12, 40, 390, "再按确认或点按关闭")
  end

  if S.won then
    gui.drawText(16, 16, 268, "答案 " .. S.secret)
  end
  frame_changed = false
end

function onKey(key)
  if not S then return end
  local changed = Game.on_key(S, key)
  if changed then
    frame_changed = true
    persist()
  else
    frame_changed = false
  end
end

function onTouch(x, y, phase)
  if phase and phase ~= "tap" then return end
  if not S then return end
  for i = 1, #BTN do
    if hit(BTN[i], x, y) then
      act_button(BTN[i])
      return
    end
  end
  if S.help then
    S.help = false
    frame_changed = true
    persist()
    return
  end
  frame_changed = false
end
