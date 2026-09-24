-- Care rules for the pebble pet. No gui/sys; host tests can dofile() this file.
-- Hearts 0-4. One line: egg → baby → child → teen → guard. Dead is separate.
-- Hunger and happy decay on real elapsed time. Night hours count half speed.

Pet = Pet or {}

Pet.MAX = 4
Pet.EGG_S = 60
Pet.BABY_S = 300
Pet.CHILD_S = 900
Pet.TEEN_S = 1800
Pet.HUNGER_S = 1800
Pet.HAPPY_S = 1800
Pet.POOP_S = 900
Pet.AGE_S = 60
Pet.DEATH_S = 120
Pet.TZ_S = 8 * 3600

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

-- Local hour without os.date. 22:00–07:00 is sleep; 07:00 is awake.
function Pet.sleeping(now)
  if not now or now <= 0 then return false end
  local hour = math.floor((now + Pet.TZ_S) / 3600) % 24
  return hour >= 22 or hour < 7
end

-- Hunger 0 and happy 0 also set the sick flag, so those faces are chosen
-- before the sick face. flash is "play" or "power" for one painted frame.
function Pet.pose(s, screen, now, flash)
  if s.stage == "DEAD" then return "grave" end
  if s.stage == "EGG" then return "egg" end
  if screen == "feed" then return "eat" end
  if flash == "play" or screen == "play" then return "play" end
  if flash == "power" then return "power" end
  if Pet.sleeping(now) then return "sleep" end
  if s.hunger == 0 then return "hungry" end
  if s.happy == 0 then return "sad" end
  if s.sick == 1 then return "sick" end
  if s.happy >= 3 and s.hunger >= 3 then return "happy" end
  return "idle"
end

local NEXT = {
  EGG = { "BABY", "EGG_S", "born" },
  BABY = { "CHILD", "BABY_S", "stage_start" },
  CHILD = { "TEEN", "CHILD_S", "stage_start" },
  TEEN = { "GUARD", "TEEN_S", "stage_start" },
}

local function evolve(s, now)
  for _ = 1, 4 do
    local step = NEXT[s.stage]
    if not step then return end
    local origin = s[step[3]]
    local due = origin + Pet[step[2]]
    if now < due then return end
    local from_egg = s.stage == "EGG"
    s.stage = step[1]
    s.stage_start = due
    if from_egg then
      s.hunger_t = due
      s.happy_t = due
      s.poop_t = due
      s.age_update = due
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
    local hunger_s = Pet.HUNGER_S
    local happy_s = Pet.HAPPY_S
    if Pet.sleeping(now) then
      hunger_s = hunger_s * 2
      happy_s = happy_s * 2
    end
    n, s.hunger_t = catchup(s.hunger_t, hunger_s, now)
    if n > 0 then s.hunger = clamp(s.hunger - n, 0, Pet.MAX) end
    n, s.happy_t = catchup(s.happy_t, happy_s, now)
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
  if s.stage == "ADULT" then s.stage = "GUARD" end
  if s.stage ~= "EGG" and s.stage ~= "BABY" and s.stage ~= "CHILD"
      and s.stage ~= "TEEN" and s.stage ~= "GUARD" and s.stage ~= "DEAD" then
    return nil
  end
  return s
end
