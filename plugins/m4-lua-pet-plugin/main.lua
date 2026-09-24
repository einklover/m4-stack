-- com.m4.pet — silhouette pet for 480x800 touch e-ink.
-- Host: init/draw/onTouch, gui.drawBmp when present, fs state.csv.
-- Refresh: paint only when the visible signature changes. Idle ticks skip.

sys.load("game.lua")

state = nil
screen = "home"
msg = ""
painted = ""
frame_changed = true
flash = ""
flash_held = false

local STAGE = {
  EGG = "蛋",
  BABY = "幼体",
  CHILD = "幼年",
  TEEN = "少年",
  GUARD = "守护",
  DEAD = "离开",
}

local LINE = {
  idle = "它看着你",
  happy = "它很开心",
  hungry = "它饿了",
  sad = "它有点闷",
  sick = "它不舒服",
  eat = "要吃什么？",
  play = "它在玩",
  power = "它提起了劲",
  sleep = "它在睡觉",
  egg = "还要再等一等",
  grave = "它离开了",
}

-- Missing stage poses fall back to that stage's idle, then the child pose.
local ART = {
  egg = "egg",
  grave = "grave",
  BABY = { idle = "baby_idle", eat = "baby_eat", happy = "baby_happy", sleep = "baby_sleep", sad = "baby_sad", hungry = "baby_sad", sick = "baby_sad", play = "baby_happy", power = "baby_happy" },
  CHILD = { idle = "child_idle", eat = "child_eat", happy = "child_happy", sleep = "child_sleep", sad = "child_sad", hungry = "child_sad", sick = "child_sad", play = "child_play", power = "child_power" },
  TEEN = { idle = "teen_idle", eat = "teen_eat", happy = "teen_idle", sleep = "teen_sleep", sad = "teen_sad", hungry = "teen_sad", sick = "teen_sad", play = "teen_idle", power = "teen_idle" },
  GUARD = { idle = "guard_idle", eat = "guard_idle", happy = "guard_idle", sleep = "guard_sleep", sad = "guard_idle", hungry = "guard_idle", sick = "guard_idle", play = "guard_idle", power = "guard_power" },
}

local function now()
  if type(sys) == "table" and type(sys.time) == "function" then
    return sys.time()
  end
  return 0
end

local function persist()
  if type(fs) ~= "table" or type(fs.writeFile) ~= "function" then return end
  pcall(function()
    fs.writeFile("state.csv", Pet.serialize(state))
  end)
end

local function restore()
  if type(fs) ~= "table" or type(fs.readFile) ~= "function" then return nil end
  local ok, data = pcall(function() return fs.readFile("state.csv") end)
  if not ok or type(data) ~= "string" then return nil end
  return Pet.deserialize(data)
end

function init()
  state = restore()
  if not state then
    state = Pet.new(now())
    persist()
  end
  screen = "home"
  msg = ""
  flash = ""
  flash_held = false
  painted = ""
  frame_changed = true
end

local function hit(px, py, x, y, w, h)
  return px >= x and py >= y and px < x + w and py < y + h
end

local function note(text)
  msg = text
end

local function care_fail(kind)
  if state.stage == "EGG" then
    note("它还在蛋里")
    return
  end
  if state.stage == "DEAD" then
    note("它已经离开了")
    return
  end
  if kind == "meal" then note("它还不饿")
  elseif kind == "snack" then note("现在不想吃")
  elseif kind == "play" then note("它已经很开心")
  elseif kind == "meds" then note("它没生病")
  elseif kind == "bath" then note("已经很干净")
  end
end

local function note_evolve(prev)
  if state.stage ~= prev and state.stage ~= "DEAD" and state.stage ~= "EGG" then
    flash = "power"
    flash_held = false
  end
end

local function do_action(kind)
  local ok = false
  if kind == "meal" or kind == "snack" then
    ok = Pet.feed(state, kind)
  elseif kind == "play" then
    ok = Pet.play(state)
  elseif kind == "meds" then
    ok = Pet.meds(state)
  elseif kind == "bath" then
    ok = Pet.bath(state)
  end
  if ok then
    msg = ""
    if kind == "meal" or kind == "snack" then screen = "home" end
    if kind == "play" then
      flash = "play"
      flash_held = false
    elseif kind == "meds" then
      flash = "power"
      flash_held = false
    end
    persist()
  else
    care_fail(kind)
  end
end

function onTouch(x, y, kind)
  if kind ~= "tap" and kind ~= "up" and kind ~= "press" then return end
  if not state then return end
  x = x or 0
  y = y or 0
  local t = now()
  local prev = state.stage
  Pet.apply_time(state, t)
  note_evolve(prev)

  if screen == "confirm" then
    if hit(x, y, 22, 360, 436, 72) then
      state = Pet.restart(t)
      screen = "home"
      msg = ""
      flash = ""
      flash_held = false
      persist()
    elseif hit(x, y, 22, 452, 436, 72) then
      screen = "home"
      msg = ""
    end
    return
  end

  if screen == "feed" then
    if hit(x, y, 22, 520, 436, 72) then do_action("meal")
    elseif hit(x, y, 22, 612, 436, 72) then do_action("snack")
    elseif hit(x, y, 22, 704, 436, 72) then
      screen = "home"
      msg = ""
    end
    return
  end

  if state.stage == "DEAD" then
    if hit(x, y, 22, 616, 436, 88) then
      screen = "confirm"
      msg = ""
    end
    return
  end

  if hit(x, y, 150, 548, 180, 36) then
    screen = "confirm"
    msg = ""
    return
  end

  if state.stage == "EGG" then
    note("还要再等一等")
    return
  end

  local bw, bh, gap = 210, 72, 16
  local x0, y0 = 22, 616
  if hit(x, y, x0, y0, bw, bh) then
    screen = "feed"
    msg = ""
  elseif hit(x, y, x0 + bw + gap, y0, bw, bh) then
    do_action("play")
  elseif hit(x, y, x0, y0 + bh + gap, bw, bh) then
    do_action("bath")
  elseif hit(x, y, x0 + bw + gap, y0 + bh + gap, bw, bh) then
    do_action("meds")
  end
end

function onKey()
end

local function text_w(s, size)
  if type(gui.textWidth) == "function" then
    return gui.textWidth(size, s)
  end
  return #s * 8
end

local function center_text(s, cx, y, size)
  local w = text_w(s, size)
  gui.drawText(size, cx - math.floor(w / 2), y, s)
end

local function button(x, y, w, h, label)
  gui.drawRect(x, y, w, h)
  center_text(label, x + math.floor(w / 2), y + math.floor((h - 16) / 2), 16)
end

local function meter(x, y, label, filled)
  gui.drawText(10, x, y, label)
  local bx = x + 52
  for i = 0, Pet.MAX - 1 do
    local rx = bx + i * 22
    if i < filled then
      gui.fillRect(rx, y, 16, 14)
    else
      gui.drawRect(rx, y, 16, 14)
    end
  end
end

local function disc_fallback(x, y, w)
  local r = math.floor(w / 2)
  local cx = x + r
  local cy = y + r
  local step = 4
  for row = 0, w - 1, step do
    local dy = row + step / 2 - r
    local span = r * r - dy * dy
    if span > 0 then
      local half = math.floor(math.sqrt(span))
      gui.fillRect(cx - half, y + row, half * 2, step)
    end
  end
end

local function blit_pet(pose, x, y)
  local name
  if pose == "egg" or pose == "grave" then
    name = ART[pose]
  else
    local row = ART[state.stage] or ART.CHILD
    name = row[pose] or row.idle or "child_idle"
  end
  local rel = "art/" .. name .. ".bmp"
  if type(gui.drawBmp) == "function" and gui.drawBmp(rel, x, y) then
    return
  end
  disc_fallback(x, y, 160)
end

local function status_line(pose)
  if msg ~= "" then return msg end
  if pose == "sick" and state.hunger == 0 then return "它又饿又难受" end
  return LINE[pose] or ""
end

local function signature()
  return Pet.serialize(state) .. "|" .. screen .. "|" .. msg .. "|" .. flash
end

function draw()
  if not state then return end
  if flash_held then
    flash = ""
    flash_held = false
  end
  local before = Pet.serialize(state)
  local prev = state.stage
  Pet.apply_time(state, now())
  note_evolve(prev)
  local after = Pet.serialize(state)
  if after ~= before then persist() end

  local pending = frame_changed and true or false
  local sig = signature()
  if sig == painted then
    frame_changed = false
    return
  end

  gui.clear()
  local pose = Pet.pose(state, screen, now(), flash)
  local stage = STAGE[state.stage] or state.stage
  gui.drawText(16, 16, 16, "电子宠物")
  gui.drawText(16, 280, 20, stage)
  gui.drawText(10, 380, 22, tostring(state.age or 0))

  if state.stage ~= "EGG" and state.stage ~= "DEAD" then
    meter(16, 56, "饥饿", state.hunger)
    meter(16, 80, "心情", state.happy)
    meter(16, 104, "清洁", Pet.MAX - (state.poops or 0))
  end

  blit_pet(pose, 160, 168)
  if (state.poops or 0) > 0 and state.stage ~= "EGG" and state.stage ~= "DEAD" then
    local n = state.poops
    if n > 4 then n = 4 end
    for i = 1, n do
      gui.fillRect(340, 220 + (i - 1) * 22, 14, 10)
    end
  end

  if screen ~= "confirm" then
    center_text(status_line(pose), 240, 400, 16)
  end

  if screen == "confirm" then
    center_text("重新养一只？", 240, 280, 16)
    button(22, 360, 436, 72, "确定")
    button(22, 452, 436, 72, "返回")
  elseif screen == "feed" then
    button(22, 520, 436, 72, "正餐")
    button(22, 612, 436, 72, "点心")
    button(22, 704, 436, 72, "返回")
  elseif state.stage == "DEAD" then
    button(22, 616, 436, 88, "再养一只")
  elseif state.stage == "EGG" then
    center_text("新蛋", 240, 556, 10)
  else
    center_text("新蛋", 240, 556, 10)
    local bw, bh, gap = 210, 72, 16
    local x0, y0 = 22, 616
    button(x0, y0, bw, bh, "喂食")
    button(x0 + bw + gap, y0, bw, bh, "玩耍")
    button(x0, y0 + bh + gap, bw, bh, "清洁")
    button(x0 + bw + gap, y0 + bh + gap, bw, bh, "治疗")
  end

  painted = sig
  frame_changed = not pending
  if flash ~= "" then flash_held = true end
end
