-- com.m4.tarot - Major Arcana tarot for 480x800 e-ink.
-- Entertainment/reflection tool only. Redraw only when visible state changes.
sys.load("game.lua")

local CARD_W, CARD_H = 112, 192
local CARD_X = 184
local screen = "home"
local readings = {}
local selected = 1
local deck_page = 1
frame_changed = true
local painted = ""

local BTN = {
  daily = {22, 500, 436, 64},
  one = {22, 580, 436, 64},
  three = {22, 660, 436, 64},
  deck = {22, 732, 210, 54},
  last = {248, 732, 210, 54},
}

local function now()
  if type(sys) == "table" and type(sys.time) == "function" then return sys.time() end
  if type(sys) == "table" and type(sys.millis) == "function" then return math.floor(sys.millis()/1000) end
  return 0
end

local function seed_rng()
  local t = now()
  if t <= 0 and type(sys) == "table" and type(sys.millis) == "function" then t = sys.millis() end
  math.randomseed((t % 2147483646) + 1)
  math.random(); math.random()
end

local function rng()
  return math.random()
end

local function persist(kind)
  if type(fs) ~= "table" or type(fs.writeFile) ~= "function" then return end
  pcall(function() fs.writeFile("last.csv", Tarot.serialize(kind or screen, readings, selected)) end)
end

local function restore()
  if type(fs) ~= "table" or type(fs.readFile) ~= "function" then return end
  local ok, data = pcall(function() return fs.readFile("last.csv") end)
  if not ok or type(data) ~= "string" then return end
  local s = Tarot.deserialize(data)
  if not s then return end
  readings = s.readings
  selected = math.max(1, math.min(s.selected or 1, #readings))
end

local function hit(b, x, y)
  return x >= b[1] and x < b[1]+b[3] and y >= b[2] and y < b[2]+b[4]
end

local function text_w(s, size)
  if type(gui.textWidth) == "function" then return gui.textWidth(size, s) end
  return #s * size
end

local function center(s, cx, y, size)
  gui.drawText(size, cx - math.floor(text_w(s,size)/2), y, s)
end

local function button(b, label)
  gui.drawRect(b[1],b[2],b[3],b[4])
  local h = type(gui.lineHeight) == "function" and gui.lineHeight(16) or 18
  center(label, b[1]+math.floor(b[3]/2), b[2]+math.floor((b[4]-h)/2), 16)
end

local function card_file(index)
  local c = Tarot.card(index)
  if not c then return nil end
  return string.format("art/%02d_%s.bmp", c.id, c.key)
end

local function draw_card(reading, x, y)
  if not reading then return end
  local rel = card_file(reading.index)
  if rel and type(gui.drawBmp) == "function" and gui.drawBmp(rel, x, y) then return end
  gui.drawRect(x,y,CARD_W,CARD_H)
  local c = Tarot.card(reading.index)
  if c then center(tostring(c.id), x+math.floor(CARD_W/2), y+math.floor(CARD_H/2)-8, 16) end
end

local function draw_back(x,y)
  if type(gui.drawBmp) == "function" and gui.drawBmp("art/card_back.bmp",x,y) then return end
  gui.drawRect(x,y,CARD_W,CARD_H)
  for d=10,CARD_W+CARD_H,14 do
    local x1=x+math.min(CARD_W-8,d)
    local y1=y+8+math.max(0,d-CARD_W)
    local x2=x+8+math.max(0,d-CARD_H)
    local y2=y+math.min(CARD_H-8,d)
    if x1>=x+8 and y2>=y+8 then gui.drawLine(x1,y1,x2,y2) end
  end
end

local function orientation(r)
  return r and r.reversed and "逆 位" or "正 位"
end

local function home_screen()
  gui.drawText(16,18,16,"塔 罗 牌")
  gui.drawText(10,328,22,"大 阿 尔 卡 那")
  gui.drawLine(16,52,464,52)
  draw_back(CARD_X,120)
  center("把 问 题 留 在 心 里",240,370,16)
  center("抽 牌 只 作 为 自 我 反 思 与 娱 乐",240,408,10)
  button(BTN.daily,"今 日 一 牌")
  button(BTN.one,"随 机 一 牌")
  button(BTN.three,"三 牌 阵")
  button(BTN.deck,"牌 库")
  button(BTN.last,"上 次 结 果")
end

local function single_screen()
  local r = readings[1]
  local c = r and Tarot.card(r.index)
  gui.drawText(16,18,16,"一 张 牌")
  gui.drawLine(16,52,464,52)
  draw_card(r,CARD_X,72)
  if c then
    center(string.format("%02d  %s",c.id,c.name),240,282,16)
    center(orientation(r),240,312,12)
    gui.drawText(12,28,348,"关 键 词")
    gui.drawLine(28,372,452,372)
    center(Tarot.meaning(r),240,390,12)
  end
  button({22,600,210,62},"再 抽 一 张")
  button({248,600,210,62},"返 回")
  gui.drawText(10,30,688,"提 示：先 看 画 面，再 看 关 键 词。")
  gui.drawText(10,30,716,"把 它 当 作 一 个 新 的 观 察 角 度。")
end

local POS = {"过 去","现 在","未 来"}
local function three_screen()
  gui.drawText(16,18,16,"三 牌 阵")
  gui.drawLine(16,52,464,52)
  local xs={16,184,352}
  for i=1,3 do
    local r=readings[i]
    draw_card(r,xs[i],86)
    center(POS[i],xs[i]+math.floor(CARD_W/2),286,12)
    center(orientation(r),xs[i]+math.floor(CARD_W/2),314,10)
  end
  gui.drawLine(16,350,464,350)
  center("轻 触 任 意 一 张 牌 查 看 详 情",240,376,12)
  button({22,620,210,62},"重 新 抽 牌")
  button({248,620,210,62},"返 回")
end

local function detail_screen()
  local r = readings[selected]
  local c = r and Tarot.card(r.index)
  gui.drawText(16,18,16,POS[selected] or "牌 意")
  gui.drawLine(16,52,464,52)
  draw_card(r,CARD_X,72)
  if c then
    center(string.format("%02d  %s",c.id,c.name),240,282,16)
    center(orientation(r),240,312,12)
    center(Tarot.meaning(r),240,366,12)
  end
  button({22,606,210,62},"上 一 张")
  button({248,606,210,62},"下 一 张")
  button({22,690,436,62},"返 回 牌 阵")
end

local function deck_screen()
  gui.drawText(16,18,16,"牌 库")
  gui.drawText(10,390,22,tostring(deck_page).."/2")
  gui.drawLine(16,52,464,52)
  local first=(deck_page-1)*11+1
  for row=0,10 do
    local idx=first+row
    local c=Tarot.card(idx)
    if c then
      local y=72+row*49
      gui.drawText(12,28,y,string.format("%02d",c.id))
      gui.drawText(12,78,y,c.name)
      gui.drawText(10,220,y,c.upright)
    end
  end
  button({22,650,136,58},"上 一 页")
  button({172,650,136,58},"下 一 页")
  button({322,650,136,58},"返 回")
end

local function signature()
  return screen.."|"..Tarot.serialize(screen,readings,selected).."|"..tostring(deck_page)
end

function init()
  seed_rng()
  restore()
  screen="home"
  frame_changed=true
  painted=""
end

local function draw_single()
  readings=Tarot.draw(1,rng)
  selected=1
  screen="single"
  persist("single")
  frame_changed=true
end

local function draw_daily()
  local day=math.floor(now()/86400)
  readings={Tarot.daily(day)}
  selected=1
  screen="single"
  persist("daily")
  frame_changed=true
end

local function draw_three()
  readings=Tarot.draw(3,rng)
  selected=1
  screen="three"
  persist("three")
  frame_changed=true
end

function draw()
  local sig=signature()
  if sig==painted then frame_changed=false; return end
  gui.clear()
  if screen=="home" then home_screen()
  elseif screen=="single" then single_screen()
  elseif screen=="three" then three_screen()
  elseif screen=="detail" then detail_screen()
  elseif screen=="deck" then deck_screen()
  else screen="home"; home_screen() end
  painted=sig
end

function onKey(key)
  if screen=="home" then
    if key=="confirm" then draw_single()
    elseif key=="up" then draw_daily()
    elseif key=="down" then draw_three()
    elseif key=="right" then screen="deck"; frame_changed=true end
    return
  end
  if key=="left" then
    if screen=="detail" then selected=((selected+1)%3)+1 else screen="home" end
    frame_changed=true
  elseif key=="right" then
    if screen=="detail" then selected=(selected%3)+1
    elseif screen=="three" then selected=(selected%3)+1; screen="detail" end
    frame_changed=true
  elseif key=="confirm" then
    if screen=="single" then draw_single()
    elseif screen=="three" then screen="detail"; frame_changed=true
    elseif screen=="detail" then screen="three"; frame_changed=true
    elseif screen=="deck" then screen="home"; frame_changed=true end
  end
end

function onTouch(x,y,phase)
  if phase and phase~="tap" and phase~="up" and phase~="press" then return end
  x=x or 0; y=y or 0
  if screen=="home" then
    if hit(BTN.daily,x,y) then draw_daily()
    elseif hit(BTN.one,x,y) then draw_single()
    elseif hit(BTN.three,x,y) then draw_three()
    elseif hit(BTN.deck,x,y) then screen="deck"; frame_changed=true
    elseif hit(BTN.last,x,y) and #readings>0 then screen=(#readings>=3) and "three" or "single"; frame_changed=true end
  elseif screen=="single" then
    if hit({22,600,210,62},x,y) then draw_single()
    elseif hit({248,600,210,62},x,y) then screen="home"; frame_changed=true end
  elseif screen=="three" then
    if y>=86 and y<342 then
      if x<156 then selected=1 elseif x<324 then selected=2 else selected=3 end
      screen="detail"; frame_changed=true
    elseif hit({22,620,210,62},x,y) then draw_three()
    elseif hit({248,620,210,62},x,y) then screen="home"; frame_changed=true end
  elseif screen=="detail" then
    if hit({22,606,210,62},x,y) then selected=((selected+1)%3)+1; frame_changed=true
    elseif hit({248,606,210,62},x,y) then selected=(selected%3)+1; frame_changed=true
    elseif hit({22,690,436,62},x,y) then screen="three"; frame_changed=true end
  elseif screen=="deck" then
    if hit({22,650,136,58},x,y) then deck_page=math.max(1,deck_page-1); frame_changed=true
    elseif hit({172,650,136,58},x,y) then deck_page=math.min(2,deck_page+1); frame_changed=true
    elseif hit({322,650,136,58},x,y) then screen="home"; frame_changed=true end
  end
end
