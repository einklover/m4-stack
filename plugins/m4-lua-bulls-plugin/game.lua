-- 几A几B / Bulls and Cows. Pure rules. No leading zero. Unique digits.
-- Original Lua. Score matches the usual A/B definition (see README).

Game = Game or {}
Game.DEFAULT_LEN = 4
Game.HIST_MAX = 12
Game.ALLOW_LEAD = false

-- 31-bit LCG (glibc-style constants). Returns next state in [0, 2^31).
function Game.lcg(state)
  state = math.floor(tonumber(state) or 0)
  state = state % 2147483648
  if state < 0 then state = state + 2147483648 end
  -- Floor so a float-only Lua still stores an integer seed.
  local n = math.floor((state * 1103515245 + 12345) % 2147483648)
  return n
end

function Game.valid_len(n)
  return n == 3 or n == 4 or n == 5
end

function Game.digits_ok(s, len)
  if type(s) ~= "string" or #s ~= len then return false end
  local seen = {}
  for i = 1, #s do
    local c = s:sub(i, i)
    local b = string.byte(c)
    if not b or b < 48 or b > 57 then return false end
    if seen[c] then return false end
    seen[c] = true
  end
  if not Game.ALLOW_LEAD and s:sub(1, 1) == "0" then return false end
  return true
end

-- A = right digit and place. B = right digit, wrong place.
function Game.score(secret, guess)
  if type(secret) ~= "string" or type(guess) ~= "string" then return nil end
  if #secret ~= #guess or #secret < 1 then return nil end
  if not Game.digits_ok(secret, #secret) or not Game.digits_ok(guess, #guess) then
    return nil
  end
  local pos = {}
  for i = 1, #secret do pos[secret:sub(i, i)] = i end
  local a, b = 0, 0
  for i = 1, #guess do
    local g = guess:sub(i, i)
    if secret:sub(i, i) == g then
      a = a + 1
    elseif pos[g] then
      b = b + 1
    end
  end
  return a, b
end

function Game.mark(a, b)
  return tostring(a) .. "A" .. tostring(b) .. "B"
end

-- Fisher-Yates over "0".."9", then take `len` digits with no leading zero.
-- Returns secret string and the advanced RNG state.
function Game.make_secret(len, seed)
  if not Game.valid_len(len) then return nil, seed end
  local state = tonumber(seed) or 1
  state = state % 2147483648
  if state <= 0 then state = 1 end
  local d = {}
  for i = 0, 9 do d[i + 1] = tostring(i) end
  for i = 10, 2, -1 do
    state = Game.lcg(state)
    local j = (state % i) + 1
    d[i], d[j] = d[j], d[i]
  end
  local out = {}
  local n = 0
  -- first pass: skip a leading zero by rotating it to the end of the pick
  local picked = {}
  for i = 1, 10 do picked[i] = d[i] end
  if picked[1] == "0" then
    for i = 1, 9 do picked[i] = picked[i + 1] end
    picked[10] = "0"
  end
  for i = 1, len do
    n = n + 1
    out[n] = picked[i]
  end
  return table.concat(out), state
end

function Game.new_state(len, seed)
  len = Game.valid_len(len) and len or Game.DEFAULT_LEN
  seed = tonumber(seed) or 1
  local secret, rng = Game.make_secret(len, seed)
  return {
    len = len,
    secret = secret,
    entry = "",
    history = {},
    won = false,
    guesses = 0,
    wins = 0,
    giveups = 0,
    best = 0,
    rng = rng,
    hint = "",
    focus_r = 1,
    focus_c = 1,
    help = false,
  }
end

-- One digit that is absent from the secret and not yet hinted. Never the secret.
function Game.hint_digit(st)
  if type(st) ~= "table" or type(st.secret) ~= "string" then return nil end
  local in_sec = {}
  for i = 1, #st.secret do in_sec[st.secret:sub(i, i)] = true end
  local used = {}
  if type(st.hint) == "string" then
    for i = 1, #st.hint do used[st.hint:sub(i, i)] = true end
  end
  for d = 0, 9 do
    local c = tostring(d)
    if not in_sec[c] and not used[c] then return c end
  end
  return nil
end

function Game.push_hist(st, guess, a, b)
  local h = st.history
  h[#h + 1] = { g = guess, a = a, b = b }
  while #h > Game.HIST_MAX do
    table.remove(h, 1)
  end
end

-- Returns "ok", "win", "dup", "bad", "done".
function Game.submit(st)
  if type(st) ~= "table" then return "bad" end
  if st.won then return "done" end
  local g = st.entry or ""
  if not Game.digits_ok(g, st.len) then return "bad" end
  for i = 1, #st.history do
    if st.history[i].g == g then return "dup" end
  end
  local a, b = Game.score(st.secret, g)
  if not a then return "bad" end
  Game.push_hist(st, g, a, b)
  st.guesses = (st.guesses or 0) + 1
  st.entry = ""
  if a == st.len then
    st.won = true
    st.wins = (st.wins or 0) + 1
    local best = st.best or 0
    if best == 0 or st.guesses < best then st.best = st.guesses end
    return "win"
  end
  return "ok"
end

function Game.push_digit(st, ch)
  if st.won then return false end
  if type(ch) ~= "string" or #ch ~= 1 then return false end
  local b = string.byte(ch)
  if not b or b < 48 or b > 57 then return false end
  local e = st.entry or ""
  if #e >= st.len then return false end
  if #e == 0 and ch == "0" then return false end
  if e:find(ch, 1, true) then return false end
  st.entry = e .. ch
  return true
end

function Game.backspace(st)
  if st.won then return false end
  local e = st.entry or ""
  if #e == 0 then return false end
  st.entry = e:sub(1, #e - 1)
  return true
end

function Game.clear_entry(st)
  if st.won then return false end
  if (st.entry or "") == "" then return false end
  st.entry = ""
  return true
end

function Game.restart(st, len)
  if type(st) ~= "table" then return nil end
  if not st.won and (st.guesses or 0) > 0 then
    st.giveups = (st.giveups or 0) + 1
  end
  len = Game.valid_len(len) and len or st.len or Game.DEFAULT_LEN
  local secret, rng = Game.make_secret(len, (st.rng or 1) + 1)
  st.len = len
  st.secret = secret
  st.rng = rng
  st.entry = ""
  st.history = {}
  st.won = false
  st.guesses = 0
  st.hint = ""
  st.help = false
  return st
end

function Game.apply_hint(st)
  local c = Game.hint_digit(st)
  if not c then return false end
  st.hint = (st.hint or "") .. c
  return true
end

-- Keypad geometry shared with the UI. Rows are 1-based.
Game.PAD = {
  { "1", "2", "3" },
  { "4", "5", "6" },
  { "7", "8", "9" },
  { "C", "0", "del" },
  { "3", "4", "5" }, -- length buttons; distinguished by row
  { "hint", "ok", "new" },
  { "help" },
}

function Game.pad_cols(r)
  local row = Game.PAD[r]
  return row and #row or 0
end

function Game.move_focus(st, key)
  local r = st.focus_r or 1
  local c = st.focus_c or 1
  if key == "left" then c = c - 1
  elseif key == "right" then c = c + 1
  elseif key == "up" then r = r - 1
  elseif key == "down" then r = r + 1
  else return false end
  if r < 1 then r = #Game.PAD end
  if r > #Game.PAD then r = 1 end
  local cols = Game.pad_cols(r)
  if c < 1 then c = cols end
  if c > cols then c = 1 end
  if r == st.focus_r and c == st.focus_c then return false end
  st.focus_r, st.focus_c = r, c
  return true
end

-- Activate the focused control. Returns status string.
function Game.activate(st)
  local r = st.focus_r or 1
  local c = st.focus_c or 1
  local row = Game.PAD[r]
  if not row then return "bad" end
  local k = row[c]
  if not k then return "bad" end
  if r <= 3 or (r == 4 and k == "0") then
    return Game.push_digit(st, k) and "digit" or "bad"
  end
  if r == 4 and k == "C" then
    return Game.clear_entry(st) and "clear" or "bad"
  end
  if r == 4 and k == "del" then
    return Game.backspace(st) and "del" or "bad"
  end
  if r == 5 then
    local n = tonumber(k)
    if n == st.len and not st.won and (st.guesses or 0) == 0 and (st.entry or "") == "" then
      return "bad"
    end
    Game.restart(st, n)
    return "new"
  end
  if k == "hint" then
    return Game.apply_hint(st) and "hint" or "bad"
  end
  if k == "ok" then
    return Game.submit(st)
  end
  if k == "new" then
    Game.restart(st, st.len)
    return "new"
  end
  if k == "help" then
    st.help = not st.help
    return "help"
  end
  return "bad"
end

-- Direction keys move focus. confirm activates. Anything else is ignored.
function Game.on_key(st, key)
  if type(st) ~= "table" or type(key) ~= "string" then return false, "bad" end
  if st.help then
    if key == "confirm" or key == "left" or key == "right" or key == "up" or key == "down" then
      st.help = false
      return true, "help"
    end
    return false, "bad"
  end
  if key == "confirm" then
    if st.won then
      Game.restart(st, st.len)
      return true, "new"
    end
    local status = Game.activate(st)
    return status ~= "bad", status
  end
  if key == "left" or key == "right" or key == "up" or key == "down" then
    return Game.move_focus(st, key), "move"
  end
  return false, "bad"
end

local function num_field(s)
  if type(s) ~= "string" then return nil end
  local body = s:match("^(%d+)%.0+$") or s:match("^(%d+)$")
  if not body or #body > 9 then return nil end
  return tonumber(body)
end

local function fmt_int(n)
  n = math.floor(tonumber(n) or 0)
  if n < 0 then n = 0 end
  return string.format("%.0f", n)
end

function Game.serialize(st)
  if type(st) ~= "table" or type(st.secret) ~= "string" then return "" end
  local hist = {}
  local n = #st.history
  local from = 1
  if n > Game.HIST_MAX then from = n - Game.HIST_MAX + 1 end
  for i = from, n do
    local h = st.history[i]
    hist[#hist + 1] = table.concat({ h.g, tostring(h.a), tostring(h.b) }, ",")
  end
  local hint = st.hint
  if type(hint) ~= "string" or hint == "" then hint = "-" end
  local parts = {
    "1",
    fmt_int(st.len or 4),
    st.secret,
    st.entry or "",
    st.won and "1" or "0",
    fmt_int(st.guesses or 0),
    fmt_int(st.wins or 0),
    fmt_int(st.giveups or 0),
    fmt_int(st.best or 0),
    fmt_int(st.rng or 1),
    hint,
    fmt_int(st.focus_r or 1),
    fmt_int(st.focus_c or 1),
    table.concat(hist, ";"),
  }
  return table.concat(parts, "|")
end

function Game.deserialize(s)
  if type(s) ~= "string" or #s == 0 or #s > 800 then return nil end
  local parts = {}
  local rest = s
  -- split on | but the last field may contain ';'
  for _ = 1, 13 do
    local a, b = rest:match("^([^|]*)|(.*)$")
    if not a then return nil end
    parts[#parts + 1] = a
    rest = b
  end
  parts[#parts + 1] = rest
  if parts[1] ~= "1" then return nil end
  local len = num_field(parts[2])
  if not Game.valid_len(len) then return nil end
  local secret = parts[3]
  if not Game.digits_ok(secret, len) then return nil end
  local entry = parts[4]
  if entry ~= "" and not Game.digits_ok(entry, #entry) then return nil end
  if #entry > len then return nil end
  local won = parts[5]
  if won ~= "0" and won ~= "1" then return nil end
  local guesses = num_field(parts[6])
  local wins = num_field(parts[7])
  local giveups = num_field(parts[8])
  local best = num_field(parts[9])
  local rng = num_field(parts[10])
  if not guesses or not wins or not giveups or not best or not rng then return nil end
  if guesses > 999 or wins > 999999 or giveups > 999999 or best > 999 then return nil end
  local hint = parts[11]
  if hint == "-" then hint = "" end
  if #hint > 10 then return nil end
  local seen_h = {}
  for i = 1, #hint do
    local c = hint:sub(i, i)
    local b = string.byte(c)
    if not b or b < 48 or b > 57 or seen_h[c] then return nil end
    if secret:find(c, 1, true) then return nil end
    seen_h[c] = true
  end
  local fr = num_field(parts[12])
  local fc = num_field(parts[13])
  if not fr or not fc or fr < 1 or fr > #Game.PAD then return nil end
  if fc < 1 or fc > Game.pad_cols(fr) then return nil end
  local history = {}
  local blob = parts[14]
  if blob ~= "" then
    local seen = {}
    for tok in blob:gmatch("[^;]+") do
      local g, a, b = tok:match("^(%d+),(%d+),(%d+)$")
      a, b = tonumber(a), tonumber(b)
      if not g or not Game.digits_ok(g, len) or not a or not b then return nil end
      if a + b > len or a < 0 or b < 0 then return nil end
      local sa, sb = Game.score(secret, g)
      if sa ~= a or sb ~= b then return nil end
      if seen[g] then return nil end
      seen[g] = true
      history[#history + 1] = { g = g, a = a, b = b }
      if #history > Game.HIST_MAX then return nil end
    end
  end
  if won == "1" then
    local last = history[#history]
    if not last or last.a ~= len then return nil end
  end
  if guesses < #history then return nil end
  return {
    len = len,
    secret = secret,
    entry = entry,
    history = history,
    won = won == "1",
    guesses = guesses,
    wins = wins,
    giveups = giveups,
    best = best,
    rng = rng,
    hint = hint,
    focus_r = fr,
    focus_c = fc,
    help = false,
  }
end
