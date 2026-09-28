-- Deterministic host GUI draw recorder. This is NOT a physical e-ink screenshot.
-- Run: sokoban_lua_host plugins/m4-lua-sokoban-plugin/tools/render_ui_trace.lua
local root=(arg and arg[0] and arg[0]:match("(.*/)") or "./")
root=root.."../"
local memory={}
local function log(kind,...)
  local a={...}
  for i,v in ipairs(a) do a[i]=tostring(v):gsub("|","/"):gsub("\n"," ") end
  print(kind.."|"..table.concat(a,"|"))
end
gui={
  clear=function() print("C") end,
  drawRect=function(x,y,w,h) log("R",x,y,w,h) end,
  fillRect=function(x,y,w,h) log("F",x,y,w,h) end,
  drawLine=function(x1,y1,x2,y2) log("L",x1,y1,x2,y2) end,
  drawText=function(font,x,y,s) log("T",font,x,y,s) end,
  lineHeight=function(font) return font+4 end,
  textWidth=function(font,s)
    local width=0
    for char in s:gmatch("[%z\1-\127\194-\244][\128-\191]*") do
      width=width+(#char>1 and font or math.floor(font*0.53))
    end
    return width
  end,
}
fs={
  readFile=function(name) return memory[name] end,
  writeFile=function(name,text) memory[name]=text; return true end,
}
sys={load=function(name) dofile(root..name) end, exit=function() end}
dofile(root.."main.lua")

init()
onTouch(388,587,"tap") -- picker
print("FRAME|picker")
draw()
onTouch(298,194,"tap") -- Microban level 2, with three crates
print("FRAME|game")
draw()
onTouch(435,59,"tap")
print("FRAME|help")
draw()
onTouch(160,744,"tap")
onTouch(388,587,"tap")
onTouch(60,194,"tap") -- first simple tutorial
onKey("right")
print("FRAME|win")
draw()
print("TRACE_END")
