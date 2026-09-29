-- 推箱子 / Sokoban. Pure logic, no GUI.
-- XSB: # wall, $ crate, . goal, @ player, * crate-on-goal, + player-on-goal.
-- Curated attributed Microban levels; the offline audit proves each shipped map.

Game = Game or {}

-- Microban by David W. Skinner (revised April 2000).
-- Maps drawn from OMerkel/Sokoban 3rdParty/Levels/Microban.txt.
-- KOReader describes this set as public domain; verify redistribution
-- permission before any public app-store release. M4 Lua code is original.
Game.MICROBAN_IDS = {44, 2, 21, 4, 25, 1, 17, 24, 9, 26, 27, 14, 23, 12, 15, 3, 18, 22, 20, 11, 19, 13, 10, 8}
Game.CHAPTERS = {"初识搬运", "绕路布局", "次序机关"}

Game.LEVELS = {
-- Microban #44
[[
#####
#@$.#
#####
]],
-- Microban #2
[[
######
#    #
# #@ #
# $* #
# .* #
#    #
######
]],
-- Microban #21
[[
#######
#  ####
# . . #
# $$#@#
##    #
#######
]],
-- Microban #4
[[
########
#      #
# .**$@#
#      #
#####  #
########
]],
-- Microban #25
[[
#######
##  ###
## $$ #
##... #
#  @$ #
#   ###
#######
]],
-- Microban #1
[[
######
# .###
#  ###
#*@  #
#  $ #
#  ###
######
]],
-- Microban #17
[[
######
# @ ##
#...##
#$$$##
#    #
#    #
######
]],
-- Microban #24
[[
#######
###   #
###$$@#
#   ###
#     #
# . . #
#######
]],
-- Microban #9
[[
######
#.  ##
#@$$ #
##   #
###  #
####.#
######
]],
-- Microban #26
[[
######
## @ #
##   #
###$ #
# ...#
# $$ #
###  #
######
]],
-- Microban #27
[[
#######
#   .##
# ## ##
#  $$@#
# #   #
#.  ###
#######
]],
-- Microban #14
[[
#######
#     #
# # # #
#. $*@#
#   ###
#######
]],
-- Microban #23
[[
#######
#  *  #
#     #
## # ##
##$@.##
##   ##
#######
]],
-- Microban #12
[[
#########
#   #####
# $  ####
## $ ####
####@.  #
###  .# #
###     #
#########
]],
-- Microban #15
[[
#########
######@##
#    .* #
#   #   #
#####$# #
#####   #
#########
]],
-- Microban #3
[[
#########
###  ####
#     $ #
# #  #$ #
# . .#@ #
#########
]],
-- Microban #18
[[
#######
#     #
#. .  #
# ## ##
#  $ ##
###$ ##
###@ ##
###  ##
#######
]],
-- Microban #22
[[
#######
#   ###
#. .  #
#   # #
## #  #
##@$$ #
##    #
##  ###
#######
]],
-- Microban #20
[[
#########
#     ###
#  @$$..#
#### ## #
###     #
###  ####
###  ####
#########
]],
-- Microban #11
[[
#########
###    ##
### ##@##
### # $ #
# ..# $ #
#       #
#  ######
#########
]],
-- Microban #19
[[
########
#   .. #
#  @$$ #
##### ##
####  ##
####  ##
####  ##
########
]],
-- Microban #13
[[
#######
#. ####
#.@ ###
#. $###
##$ ###
## $  #
##    #
##  ###
#######
]],
-- Microban #10
[[
###########
#######.  #
#######.# #
#######.# #
# @ $ $ $ #
# # # # ###
#       ###
###########
]],
-- Microban #8
[[
########
### ..@#
### $$ #
#### ###
#### ###
#### ###
#### ###
#    ###
# #   ##
#   # ##
###   ##
########
]],
}

local DIRS = {
  u = { -1, 0 }, d = { 1, 0 }, l = { 0, -1 }, r = { 0, 1 },
}

function Game.key(r, c)
  return tostring(r) .. "," .. tostring(c)
end

function Game.parse(text)
  if type(text) ~= "string" then return nil end
  local rows = {}
  local cols = 0
  for line in (text .. "\n"):gmatch("(.-)\n") do
    if line:find("[#$@.*+]") then
      rows[#rows + 1] = line
      if #line > cols then cols = #line end
    end
  end
  if #rows == 0 or cols == 0 then return nil end
  local walls, goals, crates = {}, {}, {}
  local pr, pc, nplayer, ncrate = nil, nil, 0, 0
  for r = 1, #rows do
    local line = rows[r]
    for c = 1, cols do
      local ch = line:sub(c, c)
      if ch == "" then ch = " " end
      local k = Game.key(r, c)
      if ch == "#" then
        walls[k] = true
      elseif ch == "$" or ch == "*" then
        crates[k] = true
        ncrate = ncrate + 1
        if ch == "*" then goals[k] = true end
      elseif ch == "." then
        goals[k] = true
      elseif ch == "@" or ch == "+" then
        pr, pc = r, c
        nplayer = nplayer + 1
        if ch == "+" then goals[k] = true end
      elseif ch ~= " " then
        return nil
      end
    end
  end
  if nplayer ~= 1 or ncrate < 1 then return nil end
  return {
    rows = #rows,
    cols = cols,
    walls = walls,
    goals = goals,
    crates = crates,
    pr = pr,
    pc = pc,
    moves = 0,
    pushes = 0,
  }
end

function Game.blocked(st, r, c)
  if r < 1 or c < 1 or r > st.rows or c > st.cols then return true end
  return st.walls[Game.key(r, c)] == true
end

function Game.dead_squares(st)
  local dead = {}
  for r = 1, st.rows do
    for c = 1, st.cols do
      local k = Game.key(r, c)
      if not st.walls[k] and not st.goals[k] then
        local up = Game.blocked(st, r - 1, c)
        local down = Game.blocked(st, r + 1, c)
        local left = Game.blocked(st, r, c - 1)
        local right = Game.blocked(st, r, c + 1)
        if (up and left) or (up and right) or (down and left) or (down and right) then
          dead[k] = true
        end
      end
    end
  end
  return dead
end

function Game.attach_dead(st)
  st.dead = Game.dead_squares(st)
  return st
end

function Game.load(index)
  local text = Game.LEVELS[index]
  if not text then return nil end
  local st = Game.parse(text)
  if not st then return nil end
  st.index = index
  return Game.attach_dead(st)
end

function Game.snapshot(st)
  local crates = {}
  for k, v in pairs(st.crates) do
    if v then crates[k] = true end
  end
  return {
    pr = st.pr, pc = st.pc,
    moves = st.moves, pushes = st.pushes,
    crates = crates,
  }
end

function Game.restore(st, snap)
  st.pr, st.pc = snap.pr, snap.pc
  st.moves, st.pushes = snap.moves, snap.pushes
  st.crates = {}
  for k, v in pairs(snap.crates) do
    if v then st.crates[k] = true end
  end
end

-- Returns "walk", "push", or nil. One crate only; no chain pushes.
function Game.step(st, dr, dc)
  if (dr ~= 0 and dc ~= 0) or (dr == 0 and dc == 0) then return nil end
  if math.abs(dr) + math.abs(dc) ~= 1 then return nil end
  local nr, nc = st.pr + dr, st.pc + dc
  if Game.blocked(st, nr, nc) then return nil end
  local nk = Game.key(nr, nc)
  if st.crates[nk] then
    local br, bc = nr + dr, nc + dc
    if Game.blocked(st, br, bc) then return nil end
    local bk = Game.key(br, bc)
    if st.crates[bk] then return nil end
    st.crates[nk] = nil
    st.crates[bk] = true
    st.pr, st.pc = nr, nc
    st.moves = st.moves + 1
    st.pushes = st.pushes + 1
    return "push"
  end
  st.pr, st.pc = nr, nc
  st.moves = st.moves + 1
  return "walk"
end

-- Deterministic bounded path to an EMPTY floor tile. Never moves a crate.
-- Called only on tap: search size is bounded by the current tiny board.
function Game.walk_path(st, target_r, target_c)
  if Game.blocked(st, target_r, target_c) or
      st.crates[Game.key(target_r, target_c)] then return nil end
  if target_r == st.pr and target_c == st.pc then return {} end
  local cap = st.rows * st.cols
  local q, head = { { st.pr, st.pc } }, 1
  local start = Game.key(st.pr, st.pc)
  local prev = { [start] = true }
  local goal = Game.key(target_r, target_c)
  local moves = { {-1,0}, {1,0}, {0,-1}, {0,1} }
  while head <= #q and head <= cap do
    local at = q[head]
    head = head + 1
    for _,d in ipairs(moves) do
      local nr,nc = at[1]+d[1],at[2]+d[2]
      local nk = Game.key(nr,nc)
      if not prev[nk] and not Game.blocked(st,nr,nc) and not st.crates[nk] then
        prev[nk] = { Game.key(at[1],at[2]),d[1],d[2] }
        if nk == goal then
          local path = {}
          local k = goal
          while k ~= start do
            local link = prev[k]
            table.insert(path,1,{ link[2],link[3] })
            k = link[1]
          end
          return path
        end
        q[#q+1] = {nr,nc}
      end
    end
  end
  return nil
end

function Game.won(st)
  local n = 0
  for k, v in pairs(st.crates) do
    if v then
      n = n + 1
      if not st.goals[k] then return false end
    end
  end
  return n > 0
end

function Game.deadlocked(st)
  if not st.dead then return false end
  for k, v in pairs(st.crates) do
    if v and st.dead[k] then return true end
  end
  return false
end

function Game.play(st, moves)
  if type(moves) ~= "string" then return false end
  for i = 1, #moves do
    local d = DIRS[moves:sub(i, i)]
    if not d or not Game.step(st, d[1], d[2]) then return false end
  end
  return true
end

function Game.crate_list(st)
  local t = {}
  for k, v in pairs(st.crates) do
    if v then t[#t + 1] = k end
  end
  table.sort(t)
  return t
end

-- Progress: level index, per-level best moves, best pushes, cleared flags.
function Game.blank_progress()
  local n = #Game.LEVELS
  local best_m, best_p, cleared = {}, {}, {}
  for i = 1, n do
    best_m[i] = 0
    best_p[i] = 0
    cleared[i] = 0
  end
  return { level = 1, best_m = best_m, best_p = best_p, cleared = cleared }
end

local function csv_nums(list)
  local t = {}
  for i = 1, #list do t[i] = tostring(list[i]) end
  return table.concat(t, ",")
end

local function parse_csv(s, n)
  local out = {}
  local i = 1
  for tok in (s .. ","):gmatch("(.-),") do
    out[i] = tonumber(tok) or 0
    i = i + 1
    if i > n then break end
  end
  while #out < n do out[#out + 1] = 0 end
  return out
end

function Game.serialize_progress(p)
  return table.concat({
    "v2",
    tostring(p.level),
    csv_nums(p.best_m),
    csv_nums(p.best_p),
    csv_nums(p.cleared),
  }, "|")
end

function Game.deserialize_progress(s)
  local p = Game.blank_progress()
  if type(s) ~= "string" or s:sub(1, 3) ~= "v2|" then return p end
  local parts = {}
  for tok in (s .. "|"):gmatch("(.-)|") do parts[#parts + 1] = tok end
  if #parts < 5 then return p end
  local n = #Game.LEVELS
  local level = tonumber(parts[2]) or 1
  if level < 1 or level > n then level = 1 end
  p.level = level
  p.best_m = parse_csv(parts[3], n)
  p.best_p = parse_csv(parts[4], n)
  p.cleared = parse_csv(parts[5], n)
  for i = 1, n do
    if p.cleared[i] ~= 0 then p.cleared[i] = 1 end
  end
  return p
end

function Game.note_clear(p, level, moves, pushes)
  if level < 1 or level > #Game.LEVELS then return end
  p.cleared[level] = 1
  local bm, bp = p.best_m[level], p.best_p[level]
  if bm == 0 or moves < bm or (moves == bm and pushes < bp) then
    p.best_m[level] = moves
    p.best_p[level] = pushes
  end
end
