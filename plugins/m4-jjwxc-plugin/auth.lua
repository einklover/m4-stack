-- 晋江官方扫码登录 (晋江小说阅读 App 扫一扫)。
-- 流程: GET login.php → jjreaderKey (随机) → 画二维码
--       POST callback.php {jjreaderKey, action=check} 每 2s 轮询 → status 200
--       GET callback.php?action=login&jjreaderKey=KEY → Set-Cookie 会话
Auth = {
  cookies = {},
  key = nil,
  login_msg = "准备登录",
  poll_next_ms = 0,
  poll_deadline = 0,
  cancelled = false,
}

local LOGIN_BASE = "https://my.jjwxc.net/backend/login/jjreader"
local LOGIN_POLL_MS = 90000  -- 二维码 1 分钟有效, 留轮询余量

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
  Auth.key = nil
  Auth.poll_deadline = 0
  Storage.write_json("config.json", { cookies = {} })
end

-- 有会话即认为已登录 (token cookie 由扫码登录种下)。
function Auth.has()
  local c = Auth.cookies
  return type(c) == "table" and next(c) ~= nil and Auth.cookie_header() ~= ""
end

function Auth.cookie_header()
  local c = Auth.cookies
  if type(c) ~= "table" then return "" end
  local parts = {}
  for k, v in pairs(c) do
    local vs = tostring(v)
    if vs ~= "" then parts[#parts + 1] = tostring(k) .. "=" .. vs end
  end
  table.sort(parts)
  return table.concat(parts, "; ")
end

-- 吸收 Set-Cookie 行 (全部 cookie, 不记录日志值)。
function Auth.absorb_set_cookie(resp)
  if not resp then return false end
  local list = resp.set_cookie
  if type(list) ~= "table" then
    local h = resp.headers and (resp.headers["Set-Cookie"] or resp.headers["set-cookie"])
    if type(h) == "string" then list = { h }
    elseif type(h) == "table" then list = h
    else return false end
  end
  local c = Auth.cookies or {}
  local changed = false
  for i = 1, #list do
    local line = list[i]
    if type(line) == "string" then
      local nv = line:match("^([^;]+)")
      if nv then
        local name, val = nv:match("^%s*([^=]+)=(.*)$")
        if name and val then
          name = name:lower():gsub("^%s+", ""):gsub("%s+$", "")
          c[name] = val
          changed = true
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

-- 连接网络 + 取 jjreaderKey。返回 true / false (Auth.login_msg 含原因)。
function Auth.begin_login()
  Auth.key = nil
  Auth.cancelled = false
  Auth.login_msg = "获取登录码..."
  Auth.poll_deadline = 0
  Auth.poll_next_ms = 0
  if not net.isConnected() then
    if type(net.connectSaved) == "function" then
      local cr = net.connectSaved(20000)
      if not (type(cr) == "table" and cr.ok) then
        local err = (type(cr) == "table" and cr.error and cr.error ~= "") and cr.error or "connect_failed"
        if err == "no_saved_wifi" then
          Auth.login_msg = "无已存 Wi-Fi · 请先设置"
        elseif err == "timeout" then
          Auth.login_msg = "Wi-Fi 超时 · 点按重试"
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
  local r = net.request("GET", LOGIN_BASE .. "/login.php", { timeout_ms = 15000 })
  if not r or not r.ok or not r.body then
    Auth.login_msg = "获取登录码失败: " .. tostring(r and (r.error ~= "" and r.error or r.status) or "?")
    return false
  end
  local key = r.body:match('jjreaderKey%s*=%s*"([0-9a-fA-F]+)"')
  r.body = nil
  if not key then
    Auth.login_msg = "登录页响应无效"
    return false
  end
  Auth.key = string.lower(key)
  Auth.login_msg = "请用晋江App扫一扫"
  Auth.poll_deadline = (type(sys) == "table" and type(sys.millis) == "function" and sys.millis() or 0) + LOGIN_POLL_MS
  Auth.poll_next_ms = 0
  return true
end

function Auth.cancel_login()
  Auth.cancelled = true
  Auth.key = nil
  Auth.poll_deadline = 0
end

function Auth.qr_data()
  if not Auth.key then return nil end
  return "http://my.jjwxc.net/backend/login/jjreader/login.php?sign=" .. Auth.key
end

-- 非阻塞轮询: "pending" | "ok" | "fail" | "timeout" | "cancelled"
function Auth.poll_login_step()
  if Auth.cancelled then return "cancelled" end
  if not Auth.key then return "fail" end
  local now = (type(sys) == "table" and type(sys.millis) == "function" and sys.millis() or 0)
  if Auth.poll_deadline > 0 and now > Auth.poll_deadline then
    Auth.login_msg = "二维码已失效，点按重试"
    return "timeout"
  end
  if now < Auth.poll_next_ms then return "pending" end
  Auth.poll_next_ms = now + 2000

  local r = net.request("POST", LOGIN_BASE .. "/callback.php", {
    headers = { ["Content-Type"] = "application/x-www-form-urlencoded" },
    body = "jjreaderKey=" .. Auth.key .. "&action=check",
    timeout_ms = 12000,
  })
  if not r or not r.ok or not r.body then return "pending" end
  local okd, doc = pcall(json.decode, r.body)
  r.body = nil
  if not okd or type(doc) ~= "table" then return "pending" end
  if tonumber(doc.status) ~= 200 then return "pending" end
  -- 已扫码: 执行正式登录, 吸收会话 cookie。
  local lr = net.request("GET", LOGIN_BASE .. "/callback.php?action=login&jjreaderKey=" .. Auth.key, {
    timeout_ms = 15000,
  })
  local absorbed = Auth.absorb_set_cookie(lr)
  local doc2 = nil
  if lr and lr.body and lr.body ~= "" then
    local ok2, d2 = pcall(json.decode, lr.body)
    if ok2 and type(d2) == "table" then doc2 = d2 end
  end
  if lr then lr.body = nil end
  if not absorbed and not doc2 then
    Auth.login_msg = "登录失败 · 点按重试"
    return "fail"
  end
  if not Auth.has() then
    Auth.login_msg = "未获得会话 · 点按重试"
    return "fail"
  end
  Auth.login_msg = "登录成功"
  Auth.key = nil
  Auth.poll_deadline = 0
  return "ok"
end

return Auth
