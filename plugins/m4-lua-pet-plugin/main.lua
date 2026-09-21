-- pet plugin single-file entry (ASCII header)
-- Host callbacks: init draw onKey onTouch

-- Tamagotchi-style pet rules. No gui/sys; host tests can dofile() this file.
-- Rules follow ReKindle pet.html (hearts 0-4, feed/play/meds/bath, egg→child→teen→adult→dead)
-- with Tamagotchi Pi-style hunger/happy decay on real elapsed time (e-ink: tick on open/key).

Pet = Pet or {}

Pet.MAX = 4
Pet.EGG_S = 60
Pet.CHILD_S = 300
Pet.TEEN_S = 600
Pet.HUNGER_S = 1800
Pet.HAPPY_S = 1800
Pet.POOP_S = 900
Pet.AGE_S = 60
Pet.DEATH_S = 120

function Pet.new(now)
  now = now or 0
  return {
    stage = "EGG",
    hunger = Pet.MAX,
    happy = Pet.MAX,
    poops = 0,
    sick = 0,
    age = 0,
    weight = 5,
    born = now,
    last = now,
    stage_start = now,
    age_update = now,
    hunger_t = now,
    happy_t = now,
    poop_t = now,
  }
end

function Pet.clone(s)
  local o = {}
  for k, v in pairs(s) do o[k] = v end
  return o
end

local function clamp(v, lo, hi)
  if v < lo then return lo end
  if v > hi then return hi end
  return v
end

function Pet.attention(s)
  return s.stage ~= "EGG" and s.stage ~= "DEAD"
    and (s.hunger == 0 or s.happy == 0 or s.sick == 1 or s.poops > 0)
end

function Pet.sprite_key(s)
  if s.stage == "EGG" then return "egg" end
  if s.stage == "DEAD" then return "grave" end
  if s.sick == 1 then return "pet_sick" end
  return "pet"
end

local function evolve(s, now)
  if s.stage == "EGG" then
    if now - s.born >= Pet.EGG_S then
      s.stage = "CHILD"
      s.stage_start = now
    end
  elseif s.stage == "CHILD" then
    if now - s.stage_start >= Pet.CHILD_S then
      s.stage = "TEEN"
      s.stage_start = now
    end
  elseif s.stage == "TEEN" then
    if now - s.stage_start >= Pet.TEEN_S then
      s.stage = "ADULT"
      s.stage_start = now
    end
  end
end

-- Count elapsed periods in O(1). A while-step catchup over Unix-scale
-- clocks (RTC 0 → time()) exceeds the Lua instruction budget.
local function catchup(t, period, now)
  if not t or not period or period <= 0 then return 0, now end
  if t > now then return 0, now end
  local n = math.floor((now - t) / period)
  if n < 0 then n = 0 end
  return n, t + n * period
end

function Pet.apply_time(s, now)
  if not now or now < s.last then now = s.last end
  if s.stage == "DEAD" then
    s.last = now
    return s
  end
  evolve(s, now)
  if s.stage ~= "EGG" then
    local n
    n, s.hunger_t = catchup(s.hunger_t, Pet.HUNGER_S, now)
    if n > 0 then s.hunger = clamp(s.hunger - n, 0, Pet.MAX) end
    n, s.happy_t = catchup(s.happy_t, Pet.HAPPY_S, now)
    if n > 0 then s.happy = clamp(s.happy - n, 0, Pet.MAX) end
    n, s.poop_t = catchup(s.poop_t, Pet.POOP_S, now)
    if n > 0 and s.hunger > 0 then s.poops = clamp(s.poops + n, 0, Pet.MAX) end
    n, s.age_update = catchup(s.age_update, Pet.AGE_S, now)
    if n > 0 then s.age = s.age + n end
    if s.poops >= 3 or s.hunger == 0 or s.happy == 0 then
      s.sick = 1
    end
    if s.sick == 1 and s.hunger == 0 and now - s.last >= Pet.DEATH_S then
      -- death checked against last care; use hunger_t as last neglected
      if now - s.hunger_t >= Pet.DEATH_S then
        s.stage = "DEAD"
      end
    end
  end
  s.last = now
  return s
end

function Pet.feed(s, kind)
  if s.stage == "EGG" or s.stage == "DEAD" then return false end
  if kind == "snack" then
    if s.happy >= Pet.MAX then return false end
    s.happy = s.happy + 1
    s.weight = s.weight + 2
    return true
  end
  if s.hunger >= Pet.MAX then return false end
  s.hunger = s.hunger + 1
  s.weight = s.weight + 1
  return true
end

function Pet.play(s)
  if s.stage == "EGG" or s.stage == "DEAD" then return false end
  if s.happy >= Pet.MAX then return false end
  s.happy = s.happy + 1
  s.weight = math.max(1, s.weight - 1)
  return true
end

function Pet.meds(s)
  if s.stage == "EGG" or s.stage == "DEAD" then return false end
  if s.sick ~= 1 then return false end
  s.sick = 0
  return true
end

function Pet.bath(s)
  if s.stage == "EGG" or s.stage == "DEAD" then return false end
  if s.poops <= 0 then return false end
  s.poops = 0
  return true
end

function Pet.restart(now)
  return Pet.new(now)
end

function Pet.serialize(s)
  local keys = {
    "stage", "hunger", "happy", "poops", "sick", "age", "weight",
    "born", "last", "stage_start", "age_update", "hunger_t", "happy_t", "poop_t",
  }
  local parts = {}
  for i = 1, #keys do
    parts[i] = keys[i] .. "=" .. tostring(s[keys[i]])
  end
  return table.concat(parts, ",")
end

function Pet.deserialize(str)
  if type(str) ~= "string" or str == "" then return nil end
  local s = Pet.new(0)
  for pair in string.gmatch(str, "[^,]+") do
    local k, v = pair:match("^([^=]+)=(.*)$")
    if k and v then
      if k == "stage" then
        s.stage = v
      else
        s[k] = tonumber(v) or 0
      end
    end
  end
  if s.stage ~= "EGG" and s.stage ~= "CHILD" and s.stage ~= "TEEN"
      and s.stage ~= "ADULT" and s.stage ~= "DEAD" then
    return nil
  end
  return s
end

-- 1-bit sprites packed as row strings (24x24). Generated from e-ink art.
Sprites = Sprites or {}
Sprites.SIZE = 24
Sprites.DATA = {
  egg = {
    "000000000000000000000000",
    "000000000000000000000000",
    "000000000001100000000000",
    "000000000110111000000000",
    "000000001001100100000000",
    "000000010001000010000000",
    "000000110011000011000000",
    "000000100001100001000000",
    "000001100000110001100000",
    "000001000001000000100000",
    "000001000011000000100000",
    "000011000001000000110000",
    "000010000000110000010000",
    "000010000000110000110000",
    "000010000000100000010000",
    "000010100001000000010000",
    "000011100011000000110000",
    "000001000000100000100000",
    "000001100000110011100000",
    "000000110000100011000000",
    "000000011101001110000000",
    "000000000111111000000000",
    "000000000000000000000000",
    "000000000000000000000000",
  },
  pet = {
    "000000000000000000000000",
    "000110000000000000011000",
    "000111000111111000111100",
    "000100111100001111001100",
    "001101110000000011101100",
    "000111000000000000111000",
    "000110001000000100011000",
    "000100011100000110001000",
    "001100101100001011001100",
    "001100111100001111001100",
    "001000111100001111000100",
    "001000111100001111000100",
    "001000011000000110000100",
    "001000000000000000000100",
    "001000000100001000000100",
    "001100000111111000001100",
    "001100000011110000001100",
    "000110000000000000011000",
    "000011110000000001110000",
    "000000110111111001000000",
    "000000110110011001000000",
    "000000111110011111000000",
    "000000011100001110000000",
    "000000000000000000000000",
  },
  pet_eat = {
    "000000000000000000000000",
    "000110000000000000011000",
    "000111000111111000111100",
    "000100111100001111001100",
    "000101110000000011101100",
    "000111000000000000111000",
    "000110001000000100011000",
    "000100011000000110001000",
    "000100101100001011001100",
    "000100111100001111001100",
    "001000111100001111000100",
    "001000111100001111000100",
    "001000011000000110000100",
    "001000000001100000000100",
    "001000000001100000000100",
    "001100000110011000001100",
    "001100000000001000001100",
    "000110000001110000011000",
    "000011110000000011110000",
    "000000110111111011000000",
    "000000110110011011000000",
    "000000111110011111000000",
    "000000011100001110000000",
    "000000000000000000000000",
  },
  pet_sick = {
    "000000000000000000000000",
    "000110000000000000011000",
    "000111000111111000111100",
    "000100111100001111001100",
    "001101110000000011101100",
    "000111000000000000111000",
    "000110000000000000011000",
    "000100000000000000001000",
    "001100110100001011001100",
    "001100011000000110001100",
    "001000011100000110000100",
    "001000100100001001000100",
    "001000000000000000000100",
    "001000000000000000000100",
    "001000000011110000000100",
    "001100000110011000001100",
    "001100000100001000001100",
    "000110000000000000011000",
    "000011110000000001110000",
    "000000110111111001000000",
    "000000110110011001000000",
    "000000111110011111000000",
    "000000011100001110000000",
    "000000000000000000000000",
  },
  grave = {
    "000000000000000000000000",
    "000000000000000000000000",
    "000000000000000000000000",
    "000000000000000000000000",
    "000000000000000010000000",
    "000000000000010001000000",
    "000000000000010000000000",
    "000000000000010001000000",
    "000000000000000000000000",
    "000000000100011000000000",
    "000000001000011100000000",
    "000000001101100100000000",
    "000000000000001000000000",
    "000000000000001100000000",
    "000000000000001100000000",
    "000000000000001100000000",
    "000000000000001000000000",
    "000000000100000100000000",
    "000000011111111110000000",
    "000000111111111111000000",
    "000000000000000000000000",
    "000000000000000000000000",
    "000000000000000000000000",
    "000000000000000000000000",
  },
}

function Sprites.runs(row)
  local segs, x, n = {}, 1, #row
  while x <= n do
    if row:sub(x, x) == '1' then
      local x0 = x
      while x <= n and row:sub(x, x) == '1' do x = x + 1 end
      segs[#segs + 1] = { x0, x - x0 }
    else
      x = x + 1
    end
  end
  return segs
end

frame_changed = true
screen = "main"
menu_i = 1
stats_page = 0
feed_kind = 1 -- 1 meal, 2 snack
msg = ""

local ACTIONS = { "feed", "play", "meds", "bath", "stats", "reset" }
local LABELS = { "喂食", "玩耍", "吃药", "打扫", "状态", "新蛋" }

local W, H = 480, 800
local SCALE = 8

local function now_s()
  if type(sys) == "table" and type(sys.time) == "function" then
    return math.floor(sys.time())
  end
  if type(sys) == "table" and type(sys.millis) == "function" then
    return math.floor(sys.millis() / 1000)
  end
  return 0
end

local function persist()
  if type(fs) ~= "table" or type(fs.writeFile) ~= "function" then return end
  pcall(function()
    fs.writeFile("state.csv", Pet.serialize(state))
  end)
end

local function try_restore()
  if type(fs) ~= "table" or type(fs.readFile) ~= "function" then return false end
  local ok, data = pcall(function() return fs.readFile("state.csv") end)
  if not ok or type(data) ~= "string" or data == "" then return false end
  local st = Pet.deserialize(data)
  if not st then return false end
  state = st
  Pet.apply_time(state, now_s())
  return true
end

local function bump()
  Pet.apply_time(state, now_s())
  persist()
  frame_changed = true
end

function init()
  state = Pet.new(now_s())
  if not try_restore() then
    persist()
  end
  frame_changed = true
end

local function draw_sprite(key, ox, oy, scale)
  local rows = Sprites.DATA[key]
  if not rows then return end
  scale = scale or SCALE
  for y = 1, #rows do
    local segs = Sprites.runs(rows[y])
    for i = 1, #segs do
      local x0, w = segs[i][1], segs[i][2]
      gui.fillRect(ox + (x0 - 1) * scale, oy + (y - 1) * scale, w * scale, scale)
    end
  end
end

local function draw_hearts(n, x, y)
  for i = 0, 3 do
    local hx = x + i * 36
    gui.drawRect(hx, y, 28, 24)
    if i < n then
      gui.fillRect(hx + 4, y + 4, 20, 16)
    end
  end
end

local BTN = {}
local function layout_btns()
  local bw, bh, gap = 140, 56, 12
  local x0, y0 = 24, 520
  for i = 1, 6 do
    local col = (i - 1) % 3
    local row = math.floor((i - 1) / 3)
    BTN[i] = { x = x0 + col * (bw + gap), y = y0 + row * (bh + gap), w = bw, h = bh }
  end
end
layout_btns()

local function hit(b, x, y)
  return x >= b.x and x < b.x + b.w and y >= b.y and y < b.y + b.h
end

local function draw_box(b, lab)
  gui.drawRect(b.x, b.y, b.w, b.h)
  local tw = gui.textWidth(16, lab)
  gui.drawText(16, b.x + math.floor((b.w - tw) / 2), b.y + math.floor((b.h - 22) / 2), lab)
end

local function draw_btn(i)
  draw_box(BTN[i], LABELS[i])
end

local FEED_HIT = {
  { x = 40, y = 340, w = 400, h = 56, kind = "food", lab = "正餐" },
  { x = 40, y = 408, w = 400, h = 56, kind = "snack", lab = "点心" },
}
local CONF_HIT = {
  { x = 40, y = 400, w = 180, h = 56, act = "cancel", lab = "取消" },
  { x = 260, y = 400, w = 180, h = 56, act = "ok", lab = "确定" },
}
local STATS_HIT = { x = 40, y = 340, w = 400, h = 150 }

local function do_action(i)
  Pet.apply_time(state, now_s())
  local act = ACTIONS[i]
  msg = ""
  if act == "feed" then
    screen = "feed"
  elseif act == "play" then
    if Pet.play(state) then msg = "玩耍" else msg = "不想玩" end
    screen = "main"
  elseif act == "meds" then
    if Pet.meds(state) then msg = "吃药" else msg = "没病" end
    screen = "main"
  elseif act == "bath" then
    if Pet.bath(state) then msg = "打扫" else msg = "很干净" end
    screen = "main"
  elseif act == "stats" then
    if screen == "stats" then
      stats_page = stats_page + 1
      if stats_page > 2 then
        stats_page = 0
        screen = "main"
      end
    else
      screen = "stats"
      stats_page = 0
    end
  elseif act == "reset" then
    if state.stage == "DEAD" or screen == "confirm_reset" then
      state = Pet.restart(now_s())
      screen = "main"
      msg = "新蛋"
    else
      screen = "confirm_reset"
    end
  end
  persist()
  frame_changed = true
end

local function do_feed(kind)
  Pet.apply_time(state, now_s())
  if Pet.feed(state, kind) then
    msg = (kind == "snack") and "点心" or "正餐"
    screen = "main"
  else
    msg = "吃不下"
  end
  persist()
  frame_changed = true
end

function draw()
  Pet.apply_time(state, now_s())
  gui.clear()
  gui.drawText(16, 16, 12, "电子宠物")
  local age_s = "岁 " .. tostring(state.age)
  gui.drawText(12, W - 16 - gui.textWidth(12, age_s), 16, age_s)
  gui.drawLine(16, 48, W - 16, 48)

  if Pet.attention(state) then
    gui.drawText(12, 16, 58, "需要照顾")
  elseif state.stage == "EGG" then
    gui.drawText(12, 16, 58, "蛋在孵化")
  elseif state.stage == "DEAD" then
    gui.drawText(12, 16, 58, "确认开始新蛋")
  else
    gui.drawText(12, 16, 58, state.stage)
  end

  local key = Pet.sprite_key(state)
  if screen == "feed" and state.stage ~= "EGG" and state.stage ~= "DEAD" then
    key = "pet_eat"
  end
  local sw = Sprites.SIZE * SCALE
  draw_sprite(key, math.floor((W - sw) / 2), 96, SCALE)

  if state.poops > 0 and state.stage ~= "DEAD" then
    gui.fillRect(40, 320, 24, 16)
    if state.poops > 1 then gui.fillRect(W - 64, 320, 24, 16) end
    if state.poops > 2 then gui.fillRect(56, 300, 20, 14) end
  end

  if screen == "stats" then
    gui.drawRect(40, 340, 400, 150)
    if stats_page == 0 then
      gui.drawText(16, 60, 370, "年龄 " .. tostring(state.age))
      gui.drawText(16, 60, 410, "体重 " .. tostring(state.weight))
      gui.drawText(12, 60, 450, "阶段 " .. state.stage)
    elseif stats_page == 1 then
      gui.drawText(16, 60, 370, "饥饿")
      draw_hearts(state.hunger, 60, 410)
    else
      gui.drawText(16, 60, 370, "心情")
      draw_hearts(state.happy, 60, 410)
    end
  elseif screen == "feed" then
    for i = 1, #FEED_HIT do
      draw_box(FEED_HIT[i], FEED_HIT[i].lab)
    end
  elseif screen == "confirm_reset" then
    gui.drawText(16, 40, 360, "丢掉现在的宠物？")
    for i = 1, #CONF_HIT do
      draw_box(CONF_HIT[i], CONF_HIT[i].lab)
    end
  end

  if msg ~= "" then
    gui.drawText(12, 16, 490, msg)
  end

  for i = 1, 6 do draw_btn(i) end
  gui.drawText(10, 20, 770, "点按按钮")
end

function onKey(_key)
end

function onTouch(x, y, phase)
  if phase and phase ~= "tap" and phase ~= "up" and phase ~= "press" then
    return
  end
  if screen == "feed" then
    for i = 1, #FEED_HIT do
      if hit(FEED_HIT[i], x, y) then
        do_feed(FEED_HIT[i].kind)
        return
      end
    end
  end
  if screen == "confirm_reset" then
    for i = 1, #CONF_HIT do
      if hit(CONF_HIT[i], x, y) then
        if CONF_HIT[i].act == "ok" then
          Pet.apply_time(state, now_s())
          state = Pet.restart(now_s())
          screen = "main"
          msg = "新蛋"
          persist()
          frame_changed = true
        else
          screen = "main"
          msg = ""
          frame_changed = true
        end
        return
      end
    end
  end
  if screen == "stats" and hit(STATS_HIT, x, y) then
    do_action(5)
    return
  end
  for i = 1, 6 do
    if hit(BTN[i], x, y) then
      do_action(i)
      return
    end
  end
end
