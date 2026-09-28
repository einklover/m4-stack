-- Host-only simulated 480x800 GUI trace for the actual plugin main.lua.
-- The firmware sandbox is not a renderer; this verifies callbacks and bounds.
local root = (arg and arg[0] and arg[0]:match("(.*/)")) or "./"
local mem = { ["progress.csv"] = "LEGACY_SOKOBAN_V1_UNTOUCHED" }
local labels = {}
local count = 0
local function shape(x,y,w,h)
  assert(type(x)=="number" and type(y)=="number" and type(w)=="number" and type(h)=="number")
  assert(x >= 0 and y >= 0 and x+w <= 480 and y+h <= 800,
         string.format("out-of-bounds rect (%d,%d,%d,%d)",x,y,w,h))
  count = count + 1
end
gui = {
  clear = function() labels={}; count=0 end,
  drawRect = shape, fillRect = shape,
  drawLine = function(x1,y1,x2,y2)
    assert(x1>=0 and y1>=0 and x2>=0 and y2>=0 and
           x1<480 and y1<800 and x2<480 and y2<800)
    count = count+1
  end,
  drawText = function(font,x,y,s)
    assert(x>=0 and x<480 and y>=0 and y<800)
    labels[#labels+1] = s
  end,
  lineHeight = function(font) return font + 4 end,
  textWidth = function(font,s) return #s * 4 end,
}
fs = {
  readFile = function(path) return mem[path] end,
  writeFile = function(path,data) mem[path]=data; return true end,
}
sys = {
  load = function(name) dofile(root .. name) end,
  exit = function() end,
}
local function has(s)
  for _,v in ipairs(labels) do if v:find(s,1,true) then return true end end
  return false
end
dofile(root .. "main.lua")
init()
draw()
assert(has("1/24"),"initial game must be level 1 of 24")
assert(count>10,"the game board was not drawn")
-- First level is Skinner Microban #44: exactly one push to the right.
onKey("right")
draw()
assert(has("过关"),"first win must announce success")
assert(has("下一关"),"a visible next-level action is required")
assert(mem["progress.csv"]=="LEGACY_SOKOBAN_V1_UNTOUCHED","old save overwritten")
assert(type(mem["microban_progress.csv"])=="string","new save missing")
onTouch(150,710,"tap")
draw()
assert(has("2/24"),"next-level hitbox failed")
-- Level selector shows only eight levels per page and three chapters.
onTouch(270,711,"tap")
draw()
assert(has("初识搬运"),"chapter 1 missing")
assert(has("1/24") and has("8/24") and not has("9/24"),"chapter 1 pagination wrong")
onTouch(300,588,"tap")
draw()
assert(has("绕路布局") and has("9/24") and has("16/24"),"chapter 2 missing")
assert(not has("17/24"),"chapter 2 not paginated")
onKey("right")
draw()
assert(has("次序机关") and has("17/24") and has("24/24"),"chapter 3 missing")
onKey("confirm")
draw()
assert(has("17/24"),"physical confirm did not select focused page 3 level")
init()
draw()
assert(has("17/24"),"V2 progress did not restore current level")
print("test_ui.lua OK rects="..count.." labels="..#labels)
