dofile((arg and arg[0] and arg[0]:match("(.*/)") or "./") .. "game.lua")

local fails = 0
local function eq(a, b, msg)
  if a ~= b then
    fails = fails + 1
    print("FAIL " .. msg .. " got=" .. tostring(a) .. " want=" .. tostring(b))
  end
end

do
  local s = Pet.new(0)
  eq(s.stage, "EGG", "start egg")
  Pet.apply_time(s, Pet.EGG_S - 1)
  eq(s.stage, "EGG", "still egg")
  Pet.apply_time(s, Pet.EGG_S)
  eq(s.stage, "CHILD", "hatch")
end

do
  local s = Pet.new(0)
  Pet.apply_time(s, Pet.EGG_S)
  Pet.apply_time(s, Pet.EGG_S + Pet.CHILD_S)
  eq(s.stage, "TEEN", "child to teen")
  Pet.apply_time(s, Pet.EGG_S + Pet.CHILD_S + Pet.TEEN_S)
  eq(s.stage, "ADULT", "teen to adult")
end

do
  local s = Pet.new(0)
  Pet.apply_time(s, Pet.EGG_S)
  eq(Pet.feed(s, "food"), false, "full cannot meal")
  s.hunger = 2
  eq(Pet.feed(s, "food"), true, "meal")
  eq(s.hunger, 3, "hunger +1")
  s.happy = 2
  eq(Pet.feed(s, "snack"), true, "snack")
  eq(s.happy, 3, "happy +1")
end

do
  local s = Pet.new(0)
  Pet.apply_time(s, Pet.EGG_S)
  s.happy = 2
  eq(Pet.play(s), true, "play")
  eq(s.happy, 3, "play happy")
  s.sick = 1
  eq(Pet.meds(s), true, "meds")
  eq(s.sick, 0, "cured")
  s.poops = 2
  eq(Pet.bath(s), true, "bath")
  eq(s.poops, 0, "clean")
end

do
  local s = Pet.new(0)
  Pet.apply_time(s, Pet.EGG_S)
  local t0 = s.last
  Pet.apply_time(s, t0 + Pet.HUNGER_S * 2)
  eq(s.hunger, Pet.MAX - 2, "hunger decay 2")
end

do
  local s = Pet.new(0)
  Pet.apply_time(s, Pet.EGG_S)
  s.hunger = 0
  s.sick = 1
  s.hunger_t = s.last - Pet.DEATH_S
  Pet.apply_time(s, s.last + Pet.DEATH_S)
  eq(s.stage, "DEAD", "starved sick dies")
end

do
  local s = Pet.new(100)
  s.hunger = 1
  s.stage = "CHILD"
  local txt = Pet.serialize(s)
  local d = Pet.deserialize(txt)
  eq(d.hunger, 1, "roundtrip hunger")
  eq(d.stage, "CHILD", "roundtrip stage")
  eq(d.born, 100, "roundtrip born")
end

do
  eq(Pet.feed(Pet.new(0), "food"), false, "egg cannot feed")
end

do
  local s = Pet.new(0)
  Pet.apply_time(s, 1700000000)
  -- O(1) catchup must finish; years of neglect from t=0 is death.
  eq(s.stage, "DEAD", "unix jump neglect dies")
  eq(s.hunger, 0, "unix jump hunger floors")
  eq(s.happy, 0, "unix jump happy floors")
  if s.age < 1000 then
    fails = fails + 1
    print("FAIL unix jump age got=" .. tostring(s.age))
  end
end

if fails > 0 then
  error(fails .. " failures", 0)
end
print("test_game.lua OK")
