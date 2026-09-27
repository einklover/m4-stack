-- Tower of Hanoi rules from ReKindle hanoi.html (not a source copy).
-- 3 pegs. Disks 1..n (1 smallest). Start on peg 1; win when all on peg 3.
-- Only move a smaller disk onto a larger one (or empty peg).

Game = Game or {}

function Game.min_moves(n)
  return (2 ^ n) - 1
end

function Game.new_stacks(n)
  local s = { {}, {}, {} }
  for i = n, 1, -1 do
    s[1][#s[1] + 1] = i
  end
  return s
end

function Game.top(stack)
  if #stack == 0 then return nil end
  return stack[#stack]
end

function Game.can_move(stacks, from, to)
  if from == to then return false end
  local disk = Game.top(stacks[from])
  if not disk then return false end
  local dest = Game.top(stacks[to])
  if dest and disk >= dest then return false end
  return true
end

function Game.move(stacks, from, to)
  if not Game.can_move(stacks, from, to) then return false end
  local disk = stacks[from][#stacks[from]]
  stacks[from][#stacks[from]] = nil
  stacks[to][#stacks[to] + 1] = disk
  return true
end

function Game.is_win(stacks, n)
  return #stacks[3] == n
end

function Game.serialize(st)
  local lines = {
    tostring(st.n),
    tostring(st.moves),
    tostring(st.sel or 0),
    tostring(st.cursor or 1),
    st.won and "1" or "0",
  }
  for p = 1, 3 do
    local t = {}
    for i = 1, #st.stacks[p] do
      t[i] = tostring(st.stacks[p][i])
    end
    lines[#lines + 1] = table.concat(t, ",")
  end
  return table.concat(lines, "\n")
end

function Game.deserialize(s)
  if type(s) ~= "string" then return nil end
  local a, b, c, d, e, p1, p2, p3 = s:match(
    "^(%d+)\n(%d+)\n(%-?%d+)\n(%d+)\n([01])\n([^\n]*)\n([^\n]*)\n([^\n]*)")
  if not a then return nil end
  local function pack(str)
    local t = {}
    if str == "" then return t end
    for num in str:gmatch("[^,]+") do
      t[#t + 1] = tonumber(num)
    end
    return t
  end
  local n = tonumber(a)
  local stacks = { pack(p1), pack(p2), pack(p3) }
  local count = #stacks[1] + #stacks[2] + #stacks[3]
  if count ~= n then return nil end
  return {
    n = n,
    moves = tonumber(b),
    sel = tonumber(c),
    cursor = tonumber(d),
    won = e == "1",
    stacks = stacks,
  }
end
