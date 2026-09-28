-- com.m4.sokoban: high-contrast 1-bit e-ink UI for the 480x800 M4.
-- Original M4 layout; interaction inspired by established e-reader puzzle games.
-- Drawing uses ONLY existing M4 gui.clear/drawRect/fillRect/drawLine/drawText API.
sys.load("game.lua")

dirty = false
frame_changed = true

local W, H = 480, 800
local MAX_UNDO = 80
local PICK_PER_PAGE = 8
local mode = "play"                 -- play / pick / help
local st, prog, undo_stack
local CELL, GRID_X, GRID_Y, GRID_W, GRID_H
local BTN = {}
local pick_page, pick_cursor = 1, 1
local last_action = ""              -- one short, persistent status message

local function button(id, x, y, w, h, title)
  BTN[id] = {x=x, y=y, w=w, h=h, title=title}
end

local function layout()
  local rows, cols = st and st.rows or 8, st and st.cols or 8
  CELL = math.min(68, math.floor(444 / rows), math.floor(444 / cols))
  GRID_W, GRID_H = CELL * cols, CELL * rows
  GRID_X = math.floor((W - GRID_W) / 2)
  GRID_Y = 96 + math.floor((448 - GRID_H) / 2)
  -- Single-purpose toolbar. All labels match actions in every state.
  button("undo",   14, 565, 144, 58, "撤销")
  button("reset", 168, 565, 144, 58, "重开")
  button("pick",  322, 565, 144, 58, "选关")
  button("help", 414, 43, 52, 40, "?")
  -- Large physical-D-pad-equivalent touch targets, with arrows AND labels.
  button("up",    198, 633, 84, 65, "上")
  button("left",   97, 707, 84, 69, "左")
  button("down",  198, 707, 84, 69, "下")
  button("right", 299, 707, 84, 69, "右")
  button("nextlevel", 24, 654, 432, 67, "下一关")
  button("winpick",   24, 730, 432, 53, "选择关卡")
  -- Chapter selector: all eight cards visible on one page.
  for i=1,3 do
    button("tab"..i, 9+(i-1)*157, 82, 149, 58,
           (Game.CHAPTERS and Game.CHAPTERS[i]) or ("第"..i.."章"))
  end
  button("prev",  14, 610, 218, 62, "上一章")
  button("nextpage", 248, 610, 218, 62, "下一章")
  button("back",  14, 714, 452, 68, "返回游戏")
end

local function persist()
  if type(fs) ~= "table" or type(fs.writeFile) ~= "function" then return end
  pcall(function()
    fs.writeFile("microban_progress.csv", Game.serialize_progress(prog))
  end)
end

local function fresh_level(index)
  st = Game.load(index)
  if not st then return false end
  undo_stack = {}
  prog.level = index
  last_action = ""
  layout()
  return true
end

local function push_undo()
  undo_stack[#undo_stack+1] = Game.snapshot(st)
  if #undo_stack > MAX_UNDO then table.remove(undo_stack, 1) end
end

local function restore_progress()
  prog = Game.blank_progress()
  if type(fs) ~= "table" or type(fs.readFile) ~= "function" then return end
  local ok, data = pcall(function() return fs.readFile("microban_progress.csv") end)
  if ok and type(data) == "string" and #data > 0 and #data < 8192 then
    prog = Game.deserialize_progress(data)
  end
end

function init()
  restore_progress()
  if not fresh_level(prog.level) then fresh_level(1) end
  mode = "play"
  frame_changed = true
end

local function note_change(kind)
  if kind == "push" then last_action = "已推动箱子"
  else last_action = "" end
  if Game.won(st) then
    Game.note_clear(prog, st.index, st.moves, st.pushes)
    persist()
    last_action = "过关！"
  elseif Game.deadlocked(st) then
    last_action = "箱子卡在墙角，请撤销"
  end
  frame_changed = true
end

local function step(dr, dc)
  if mode ~= "play" or Game.won(st) then return end
  push_undo()
  local kind = Game.step(st, dr, dc)
  if not kind then
    undo_stack[#undo_stack] = nil
    return
  end
  note_change(kind)
end

local function undo()
  if mode ~= "play" or not undo_stack or #undo_stack == 0 then return end
  local snap = undo_stack[#undo_stack]
  undo_stack[#undo_stack] = nil
  Game.restore(st, snap)
  last_action = ""
  frame_changed = true
end

local function restart()
  if fresh_level(prog.level) then
    persist()
    frame_changed = true
  end
end

local function advance_level()
  if st.index < #Game.LEVELS then
    fresh_level(st.index + 1)
    persist()
  else
    pick_page, pick_cursor = 3, #Game.LEVELS
    mode = "pick"
  end
  frame_changed = true
end

local function hit(b, x, y)
  return b and x >= b.x and y >= b.y and x < b.x+b.w and y < b.y+b.h
end

local function label(font, x, y, text)
  gui.drawText(font, x, y, text)
end

local function centered(font, b, text, dy)
  label(font, b.x + math.max(4, math.floor((b.w - gui.textWidth(font, text))/2)),
        b.y + dy, text)
end

local function arrow(x, y, direction)
  local c = 12
  if direction == "up" then
    gui.drawLine(x, y-c, x-12, y+2)
    gui.drawLine(x, y-c, x+12, y+2)
    gui.drawLine(x, y-c, x, y+11)
  elseif direction == "down" then
    gui.drawLine(x, y+c, x-12, y-2)
    gui.drawLine(x, y+c, x+12, y-2)
    gui.drawLine(x, y+c, x, y-11)
  elseif direction == "left" then
    gui.drawLine(x-c, y, x+2, y-12)
    gui.drawLine(x-c, y, x+2, y+12)
    gui.drawLine(x-c, y, x+11, y)
  else
    gui.drawLine(x+c, y, x-2, y-12)
    gui.drawLine(x+c, y, x-2, y+12)
    gui.drawLine(x+c, y, x-11, y)
  end
end

local function draw_button(b, style)
  gui.drawRect(b.x, b.y, b.w, b.h)
  if style == "selected" or style == "primary" then
    gui.drawRect(b.x+3, b.y+3, b.w-6, b.h-6)
  end
  if style == "direction" then
    local id = (b == BTN.left and "left") or
               (b == BTN.right and "right") or
               (b == BTN.up and "up") or "down"
    arrow(b.x+math.floor(b.w/2), b.y+26, id)
    centered(12, b, b.title, b.h-23)
  else
    centered(16, b, b.title, math.floor((b.h - gui.lineHeight(16))/2))
  end
end

local function board_cell(r,c)
  return GRID_X+(c-1)*CELL, GRID_Y+(r-1)*CELL
end

-- Black/white symbols must differ in silhouette, not just shading or color.
local function draw_wall(x, y, sz)
  gui.fillRect(x+1, y+1, sz-2, sz-2) -- 1px white brick grout
end

local function draw_goal(x, y, sz)
  local m = math.max(5, math.floor(sz/4))
  gui.drawRect(x+m, y+m, sz-2*m, sz-2*m)
  gui.drawRect(x+m+3, y+m+3, sz-2*m-6, sz-2*m-6)
  gui.fillRect(x+math.floor(sz/2)-2, y+math.floor(sz/2)-2, 4, 4)
end

local function draw_crate(x, y, sz, on_goal)
  local pad = math.max(3, math.floor(sz/10))
  local x1,y1,x2,y2 = x+pad,y+pad,x+sz-pad-1,y+sz-pad-1
  gui.drawRect(x1,y1,x2-x1+1,y2-y1+1)
  gui.drawRect(x1+2,y1+2,x2-x1-3,y2-y1-3)
  gui.drawLine(x1+6,y1+6,x2-6,y2-6)
  gui.drawLine(x1+6,y2-6,x2-6,y1+6)
  if on_goal then
    -- Four solid corner nails identify a correctly placed crate.
    gui.fillRect(x1+3,y1+3,4,4)
    gui.fillRect(x2-6,y1+3,4,4)
    gui.fillRect(x1+3,y2-6,4,4)
    gui.fillRect(x2-6,y2-6,4,4)
    gui.fillRect(x+math.floor(sz/2)-5,y+math.floor(sz/2)-5,10,10)
  end
end

local function draw_player(x,y,sz)
  -- Dedicated 9x11 1-bit pawn silhouette. An opaque, human-shaped sprite reads
  -- clearly even at the smallest bundled 37px tile; crate symbols remain outlines.
  local glyph={
    "...###...",
    "..#####..",
    "..#####..",
    "...###...",
    ".#######.",
    "#########",
    ".#######.",
    "..#####..",
    "..##.##..",
    ".###.###.",
    "###...###",
  }
  local px=math.max(2,math.floor(sz/14))
  local sx=x+math.floor((sz-9*px)/2)
  local sy=y+math.floor((sz-11*px)/2)
  for row,line in ipairs(glyph) do
    local begin=nil
    for col=1,10 do
      local filled=col<=9 and line:sub(col,col)=="#"
      if filled and not begin then begin=col end
      if not filled and begin then
        gui.fillRect(sx+(begin-1)*px,sy+(row-1)*px,(col-begin)*px,px)
        begin=nil
      end
    end
  end
end

local function board()
  gui.drawRect(GRID_X-3, GRID_Y-3, GRID_W+6, GRID_H+6)
  for r=1,st.rows do
    for c=1,st.cols do
      local key = Game.key(r,c)
      local x,y=board_cell(r,c)
      if st.walls[key] then
        draw_wall(x,y,CELL)
      else
        if st.goals[key] and not st.crates[key] then draw_goal(x,y,CELL) end
        if st.crates[key] then draw_crate(x,y,CELL,st.goals[key]) end
      end
    end
  end
  local px,py=board_cell(st.pr,st.pc)
  draw_player(px,py,CELL)
end

local function draw_play()
  local chapter = math.floor((st.index-1)/PICK_PER_PAGE)+1
  label(16, 14, 12, "推箱子 · "..(Game.CHAPTERS[chapter] or "挑战"))
  label(16, 354, 12, tostring(st.index).." / "..tostring(#Game.LEVELS))
  gui.drawLine(14,39,466,39)
  local info="步数 "..st.moves.."    推箱 "..st.pushes
  local best=prog.best_m[st.index] or 0
  if best>0 then info=info.."    最佳 "..best end
  label(16, 16, 51, info)
  if last_action ~= "" and not Game.won(st) then
    label(12, 18, 78, last_action)
  end
  board()
  draw_button(BTN.help)
  if Game.won(st) then
    gui.drawLine(14,558,466,558)
    label(16, 20, 579, "过关！  "..st.moves.." 步 / "..st.pushes.." 次推动")
    draw_button(BTN.nextlevel,"primary")
    draw_button(BTN.winpick)
  else
    gui.drawLine(14,556,466,556)
    draw_button(BTN.undo)
    draw_button(BTN.reset)
    draw_button(BTN.pick)
    draw_button(BTN.up,"direction")
    draw_button(BTN.left,"direction")
    draw_button(BTN.down,"direction")
    draw_button(BTN.right,"direction")
  end
end

local function count_cleared()
  local n=0
  for i=1,#Game.LEVELS do if prog.cleared[i]==1 then n=n+1 end end
  return n
end

local function set_page(page)
  if page < 1 or page > 3 then return end
  pick_page=page
  pick_cursor=(page-1)*PICK_PER_PAGE+1
  frame_changed=true
end

local function draw_pick()
  label(20, 16, 13, "选择关卡")
  label(12, 16, 46, "已完成 "..count_cleared().."/"..#Game.LEVELS)
  gui.drawLine(14,70,466,70)
  for i=1,3 do
    draw_button(BTN["tab"..i],i==pick_page and "selected" or nil)
  end
  local first=(pick_page-1)*PICK_PER_PAGE+1
  for offset=0,7 do
    local i=first+offset
    if i<=#Game.LEVELS then
      local col,row=offset%2,math.floor(offset/2)
      local x,y=14+col*236,154+row*106
      local w,h=216,94
      gui.drawRect(x,y,w,h)
      if i==pick_cursor then gui.drawRect(x+3,y+3,w-6,h-6) end
      label(20,x+17,y+13,"第 "..i.." 关")
      if prog.cleared[i]==1 then
        gui.fillRect(x+w-27,y+16,9,9)
        label(12,x+17,y+61,"已通关 · 最佳 "..prog.best_m[i].." 步")
      else
        label(12,x+17,y+61,"点击开始")
      end
    end
  end
  if pick_page>1 then draw_button(BTN.prev) end
  if pick_page<3 then draw_button(BTN.nextpage) end
  draw_button(BTN.back)
end

local function draw_help()
  label(20,16,14,"怎么玩")
  label(12,16,45,"将所有箱子推到目标点。箱子只能推，不能拉。")
  gui.drawLine(16,72,464,72)
  local xlist={32,151,270,389}
  draw_wall(xlist[1],90,48)
  draw_goal(xlist[2],90,48)
  draw_crate(xlist[3],90,48,false)
  draw_player(xlist[4],90,48)
  label(16,xlist[1],150,"墙")
  label(16,xlist[2],150,"目标")
  label(16,xlist[3],150,"箱子")
  label(16,xlist[4],150,"人物")
  gui.drawLine(16,197,464,197)
  label(16,18,217,"方向键或屏幕方向按钮：行走、推箱子")
  label(16,18,272,"点击空地：自动走过去，不会推箱子")
  label(16,18,327,"撤销：退回上一步")
  label(16,18,382,"重开：重新开始本关")
  label(16,18,437,"选关：自由挑战三个章节")
  label(16,18,492,"角落里的箱子可能无法再推动")
  label(12,18,554,"设备返回键可退出游戏；确认键可撤销。")
  draw_button(BTN.back)
end

function draw()
  gui.clear()
  if mode=="pick" then draw_pick()
  elseif mode=="help" then draw_help()
  else draw_play() end
  frame_changed=false
end

local DIR={
  up={-1,0},down={1,0},left={0,-1},right={0,1}
}

function onKey(key)
  if mode=="help" then
    if key=="back" or key=="confirm" then mode="play";frame_changed=true end
    return
  end
  if mode=="pick" then
    local first=(pick_page-1)*PICK_PER_PAGE+1
    local slot=pick_cursor-first
    if key=="back" then mode="play"
    elseif key=="confirm" then fresh_level(pick_cursor);persist();mode="play"
    elseif key=="left" and slot%2==1 then pick_cursor=pick_cursor-1
    elseif key=="left" then set_page(pick_page-1);return
    elseif key=="right" and slot%2==0 then pick_cursor=math.min(first+7,#Game.LEVELS,pick_cursor+1)
    elseif key=="right" then set_page(pick_page+1);return
    elseif key=="up" then pick_cursor=math.max(first,pick_cursor-2)
    elseif key=="down" then pick_cursor=math.min(first+7,#Game.LEVELS,pick_cursor+2)
    end
    frame_changed=true
    return
  end
  if key=="back" then return end -- the host handles system Back
  if key=="confirm" then
    if Game.won(st) then advance_level() else undo() end
    return
  end
  local d=DIR[key]
  if d then step(d[1],d[2]) end
end

local function tap_empty_floor(r,c)
  if Game.won(st) or st.crates[Game.key(r,c)] then return end
  local path=Game.walk_path(st,r,c)
  if not path or #path==0 then return end
  -- Only one frame is drawn after this loop. Every step remains undoable.
  for _,d in ipairs(path) do
    push_undo()
    local kind=Game.step(st,d[1],d[2])
    if kind~="walk" then
      undo_stack[#undo_stack]=nil
      return
    end
  end
  last_action=""
  frame_changed=true
end

function onTouch(x,y,phase)
  if phase and phase~="tap" then return end
  if mode=="help" then
    if hit(BTN.back,x,y) then mode="play";frame_changed=true end
    return
  end
  if mode=="pick" then
    if hit(BTN.back,x,y) then mode="play";frame_changed=true;return end
    if hit(BTN.prev,x,y) and pick_page>1 then set_page(pick_page-1);return end
    if hit(BTN.nextpage,x,y) and pick_page<3 then set_page(pick_page+1);return end
    for i=1,3 do
      if hit(BTN["tab"..i],x,y) then set_page(i);return end
    end
    if y>=154 and y<566 then
      local col= (x>=250 and x<466 and 1) or (x>=14 and x<230 and 0)
      if col~=nil then
        local row=math.floor((y-154)/106)
        local local_y=(y-154)%106
        if local_y<94 then
          local i=(pick_page-1)*PICK_PER_PAGE+row*2+col+1
          if i<=#Game.LEVELS then
            fresh_level(i);persist();mode="play";frame_changed=true
          end
        end
      end
    end
    return
  end
  if hit(BTN.help,x,y) then mode="help";frame_changed=true;return end
  if Game.won(st) then
    if hit(BTN.nextlevel,x,y) then advance_level()
    elseif hit(BTN.winpick,x,y) then
      pick_page=math.floor((st.index-1)/PICK_PER_PAGE)+1
      pick_cursor=st.index
      mode="pick";frame_changed=true
    end
    return
  end
  if hit(BTN.undo,x,y) then undo();return end
  if hit(BTN.reset,x,y) then restart();return end
  if hit(BTN.pick,x,y) then
    pick_page=math.floor((st.index-1)/PICK_PER_PAGE)+1
    pick_cursor=st.index
    mode="pick";frame_changed=true;return
  end
  for name,d in pairs(DIR) do
    if hit(BTN[name],x,y) then step(d[1],d[2]);return end
  end
  if x>=GRID_X and x<GRID_X+GRID_W and
      y>=GRID_Y and y<GRID_Y+GRID_H then
    local c=math.floor((x-GRID_X)/CELL)+1
    local r=math.floor((y-GRID_Y)/CELL)+1
    tap_empty_floor(r,c)
  end
end
