-- Host-only interaction and 480x800 draw-bounds test; not a physical device test.
local root=(arg and arg[0] and arg[0]:match("(.*/)")) or "./"
local mem={ ["progress.csv"]="LEGACY_V1_KEEP" }
local labels, rects, lines={},0,0
local function check_rect(x,y,w,h)
  assert(type(x)=="number" and type(y)=="number" and type(w)=="number" and type(h)=="number")
  assert(w>0 and h>0 and x>=0 and y>=0 and x+w<=480 and y+h<=800,
    string.format("offscreen %s,%s,%s,%s",tostring(x),tostring(y),tostring(w),tostring(h)))
  rects=rects+1
end
gui={
  clear=function() labels={};rects=0;lines=0 end,
  drawRect=check_rect, fillRect=check_rect,
  drawLine=function(x1,y1,x2,y2)
    assert(x1>=0 and y1>=0 and x2>=0 and y2>=0 and
      x1<480 and y1<800 and x2<480 and y2<800,
      "offscreen line")
    lines=lines+1
  end,
  drawText=function(font,x,y,text)
    assert(x>=0 and y>=0 and x<480 and y<800,"offscreen text anchor")
    labels[#labels+1]=text
  end,
  lineHeight=function(font) return font+4 end,
  textWidth=function(font,text) return math.floor(#text*4) end
}
fs={
  readFile=function(path) return mem[path] end,
  writeFile=function(path,data) mem[path]=data;return true end
}
sys={load=function(name) dofile(root..name) end,exit=function() end}
local function has(s)
  for _,v in ipairs(labels) do if v:find(s,1,true) then return true end end
  return false
end
dofile(root.."main.lua")
init()
draw()
assert(has("1 / 24"),"initial stage and level header")
assert(has("步数") and has("推箱"),"visible move and push scores")
assert(rects>10 and lines>10,"not a drawn game board")
-- Tap the adjacent crate: it must never auto-push; the physical right key does.
onTouch(240,320,"tap")
draw()
assert(has("1 / 24") and not has("过关！"),"crate tap pushed unexpectedly")
onKey("right")
draw()
assert(has("过关！") and has("下一关"),"win screen lacks Next button")
assert(mem["progress.csv"]=="LEGACY_V1_KEEP","old progress must survive")
assert(type(mem["microban_progress.csv"])=="string","new progress missing")
onTouch(235,690,"tap")
draw()
assert(has("2 / 24"),"next-level touch misses")
-- The board tap on a far, reachable, EMPTY tile should walk a full route in
-- one visible frame and retain step-by-step Undo. Coordinates are for Microban
-- level 2 at 63px/cell (board x51 y99, destination row6 col5).
onTouch(334,445,"tap")
draw()
assert(not has("步数 0"),"far empty-floor tap failed to walk")
onKey("confirm")  -- Undo the final step, not all path steps at once.
draw()
assert(not has("步数 0"),"tap-to-walk undo should preserve earlier steps")
-- A small upper-right '?' makes instructions discoverable.
onTouch(435,60,"tap")
draw()
assert(has("怎么玩") and has("点击空地"),"help lacks legend and tap instructions")
onTouch(160,740,"tap")
draw()
assert(has("2 / 24"),"help return misses")
-- Toolbar now has three actions; pick must expose a 2x4 chapter grid.
onTouch(384,587,"tap")
draw()
assert(has("选择关卡") and has("初识搬运"),"picker header missing")
assert(has("第 1 关") and has("第 8 关") and not has("第 9 关"),
  "chapter 1 is not eight visible cards")
onTouch(234,110,"tap")
draw()
assert(has("绕路布局") and has("第 9 关") and has("第 16 关") and not has("第 17 关"),
  "chapter tab 2 misses or picker overflow")
onTouch(388,112,"tap")
draw()
assert(has("次序机关") and has("第 17 关") and has("第 24 关"),
  "chapter 3 misses")
-- Tapping the upper-left card starts chapter 3's first level.
onTouch(60,185,"tap")
draw()
assert(has("17 / 24"),"chapter card touch misses")
init()
draw()
assert(has("17 / 24"),"v2 save failed to resume chosen level")
-- Offline board-tap BFS must refuse a box square and find an empty floor route.
local a=Game.load(1)
assert(not Game.walk_path(a,2,3),"a crate cannot be a tap-to-walk destination")
local b=Game.load(2)
local possible=0
for r=1,b.rows do for c=1,b.cols do
  local path=Game.walk_path(b,r,c)
  if path and #path>0 then possible=possible+1 end
end end
assert(possible>0,"tap-to-walk cannot find any reachable floor")
assert(not Game.walk_path(b,0,0),"out-of-range tap must be rejected")
print("test_ui.lua OK rects="..rects.." lines="..lines.." reachable="..possible)
