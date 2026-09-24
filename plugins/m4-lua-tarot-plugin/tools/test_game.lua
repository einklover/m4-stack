dofile((arg and arg[0] and arg[0]:match("(.*/)") or "./") .. "game.lua")
local fail=0
local function ok(v,msg) if not v then fail=fail+1; print("FAIL "..msg) end end
local function eq(a,b,msg) if a~=b then fail=fail+1; print("FAIL "..msg.." got="..tostring(a).." want="..tostring(b)) end end

eq(Tarot.count(),22,"major count")
eq(Tarot.card(1).name,"愚者","first card")
eq(Tarot.card(22).name,"世界","last card")

do
  local seq={0.0,0.9,0.0,0.1,0.0,0.9}
  local i=0
  local function r() i=i+1; return seq[i] or 0.5 end
  local a=Tarot.draw(3,r)
  eq(#a,3,"draw three")
  ok(a[1].index~=a[2].index and a[1].index~=a[3].index and a[2].index~=a[3].index,"unique")
  eq(a[1].reversed,false,"orientation upright")
  eq(a[2].reversed,true,"orientation reversed")
end

do
  local a=Tarot.daily(100)
  local b=Tarot.daily(100)
  eq(a.index,b.index,"daily deterministic card")
  eq(a.reversed,b.reversed,"daily deterministic orientation")
end

do
  local a={{index=4,reversed=false},{index=8,reversed=true},{index=12,reversed=false}}
  local s=Tarot.serialize("three",a,2)
  local d=Tarot.deserialize(s)
  eq(d.kind,"three","roundtrip kind")
  eq(d.selected,2,"roundtrip selected")
  eq(#d.readings,3,"roundtrip count")
  eq(d.readings[2].index,8,"roundtrip index")
  eq(d.readings[2].reversed,true,"roundtrip reversed")
end

do
  local a={index=1,reversed=false}
  eq(Tarot.meaning(a),Tarot.card(1).upright,"upright meaning")
  a.reversed=true
  eq(Tarot.meaning(a),Tarot.card(1).reversed,"reversed meaning")
end

if fail>0 then error(tostring(fail).." failures",0) end
print("test_game.lua OK")
