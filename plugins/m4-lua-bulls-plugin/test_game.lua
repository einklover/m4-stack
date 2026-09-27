dofile((arg and arg[0] and arg[0]:match("(.*/)") or "./") .. "game.lua")

local fails = 0
local function eq(a, b, msg)
  if a ~= b then
    fails = fails + 1
    print("FAIL " .. msg .. " got=" .. tostring(a) .. " want=" .. tostring(b))
  end
end

do
  local a, b = Game.score("4271", "1234")
  eq(a, 1, "4271 vs 1234 A")
  eq(b, 2, "4271 vs 1234 B")
  eq(Game.mark(a, b), "1A2B", "mark 1A2B")
  a, b = Game.score("4271", "4271")
  eq(a, 4, "exact A")
  eq(b, 0, "exact B")
  eq(Game.mark(a, b), "4A0B", "mark 4A0B")
  eq(Game.score("4271", "1123"), nil, "dup guess rejected")
  eq(Game.score("1123", "4271"), nil, "dup secret rejected")
  eq(Game.score("4271", "427"), nil, "short rejected")
  eq(Game.score("4271", "0427"), nil, "lead zero rejected")
  eq(Game.score("0427", "4271"), nil, "lead zero secret rejected")
end

do
  local s1, r1 = Game.make_secret(4, 42)
  local s2, r2 = Game.make_secret(4, 42)
  eq(s1, s2, "rng same secret")
  eq(r1, r2, "rng same state")
  eq(Game.digits_ok(s1, 4), true, "secret well formed")
  eq(s1:sub(1, 1) == "0", false, "no lead zero")
  local s3 = Game.make_secret(4, 99)
  eq(s1 == s3, false, "different seed differs")
  local seen = {}
  for i = 1, #s1 do
    local c = s1:sub(i, i)
    eq(seen[c], nil, "unique " .. c)
    seen[c] = true
  end
  local t3 = Game.make_secret(3, 7)
  local t5 = Game.make_secret(5, 7)
  eq(#t3, 3, "len 3")
  eq(#t5, 5, "len 5")
  eq(Game.make_secret(2, 1), nil, "bad len")
end

do
  local st = Game.new_state(4, 1)
  st.secret = "4271"
  st.entry = "1234"
  eq(Game.submit(st), "ok", "submit 1A2B")
  eq(st.history[1].a, 1, "hist A")
  eq(st.history[1].b, 2, "hist B")
  eq(st.entry, "", "entry cleared")
  st.entry = "1234"
  eq(Game.submit(st), "dup", "dup guess")
  st.entry = "1123"
  eq(Game.submit(st), "bad", "dup digits")
  st.entry = "0123"
  eq(Game.submit(st), "bad", "lead zero entry")
  eq(Game.push_digit(st, "0"), false, "no lead 0 key")
  st.entry = ""
  eq(Game.push_digit(st, "4"), true, "digit 4")
  eq(Game.push_digit(st, "4"), false, "repeat digit")
  eq(Game.push_digit(st, "2"), true, "digit 2")
  eq(Game.push_digit(st, "7"), true, "digit 7")
  eq(Game.push_digit(st, "1"), true, "digit 1")
  eq(Game.push_digit(st, "9"), false, "full")
  eq(Game.submit(st), "win", "win")
  eq(st.won, true, "won flag")
  eq(st.wins, 1, "wins")
  eq(st.best, 2, "best")
  eq(Game.push_digit(st, "5"), false, "no input after win")
end

do
  local st = Game.new_state(4, 1)
  st.secret = "4271"
  for i = 1, 14 do
    st.entry = tostring(1000 + i * 11) -- may duplicate digits; craft unique
  end
  -- 14 unique-ish guesses that are not the secret
  local guesses = {
    "1234","5678","1235","1236","1237","1238","1239",
    "1245","1246","1247","1248","1249","1256","1257",
  }
  for i = 1, #guesses do
    st.entry = guesses[i]
    local r = Game.submit(st)
    eq(r == "ok" or r == "dup" or r == "bad", true, "submit " .. guesses[i])
  end
  eq(#st.history <= 12, true, "hist cap")
  eq(#st.history, 12, "hist is 12")
  eq(st.history[1].g, "1235", "oldest dropped")
end

do
  local st = Game.new_state(4, 5)
  local secret = st.secret
  st.entry = "1234"
  -- may be invalid relative to secret uniqueness of guess itself
  if not Game.digits_ok("1234", 4) then error("fixture") end
  -- force a legal non-winning guess
  local alt = "1235"
  if alt == secret then alt = "1236" end
  -- ensure alt shares rules
  st.entry = ""
  for i = 1, #alt do Game.push_digit(st, alt:sub(i, i)) end
  if #st.entry == 4 then Game.submit(st) end
  local blob = Game.serialize(st)
  local back = Game.deserialize(blob)
  eq(back ~= nil, true, "roundtrip")
  eq(back.secret, secret, "secret restored")
  eq(back.wins, st.wins, "wins restored")
  eq(#back.history, #st.history, "hist restored")
  eq(Game.deserialize(nil), nil, "nil ser")
  eq(Game.deserialize(""), nil, "empty ser")
  eq(Game.deserialize("nope"), nil, "junk ser")
  eq(Game.deserialize("1|4|1123||||0|0|0|0|0|1|-|1|1|"), nil, "dup secret ser")
  eq(Game.deserialize("1|4|0427||||0|0|0|0|0|1|-|1|1|"), nil, "lead0 ser")
  eq(Game.deserialize("1|9|4271||0|0|0|0|0|1|-|1|1|"), nil, "bad len ser")
  eq(Game.deserialize("1|4|4271|1123|0|0|0|0|0|1|-|1|1|"), nil, "bad entry ser")
  eq(Game.deserialize("1|4|4271||2|0|0|0|0|1|-|1|1|"), nil, "bad won flag")
  eq(Game.deserialize("1|4|4271||1|0|0|0|0|1|-|1|1|"), nil, "won without 4A")
  eq(Game.deserialize("1|4|4271||0|0|0|0|0|1|4-|1|1|"), nil, "hint is secret digit")
  local long = "1|4|4271||0|0|0|0|0|1|-|1|1|" .. string.rep("1234,1,2;", 13)
  eq(Game.deserialize(long), nil, "hist overflow")
  -- mismatched score
  eq(Game.deserialize("1|4|4271||0|1|0|0|0|1|-|1|1|1234,0,0"), nil, "score mismatch")
  local win = Game.new_state(4, 1)
  win.secret = "4271"
  win.entry = "4271"
  eq(Game.submit(win), "win", "ser win submit")
  local w2 = Game.deserialize(Game.serialize(win))
  eq(w2.won, true, "win restored")
  eq(w2.history[1].a, 4, "win hist")
  local keep_wins = w2.wins
  Game.restart(w2, 4)
  eq(w2.won, false, "restart clears won")
  eq(w2.wins, keep_wins, "stats kept")
  eq(w2.secret == "4271", false, "new secret")
end

do
  local st = Game.new_state(4, 3)
  st.secret = "4271"
  local before = Game.serialize(st)
  eq(Game.on_key(st, "nope"), false, "wrong key")
  eq(Game.on_key(st, ""), false, "empty key")
  eq(Game.serialize(st), before, "wrong key no change")
  eq(Game.on_key(st, "right"), true, "right moves")
  eq(st.focus_c, 2, "focus c")
  -- move to submit: row 6 col 2 is "ok". From (1,2) down five times.
  st.focus_r, st.focus_c = 6, 2
  st.entry = "1234"
  local changed, status = Game.on_key(st, "confirm")
  eq(changed, true, "confirm submit")
  eq(status, "ok", "status ok")
  eq(st.history[1].g, "1234", "guess stored")
  st.entry = "4271"
  st.focus_r, st.focus_c = 6, 2
  changed, status = Game.on_key(st, "confirm")
  eq(status, "win", "confirm win")
  changed, status = Game.on_key(st, "confirm")
  eq(status, "new", "confirm restarts after win")
  eq(st.won, false, "after restart")
  -- hint does not reveal secret
  st.secret = "4271"
  st.focus_r, st.focus_c = 6, 1
  Game.on_key(st, "confirm")
  eq(#st.hint >= 1, true, "hint char")
  eq(st.secret:find(st.hint:sub(1, 1), 1, true), nil, "hint not in secret")
  eq(st.hint:find(st.secret, 1, true), nil, "hint hides secret")
end

if fails > 0 then
  error(fails .. " failures", 0)
end
print("test_game.lua OK")
