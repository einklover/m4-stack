-- Cookie jar, login poll state machine, renewal (-2012).
Auth = {
  cookies = {},
  login_uid = nil,
  login_fatal = false,
  login_msg = "准备登录",
  poll_deadline = 0,
  poll_interval_ms = 2000,
  poll_next_ms = 0,
  cancelled = false,
}

local HOST = "https://weread.qq.com"
local LOGIN_POLL_MS = 120000  -- 2 minutes

function Auth.jstr(v)
  if v == nil then return "" end
  if type(v) == "string" then return v end
  if type(v) == "number" then
    if v == math.floor(v) then return tostring(math.floor(v)) end
    return tostring(v)
  end
  return tostring(v)
end

function Auth.load()
  local doc = Storage.read_json("config.json")
  Auth.cookies = (doc and doc.cookies) or {}
end

function Auth.save(c)
  if c then Auth.cookies = c end
  Storage.write_json("config.json", { cookies = Auth.cookies or {} })
end

function Auth.clear()
  Auth.cookies = {}
  Storage.write_json("config.json", { cookies = {} })
end

function Auth.has()
  -- Minimum usable identity: both wr_vid and wr_skey must be present.
  local c = Auth.cookies
  return c and c.wr_vid and tostring(c.wr_vid) ~= "" and c.wr_skey and tostring(c.wr_skey) ~= ""
end

function Auth.cookie_header()
  local c = Auth.cookies
  if not c or not c.wr_skey then return "" end
  local parts = {}
  if c.wr_vid then parts[#parts + 1] = "wr_vid=" .. tostring(c.wr_vid) end
  if c.wr_skey then parts[#parts + 1] = "wr_skey=" .. tostring(c.wr_skey) end
  if c.wr_rt then parts[#parts + 1] = "wr_rt=" .. tostring(c.wr_rt) end
  parts[#parts + 1] = "wr_localvid=" .. tostring(c.wr_vid or "")
  return table.concat(parts, "; ")
end

-- Absorb Set-Cookie lines from net.request result (never log values).
function Auth.absorb_set_cookie(resp)
  if not resp then return false end
  local list = resp.set_cookie
  if type(list) ~= "table" then
    -- fallback: headers["Set-Cookie"] may be string or array
    local h = resp.headers and (resp.headers["Set-Cookie"] or resp.headers["set-cookie"])
    if type(h) == "string" then list = { h }
    elseif type(h) == "table" then list = h
    else return false end
  end
  local changed = false
  local c = Auth.cookies or {}
  for i = 1, #list do
    local line = list[i]
    if type(line) == "string" then
      local nv = line:match("^([^;]+)")
      if nv then
        local name, val = nv:match("^%s*([^=]+)=(.*)$")
        if name and val then
          name = name:lower():gsub("^%s+", ""):gsub("%s+$", "")
          if name == "wr_vid" or name == "wr_skey" or name == "wr_rt" then
            c[name] = val
            changed = true
          end
        end
      end
    end
  end
  if changed then
    Auth.cookies = c
    Auth.save()
  end
  return changed
end

function Auth.begin_login()
  Auth.login_uid = nil
  Auth.login_fatal = false
  Auth.cancelled = false
  Auth.login_msg = "获取登录码..."
  Auth.poll_deadline = 0
  Auth.poll_next_ms = 0
  -- Prefer host connectSaved (after file-transfer Wi-Fi drop) over immediate offline.
  if not net.isConnected() then
    if type(net.connectSaved) == "function" then
      local cr = net.connectSaved(20000)
      if not (type(cr) == "table" and cr.ok) then
        local err = (type(cr) == "table" and cr.error and cr.error ~= "") and cr.error or "connect_failed"
        if err == "no_saved_wifi" then
          Auth.login_msg = "无已存 Wi-Fi · 请先设置"
        elseif err == "timeout" then
          Auth.login_msg = "Wi-Fi 超时 · 点按重试"
        elseif err == "cancelled" then
          Auth.login_msg = "已取消连接"
        else
          Auth.login_msg = "Wi-Fi 失败 · 点按重试"
        end
        return false
      end
    else
      Auth.login_msg = "未连接 Wi-Fi"
      return false
    end
  end
  local r = net.request("GET", HOST .. "/api/auth/getLoginUid", { timeout_ms = 15000 })
  Auth.absorb_set_cookie(r)
  if not r or not r.ok then
    local detail = tostring(r and (r.error ~= "" and r.error or r.status) or "?")
    local detail_lower = detail:lower()
    if detail_lower:find("memory allocation failed", 1, true)
        or detail_lower:find("out of memory", 1, true)
        or detail_lower == "oom" then
      -- This cannot recover inside the current app session. Returning to the
      -- desktop tears down Lua and releases all per-app allocations.
      Auth.login_msg = "内存不足，正在退出"
      Auth.login_fatal = true
      return false
    end
    if detail == "wifi_not_connected" then
      Auth.login_msg = "无网络 · 点按重试"
    elseif type(detail) == "string" and (detail:find("ssl", 1, true) or detail:find("tls", 1, true)) then
      Auth.login_msg = "TLS 失败: " .. detail
    else
      Auth.login_msg = "获取 uid 失败: " .. detail
    end
    return false
  end
  local doc = json.decode(r.body)
  r.body = nil
  if not doc then
    Auth.login_msg = "uid 响应无效"
    return false
  end
  local uid = Auth.jstr(doc.uid)
  if uid == "" and doc.data then uid = Auth.jstr(doc.data.uid) end
  if uid == "" then
    Auth.login_msg = "无 uid"
    return false
  end
  Auth.login_uid = uid
  Auth.login_msg = "请用微信/微信读书扫码"
  Auth.poll_deadline = sys.millis() + LOGIN_POLL_MS
  -- Allow first poll immediately (headless tap / first draw); later polls use interval.
  Auth.poll_next_ms = 0
  return true
end

function Auth.cancel_login()
  Auth.cancelled = true
  Auth.login_uid = nil
  Auth.poll_deadline = 0
end

-- Non-blocking poll step; returns "pending"|"ok"|"fail"|"timeout"|"cancelled"
function Auth.poll_login_step()
  if Auth.cancelled then return "cancelled" end
  if not Auth.login_uid then return "fail" end
  local now = sys.millis()
  if Auth.poll_deadline > 0 and now > Auth.poll_deadline then
    Auth.login_msg = "登录超时，点按重试"
    return "timeout"
  end
  if now < Auth.poll_next_ms then return "pending" end
  Auth.poll_next_ms = now + Auth.poll_interval_ms

  local r = net.request("GET", HOST .. "/api/auth/getLoginInfo?uid=" .. Auth.login_uid .. "&otp=", {
    timeout_ms = 12000,
  })
  Auth.absorb_set_cookie(r)
  if not r or not r.ok then return "pending" end
  local doc = json.decode(r.body)
  r.body = nil
  if not doc then return "pending" end
  local data = doc.data or doc
  local succeed = data.succeed
  local vid = Auth.jstr(data.webLoginVid or data.vid or data.userVid or data.user_vid)
  local token = Auth.jstr(data.accessToken)
  local logic = Auth.jstr(data.logicCode)
  -- Prefer authoritative Set-Cookie (already absorbed). JSON tokens are fallback only.
  local c = Auth.cookies or {}
  local skey = Auth.jstr(c.wr_skey)
  if skey == "" then skey = token end
  local final_vid = Auth.jstr(c.wr_vid)
  if final_vid == "" then final_vid = vid end
  if succeed and final_vid ~= "" and skey ~= "" then
    c.wr_vid = final_vid
    c.wr_skey = skey  -- do not overwrite cookie skey with accessToken if cookie present
    if Auth.jstr(c.wr_rt) == "" then
      local rt = Auth.jstr(data.refreshToken or data.wr_rt)
      if rt ~= "" then c.wr_rt = rt end
    end
    Auth.save(c)
    Auth.login_msg = "登录成功"
    Auth.login_uid = nil
    Auth.poll_deadline = 0
    return "ok"
  end
  if logic == "NEED_OTP" or logic == "OTP_EXPIRED" then
    Auth.login_msg = "需要手机 OTP（暂不支持）"
    return "fail"
  elseif logic ~= "" and logic ~= "LOGIN_TIMEOUT" then
    Auth.login_msg = "登录: " .. logic
  end
  return "pending"
end

function Auth.try_renew()
  if not Auth.cookies or not Auth.cookies.wr_rt or Auth.cookies.wr_rt == "" then return false end
  local body = '{"rq":"%2Fweb%2Fbook%2Fread","ql":false}'
  local r = net.request("POST", HOST .. "/web/login/renewal", {
    headers = {
      Cookie = Auth.cookie_header(),
      Referer = "https://weread.qq.com/",
      ["Content-Type"] = "application/json",
    },
    body = body,
    timeout_ms = 15000,
  })
  if not r or not r.ok then return false end
  local absorbed = Auth.absorb_set_cookie(r)
  local doc = json.decode(r.body or "")
  r.body = nil
  if doc and type(doc) == "table" then
    -- Reject explicit failure codes
    if Auth.is_login_timeout(doc) then return false end
    local err = doc.errCode or doc.errcode or doc.err_code or doc.errorCode
    if err and err ~= 0 and err ~= "0" then return false end
    local c = Auth.cookies or {}
    local changed = absorbed
    -- JSON tokens only fill missing cookie fields (never clobber Set-Cookie)
    if (not c.wr_skey or c.wr_skey == "") and (doc.accessToken or doc.wr_skey) then
      c.wr_skey = Auth.jstr(doc.accessToken or doc.wr_skey)
      changed = true
    end
    if (not c.wr_rt or c.wr_rt == "") and (doc.refreshToken or doc.wr_rt) then
      c.wr_rt = Auth.jstr(doc.refreshToken or doc.wr_rt)
      changed = true
    end
    if changed then Auth.save(c) end
    -- Success only if we have usable identity after renewal
    return Auth.has()
  end
  -- HTTP 2xx with no JSON: require absorbed cookies or existing valid identity
  if absorbed then return Auth.has() end
  return false
end

function Auth.is_login_timeout(doc)
  if type(doc) ~= "table" then return false end
  local code = doc.errCode or doc.errcode or doc.err_code
  return code == -2012 or code == "-2012"
end

return Auth
