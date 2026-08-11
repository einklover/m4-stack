-- 晋江文学城 API: androidapi (UTF-8 JSON) + WAP (GB18030 HTML, VIP 章).
-- 数据源优先级: dl.jsonGet/dl.jsonToFile 宿主投影 > net.request + json.decode。
Api = {}

local API_HOST = "https://app-cdn.jjwxc.net"
local WAP_HOST = "https://m.jjwxc.net"
-- androidapi 需要带 app UA, 否则返回 code 2016 (浏览器标识异常)。
local APP_UA = "Mozilla/5.0 (Linux; Android 5.1; Lenovo) AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 Chrome/39.0.0.0 Mobile Safari/537.36/JINJIANG-Android/206(Lenovo;android 5.1;Scale/2.0)"
local APP_REF = "http://android.jjwxc.net?v=206"
local WAP_UA = "Mozilla/5.0 (Linux; Android 10.0; wv) AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 Chrome/78.0.3904.108 Mobile Safari/537.36"

function Api.jstr(v)
  if v == nil then return "" end
  if type(v) == "string" then return v end
  if type(v) == "number" then
    if v == math.floor(v) then return tostring(math.floor(v)) end
    return tostring(v)
  end
  return tostring(v)
end

local function app_headers(extra)
  local h = { ["User-Agent"] = APP_UA, Referer = APP_REF }
  if type(extra) == "table" then
    for k, v in pairs(extra) do h[k] = v end
  end
  return h
end

local function decode_or_err(body)
  if type(body) ~= "string" or body == "" then return nil, "empty" end
  local ok, doc = pcall(json.decode, body)
  if not ok or type(doc) ~= "table" then return nil, "json" end
  return doc, nil
end

-- 分类频道书籍 (bookstore getFullPage)。
-- channel: 频道 ID (如 14000019); page 1-based; limit 条/页。
-- 优先 dl.jsonGet 宿主投影: 响应常带大段 intro/封面, 整包 net.request+json.decode
-- 会撑爆 512KB Lua 堆 (分类列表 OOM)。只取 4 个字段进 Lua。
function Api.fetch_category(channel, page, limit, progress)
  -- progress(opt): function(status, info) for loading UI (bytes/phase/elapsed).
  channel = tostring(channel or "")
  if channel == "" then return nil, "no_channel" end
  page = math.max(1, tonumber(page) or 1)
  limit = math.max(5, math.min(25, tonumber(limit) or 25))
  local offset = (page - 1) * limit
  local body_json = '{"' .. channel .. '":{"offset":"' .. tostring(offset)
    .. '","limit":"' .. tostring(limit) .. '"}}'
  local url = API_HOST .. "/bookstore/getFullPage?versionCode=148&channelBody="
    .. crypto.urlEncode(body_json)
  local function prog(status, info)
    if type(progress) == "function" then
      pcall(progress, status, info or {})
    end
  end

  -- Host projection: body in PSRAM, only 4 fields per row enter Lua.
  -- CRITICAL: path keys must be strings — host only accepts lua_isstring path
  -- entries; a numeric channel (14000017) was silently dropped → empty path →
  -- parse/projection fail / long hang / OOM on net.request fallback.
  local last_err = nil
  if type(dl) == "table" and type(dl.jsonGet) == "function" then
    local hdrs = app_headers({ Connection = "close" })
    for attempt = 1, 2 do
      if attempt > 1 then
        if collectgarbage then
          collectgarbage("collect")
          collectgarbage("collect")
        end
        prog("retry", { attempt = attempt })
      end
      prog("download", {
        attempt = attempt,
        channel = channel,
        page = page,
        label = "下载书单 JSON…",
      })
      local t0 = (sys and sys.millis and sys.millis()) or 0
      local list, gerr = dl.jsonGet({
        url = url,
        headers = hdrs,
        -- must be string key matching JSON object field
        path = { channel },
        fields = { "novelId", "novelName", "authorName", "novelStep" },
        max_bytes = 256 * 1024,
        timeout_ms = 20000,
      })
      local dt = 0
      if sys and sys.millis then dt = sys.millis() - t0 end
      if type(list) == "table" then
        local books = {}
        for i = 1, #list do
          local it = list[i]
          if type(it) == "table" then
            local id = Api.jstr(it.novelId)
            if id ~= "" then
              books[#books + 1] = {
                bookId = id,
                title = Api.jstr(it.novelName ~= "" and it.novelName or "无标题"),
                author = Api.jstr(it.authorName or ""),
                status = Api.jstr(it.novelStep or ""),
              }
              if #books >= limit then break end
            end
          end
        end
        local nproj = #list
        list = nil
        if collectgarbage then collectgarbage("collect") end
        if #books > 0 then
          prog("done", {
            count = #books,
            projected = nproj,
            ms = dt,
            source = "jsonGet",
          })
          return books, nil, { count = #books, ms = dt, source = "jsonGet" }
        end
        last_err = "empty"
        if type(log) == "function" then
          log("[JJ] fetch_category jsonGet empty channel=" .. channel
            .. " page=" .. tostring(page) .. " attempt=" .. tostring(attempt)
            .. " ms=" .. tostring(dt))
        end
        break
      end
      last_err = tostring(gerr or "jsonGet_fail")
      if type(log) == "function" then
        log("[JJ] fetch_category jsonGet fail channel=" .. channel
          .. " err=" .. last_err .. " attempt=" .. tostring(attempt)
          .. " ms=" .. tostring(dt))
      end
      prog("fail", { err = last_err, attempt = attempt, ms = dt })
      if last_err == "timeout" or last_err == "oom" or last_err == "out_of_memory"
          or last_err == "json_parse_failed"
          or last_err:find("oom", 1, true) or last_err:find("memory", 1, true) then
        -- retry once
      else
        break
      end
    end
    -- Never fall through to net.request after jsonGet path was exercised:
    -- getFullPage carries intro/cover blobs; full Lua json.decode OOMs the 512KB heap.
    if type(log) == "function" then
      log("[JJ] fetch_category no_net_fallback channel=" .. channel
        .. " err=" .. tostring(last_err or "empty") .. " page=" .. tostring(page))
    end
    return nil, last_err or "empty"
  else
    -- Host lacks dl.jsonGet: no safe path. getFullPage intro/cover blobs would
    -- OOM the 512KB Lua heap on json.decode — never fall back to net.request.
    prog("fail", { err = "no_jsonget" })
    if type(log) == "function" then
      log("[JJ] fetch_category no_jsonGet host")
    end
    return nil, "no_jsonget"
  end
end

-- 今日限免: 响应是 channelName+data 嵌套数组。
-- 不要用 path={} 的 jsonGet 猜行：宿主投影会把「分类频道行」当成 novelId 行，
-- 模拟器 mock 也会把 getFullPage 全打成分类数组 → 限免测失败 / 真机误解析。
-- 走 net.request + 体积极限 + 低内存拒绝（OOM 防护）。
function Api.fetch_novelfree()
  local url = API_HOST .. "/bookstore/getFullPage?channel=novelfree"

  -- Full novelfree JSON is often 100KB+ with nested intros; decode in Lua OOMs.
  -- Prefer fail-soft over net.request when headroom is tight or body is huge.
  if type(sys) == "table" and type(sys.memInfo) == "function" then
    local mi = sys.memInfo()
    local head = type(mi) == "table" and tonumber(mi.lua_headroom or 0) or 0
    if head > 0 and head < 64000 then
      if type(log) == "function" then
        log("[JJ] fetch_novelfree skip_net low_mem head=" .. tostring(head))
      end
      return nil, "oom"
    end
  end
  local r = net.request("GET", url, { headers = app_headers(), timeout_ms = 25000 })
  if not r or not r.ok then return nil, (r and r.error) or "no_response" end
  local body_n = type(r.body) == "string" and #r.body or 0
  if body_n > 64 * 1024 then
    r.body = nil
    if collectgarbage then collectgarbage("collect") end
    if type(log) == "function" then
      log("[JJ] fetch_novelfree body_too_large bytes=" .. tostring(body_n))
    end
    return nil, "json_too_large"
  end
  local doc, derr = decode_or_err(r.body)
  r.body = nil
  if collectgarbage then collectgarbage("collect") end
  if not doc then return nil, derr end
  local books = {}
  for ci = 1, #doc do
    local ch = doc[ci]
    if type(ch) == "table" and type(ch.data) == "table" then
      for i = 1, #ch.data do
        local it = ch.data[i]
        if type(it) == "table" then
          local id = Api.jstr(it.novelId or it.novelid)
          if id ~= "" then
            books[#books + 1] = {
              bookId = id,
              title = Api.jstr(it.novelName or it.novelname or "无标题"),
              author = Api.jstr(it.authorName or it.authorname or ""),
              status = Api.jstr(it.novelStep or ""),
            }
            if #books >= 24 then
              doc = nil
              if collectgarbage then collectgarbage("collect") end
              return books, nil
            end
          end
        end
      end
    end
  end
  doc = nil
  if collectgarbage then collectgarbage("collect") end
  if #books == 0 then return nil, "empty" end
  return books, nil
end

-- 勿整文件 annotate VIP 标题：大目录会吃光 Lua 堆并卡死。
-- VIP 展示交给宿主 native TOC 的 vipField（isvip 列），打开路径仍读 isvip 列。
function Api.annotate_toc_vip_titles(_relPath)
  return false
end

-- 目录写入 SD (FileRows): 每行 chapterid\tchaptername\tchaptertype\tisvip\tislock
-- 返回 count [, err] [, info]  info = {bytes=N, source="jsonToFile"|"jsonGet"|"net"}
function Api.fetch_toc_to_file(bookId, relPath)
  local url = API_HOST .. "/androidapi/chapterList?novelId=" .. tostring(bookId)
    .. "&more=0&whole=1"
  local function file_bytes()
    if type(fs) == "table" and type(fs.fileSize) == "function" then
      return tonumber(fs.fileSize(relPath) or 0) or 0
    end
    return 0
  end
  if type(dl) == "table" and type(dl.jsonToFile) == "function" then
    if type(fs) == "table" and type(fs.fileSize) == "function"
        and type(fs.writeFile) == "function" and fs.fileSize(relPath) == nil then
      fs.writeFile(relPath, "")
    end
    local res, err = dl.jsonToFile({
      url = url,
      headers = app_headers(),
      out = relPath,
      path = { "chapterlist" },
      fields = { "chapterid", "chaptername", "chaptertype", "isvip", "islock" },
      max_bytes = 8 * 1024 * 1024,
      timeout_ms = 30000,
    })
    -- 仅瞬时流式错误重试一次 (避免每次失败都双倍等 30s)
    if not (res and res.ok) then
      local es = tostring(err or "")
      if es:find("json_syntax", 1, true) or es:find("stream", 1, true)
          or es:find("timeout", 1, true) or es:find("incomplete", 1, true) then
        if collectgarbage then collectgarbage("collect") end
        res, err = dl.jsonToFile({
          url = url,
          headers = app_headers(),
          out = relPath,
          path = { "chapterlist" },
          fields = { "chapterid", "chaptername", "chaptertype", "isvip", "islock" },
          max_bytes = 8 * 1024 * 1024,
          timeout_ms = 30000,
        })
      end
    end
    if res and res.ok then
      local cnt = tonumber(res.count or 0) or 0
      if cnt == 0 then return nil, "empty" end
      return cnt, nil, { bytes = file_bytes(), source = "jsonToFile" }
    end
    -- jsonGet 投影 → 拼行写盘: 边转边释放 list 行, 再 rope 合并 (避免双倍驻留)
    if type(dl.jsonGet) == "function" then
      if collectgarbage then collectgarbage("collect") end
      local list2, gerr = dl.jsonGet({
        url = url,
        headers = app_headers(),
        path = { "chapterlist" },
        fields = { "chapterid", "chaptername", "chaptertype", "isvip", "islock" },
        max_bytes = 8 * 1024 * 1024,
        timeout_ms = 30000,
      })
      if type(list2) == "table" and #list2 > 0 then
        local lines = {}
        local cnt = 0
        for i = 1, #list2 do
          local c = list2[i]
          list2[i] = nil
          if type(c) == "table" and tostring(c.chapterid or "") ~= "" then
            cnt = cnt + 1
            -- 保持原始标题；VIP 由宿主 vipField=isvip 列渲染，避免改写文件卡死
            lines[cnt] = tostring(c.chapterid) .. "\t" .. tostring(c.chaptername or "")
              .. "\t" .. tostring(c.chaptertype or "0") .. "\t" .. tostring(c.isvip or "0")
              .. "\t" .. tostring(c.islock or "0")
          end
          c = nil
        end
        list2 = nil
        if collectgarbage then collectgarbage("collect") end
        if cnt < 1 then return nil, "empty" end
        local body = table.concat(lines, "\n") .. "\n"
        lines = nil
        if collectgarbage then collectgarbage("collect") end
        local okw = false
        if type(fs) == "table" and type(fs.replaceFile) == "function" then
          okw = fs.replaceFile(relPath, body)
        elseif type(fs) == "table" and type(fs.writeFile) == "function" then
          okw = fs.writeFile(relPath, body)
        end
        local nbytes = body and #body or 0
        body = nil
        if collectgarbage then collectgarbage("collect") end
        if okw and cnt > 0 then
          return cnt, nil, { bytes = nbytes > 0 and nbytes or file_bytes(), source = "jsonGet" }
        end
        return nil, "toc_rows_write"
      end
      return nil, gerr or err or "jsonToFile"
    end
    return nil, err or "jsonToFile"
  end
  return nil, "no_jsonToFile"
end

-- 目录完整载入 Lua (旧宿主 / 小书)。
function Api.fetch_toc(bookId)
  local url = API_HOST .. "/androidapi/chapterList?novelId=" .. tostring(bookId)
    .. "&more=0&whole=1"
  -- 目录优先 dl.jsonGet 投影: 宿主流式提取 5 个字段, 大书目录不占 Lua 堆。
  -- (全量 JSON 经 net.request 进 Lua 堆 decode 会触发 json_too_large, 71KB 即失败)
  local list, err
  if type(dl) == "table" and type(dl.jsonGet) == "function" then
    list, err = dl.jsonGet({
      url = url,
      headers = app_headers(),
      path = { "chapterlist" },
      fields = { "chapterid", "chaptername", "chaptertype", "isvip", "islock" },
      max_bytes = 8 * 1024 * 1024,
      timeout_ms = 30000,
    })
    if list then
      -- 投影行转章节表 (5 字段小表, Lua 堆可控)
      local chapters = {}
      for i = 1, #list do
        local c = list[i]
        if type(c) == "table" then
          local uid = Api.jstr(c.chapterid)
          if uid ~= "" then
            chapters[#chapters + 1] = {
              chapterUid = uid,
              title = Api.jstr(c.chaptername or ("第" .. tostring(i) .. "章")),
              chaptertype = Api.jstr(c.chaptertype or "0"),
              isvip = Api.flag_is_vip(c.isvip),
              islock = Api.flag_is_vip(c.islock),
            }
          end
        end
      end
      if #chapters > 0 then return chapters, nil end
      err = "empty"
    end
  end
  if not list and type(net) == "table" and type(net.request) == "function" then
    local r = net.request("GET", url, { headers = app_headers(), timeout_ms = 30000 })
    if r and r.ok and r.body then
      local doc, derr = decode_or_err(r.body)
      r.body = nil
      if collectgarbage then collectgarbage("collect") end
      if doc and type(doc.chapterlist) == "table" then list = doc.chapterlist end
      if not list then err = derr end
    else
      err = (r and r.error) or "no_response"
    end
  end
  if not list then return nil, err or "toc_fetch" end
  local chapters = {}
  for i = 1, #list do
    local c = list[i]
    if type(c) == "table" then
      local uid = Api.jstr(c.chapterid)
      if uid ~= "" then
        chapters[#chapters + 1] = {
          chapterUid = uid,
          title = Api.jstr(c.chaptername or ("第" .. tostring(i) .. "章")),
          chaptertype = Api.jstr(c.chaptertype or "0"),
          isvip = Api.flag_is_vip(c.isvip),
          islock = Api.flag_is_vip(c.islock),
        }
        if #chapters >= 900 then break end
      end
    end
  end
  if #chapters == 0 then return nil, "empty" end
  return chapters, nil
end

-- HTML 实体解码 (最小集合, 正文常用)。
local function decode_entities(s)
  s = s:gsub("&nbsp;", " "):gsub("&lt;", "<"):gsub("&gt;", ">"):gsub("&quot;", '"')
  s = s:gsub("&#39;", "'"):gsub("&amp;", "&")
  return s
end

-- 清洗 WAP <li> 内 HTML 片段 → 纯文本。
local function clean_wap_fragment(m)
  if not m or m == "" then return nil, "no_body" end
  m = m:gsub("<br%s*/?>", "\n")
  m = m:gsub("<[^>]+>", "")
  m = decode_entities(m)
  m = m:gsub("^%s+", ""):gsub("%s+$", "")
  if m == "" then return nil, "no_body" end
  return m, nil
end

-- 从 WAP book2 页提取正文 (UTF-8 输入)。返回文本或 nil, err。
local function extract_wap_body(html)
  if not html or html == "" then return nil, "no_body" end
  local m = html:match('<ul class="content_ul">.-<li>(.-)</li>.-</ul>')
  if not m then
    if html:find("VIP", 1, true) or html:find("购买", 1, true) or html:find("vip", 1, true) then
      return nil, "vip_unpaid"
    end
    if html:find("登录", 1, true) then
      return nil, "login_expired"
    end
    return nil, "no_body"
  end
  return clean_wap_fragment(m)
end

-- 在 GBK 原始字节里找 ASCII 标记 (不解码整页)。返回 0-based 字节偏移或 nil。
local function find_bytes(path, needle, start_off)
  if type(fs) ~= "table" or type(fs.readRange) ~= "function" then return nil end
  local n = (type(fs.fileSize) == "function" and tonumber(fs.fileSize(path) or 0)) or 0
  if n < 1 or type(needle) ~= "string" or needle == "" then return nil end
  local off = math.max(0, math.floor(tonumber(start_off) or 0))
  local nl = #needle
  local window = 1536
  while off < n do
    local chunk = fs.readRange(path, off, window + nl)
    if type(chunk) ~= "string" or chunk == "" then break end
    local i = chunk:find(needle, 1, true)
    if i then return off + i - 1 end
    if #chunk <= nl then break end
    off = off + #chunk - nl
  end
  return nil
end

local function file_has_ascii(path, needle)
  return find_bytes(path, needle, 0) ~= nil
end

-- VIP 章直写 SD: dl.download 落盘 → 只抽 <li> 区间 GBK 转 UTF-8 → 清洗标签。
-- 整页 HTML / GBK 表不会与整段 UTF-8 同时驻留。
function Api.fetch_vip_chapter_to_file(bookId, chapterId, relPath)
  if type(Auth) ~= "table" or not Auth.has() then
    return nil, "login_required"
  end
  chapterId = tostring(chapterId or "")
  if chapterId == "" or type(relPath) ~= "string" or relPath == "" then
    return nil, "no_id"
  end
  local url = WAP_HOST .. "/book2/" .. tostring(bookId) .. "/" .. chapterId
  local tmp = "cache/_wap_" .. tostring(bookId) .. "_" .. chapterId .. ".bin"
  local downloaded = 0

  if type(dl) == "table" and type(dl.download) == "function" then
    local ok, res = dl.download({
      url = url,
      headers = { ["User-Agent"] = WAP_UA, Cookie = Auth.cookie_header() },
      path = tmp,
      max_bytes = 2 * 1024 * 1024,
      timeout_ms = 30000,
    })
    if not ok then
      return nil, (type(res) == "string" and res) or "wap_download"
    end
    downloaded = tonumber(type(res) == "table" and res.size or 0) or 0
    if downloaded < 1 and type(fs) == "table" and type(fs.fileSize) == "function" then
      downloaded = tonumber(fs.fileSize(tmp) or 0) or 0
    end
  else
    -- 旧宿主: net.request 整包进 Lua (尽量少驻留)
    local r = net.request("GET", url, {
      headers = { ["User-Agent"] = WAP_UA, Cookie = Auth.cookie_header() },
      timeout_ms = 25000,
    })
    if not r or not r.ok or not r.body or r.body == "" then
      return nil, (r and r.error) or "no_response"
    end
    local raw = r.body
    r.body = nil
    downloaded = #raw
    -- 先在 GBK 字节上定位正文, 只转换 body 段
    local ul = raw:find('<ul class="content_ul">', 1, true)
    local li = ul and raw:find("<li>", ul, true) or nil
    local eli = li and raw:find("</li>", li + 4, true) or nil
    if not li or not eli then
      local err = "no_body"
      if raw:find("VIP", 1, true) or raw:find("vip", 1, true)
          or raw:find("/vip/", 1, true) or raw:find("购买", 1, true) then
        err = "vip_unpaid"
      end
      if raw:find("登录", 1, true) or raw:find("登入", 1, true) then err = "login_expired" end
      raw = nil
      if collectgarbage then collectgarbage("collect") end
      if err == "login_expired" and type(Auth) == "table" then Auth.clear() end
      return nil, err
    end
    local body_gbk = raw:sub(li + 4, eli - 1)
    raw = nil
    if collectgarbage then collectgarbage("collect") end
    local utf8, gerr = Gbk.convert(body_gbk)
    body_gbk = nil
    if type(Gbk) == "table" and type(Gbk.unload) == "function" then Gbk.unload() end
    if collectgarbage then collectgarbage("collect") end
    if not utf8 then return nil, gerr or "gbk" end
    local text, err = clean_wap_fragment(utf8)
    utf8 = nil
    if collectgarbage then collectgarbage("collect") end
    if not text then
      if err == "login_expired" and type(Auth) == "table" then Auth.clear() end
      return nil, err
    end
    local okw = false
    if type(fs) == "table" and type(fs.replaceFile) == "function" then
      okw = fs.replaceFile(relPath, text)
    elseif type(fs) == "table" and type(fs.writeFile) == "function" then
      okw = fs.writeFile(relPath, text)
    end
    local n = #text
    text = nil
    if collectgarbage then collectgarbage("collect") end
    if not okw then return nil, "write_failed" end
    return n, nil, { bytes = n, downloaded = downloaded, source = "wap_net" }
  end

  if downloaded < 1 then return nil, "empty_response" end

  local li = find_bytes(tmp, "<li>", find_bytes(tmp, '<ul class="content_ul">', 0) or 0)
  local eli = li and find_bytes(tmp, "</li>", li + 4) or nil
  if not li or not eli or eli <= li + 4 then
    local err = "no_body"
    if file_has_ascii(tmp, "VIP") or file_has_ascii(tmp, "vip") then err = "vip_unpaid" end
    -- 「登录」为 UTF-8, WAP 页是 GBK; 用 ASCII 片段粗判
    if file_has_ascii(tmp, "login") then err = "login_expired" end
    if type(fs.writeFile) == "function" then pcall(fs.writeFile, tmp, "") end
    if err == "login_expired" and type(Auth) == "table" then Auth.clear() end
    return nil, err
  end
  local body_off = li + 4
  local body_len = eli - body_off
  if body_len < 1 then return nil, "no_body" end

  -- 正文 GBK 通常远小于整页; 若 headroom 够, 一次读出再 rope 转换更简单可靠
  local headroom = 200000
  if type(sys) == "table" and type(sys.memInfo) == "function" then
    local mi = sys.memInfo()
    if type(mi) == "table" then headroom = tonumber(mi.lua_headroom or headroom) or headroom end
  end
  local text, err
  if body_len < math.floor(headroom / 3) and body_len < 180000
      and type(fs.readRange) == "function" then
    local body_gbk = fs.readRange(tmp, body_off, body_len)
    if type(fs.writeFile) == "function" then pcall(fs.writeFile, tmp, "") end
    if type(body_gbk) ~= "string" or body_gbk == "" then return nil, "no_body" end
    local utf8, gerr = Gbk.convert(body_gbk)
    body_gbk = nil
    if type(Gbk) == "table" and type(Gbk.unload) == "function" then Gbk.unload() end
    if collectgarbage then collectgarbage("collect") end
    if not utf8 then return nil, gerr or "gbk" end
    text, err = clean_wap_fragment(utf8)
    utf8 = nil
  else
    -- 超大正文: 先 GBK 段转文件, 再读出清洗 (清洗后通常更小)
    local tmp_u = "cache/_wap_u_" .. chapterId .. ".txt"
    local n, gerr = Gbk.convert_range_to_file(tmp, body_off, body_len, tmp_u)
    if type(fs.writeFile) == "function" then pcall(fs.writeFile, tmp, "") end
    if type(Gbk) == "table" and type(Gbk.unload) == "function" then Gbk.unload() end
    if collectgarbage then collectgarbage("collect") end
    if not n then return nil, gerr or "gbk" end
    local utf8 = (type(fs.readFile) == "function" and fs.readFile(tmp_u)) or nil
    if type(fs.writeFile) == "function" then pcall(fs.writeFile, tmp_u, "") end
    if type(utf8) ~= "string" then return nil, "gbk_read" end
    text, err = clean_wap_fragment(utf8)
    utf8 = nil
  end
  if collectgarbage then collectgarbage("collect") end
  if not text then
    if err == "login_expired" and type(Auth) == "table" then Auth.clear() end
    return nil, err
  end
  local okw = false
  if type(fs) == "table" and type(fs.replaceFile) == "function" then
    okw = fs.replaceFile(relPath, text)
  elseif type(fs) == "table" and type(fs.writeFile) == "function" then
    okw = fs.writeFile(relPath, text)
  end
  local n = #text
  text = nil
  if collectgarbage then collectgarbage("collect") end
  if not okw then return nil, "write_failed" end
  return n, nil, { bytes = n, downloaded = downloaded, source = "wap_dl" }
end

-- 晋江 isvip: "0" 免费; "1"/"2" 均为 VIP (接口常见 2)。
function Api.flag_is_vip(v)
  if v == true then return true end
  local s = tostring(v or "")
  if s == "" or s == "0" or s == "false" or s == "nil" then return false end
  return true
end

-- 轻量探测 androidapi 章节门控 (避免 VIP 空 content 被当成网络超时)。
-- 绝不投影 content 进 Lua：512KB 堆上整章 content 会直接 OOM。
-- 返回: "free" | "vip_login" | "vip_gate" | "empty" | "error", detail?
function Api.probe_chapter_gate(bookId, chapterId)
  chapterId = tostring(chapterId or "")
  if chapterId == "" then return "error", "no_id" end
  if type(dl) ~= "table" or type(dl.jsonGet) ~= "function" then
    return "error", "unsupported"
  end
  local url = API_HOST .. "/androidapi/chapterContent?novelId=" .. tostring(bookId)
    .. "&chapterId=" .. chapterId
  -- Only gate metadata. content must stay on SD via free_try file_out.
  local res, err = dl.jsonGet({
    url = url,
    headers = app_headers(),
    path = {},
    fields = { "message", "code", "chapterName" },
    max_bytes = 8 * 1024,
    timeout_ms = 12000,
  })
  if type(res) ~= "table" then
    return "error", tostring(err or "probe_failed")
  end
  local row = res[1] or res
  if type(row) ~= "table" then return "error", "bad_probe" end
  local message = tostring(row.message or "")
  local code = tostring(row.code or "")
  -- code 1004 + 登入/VIP 文案 (未登录或需购买)
  local vipish = (code == "1004")
    or message:find("VIP", 1, true) or message:find("vip", 1, true)
    or message:find("登入", 1, true) or message:find("登录", 1, true)
    or message:find("购买", 1, true) or message:find("晋江币", 1, true)
  if vipish then
    if type(Auth) == "table" and Auth.has() then
      -- 已登录仍无 content: 多半未购买 (或 cookie 失效, WAP 再分辨)
      return "vip_gate", message ~= "" and message or "VIP"
    end
    return "vip_login", message ~= "" and message or "VIP login required"
  end
  -- No VIP markers and no content field: treat as free; caller re-does file_out.
  if message == "" and (code == "" or code == "0" or code == "200") then
    return "free", nil
  end
  return "empty", message ~= "" and message or "empty"
end

-- 系统 progressive loader 声明 (免费章 JSON content; 固件 loader API)。
function Api.chapter_loader_spec(bookId, ch, relPath, openMeta)
  local chapterId = tostring((ch and ch.chapterUid) or "")
  if chapterId == "" or type(relPath) ~= "string" or relPath == "" then return nil end
  if ch and Api.flag_is_vip(ch.isvip) then return nil end  -- VIP 仍走 WAP hop
  openMeta = openMeta or {}
  return {
    url = API_HOST .. "/androidapi/chapterContent?novelId=" .. tostring(bookId)
      .. "&chapterId=" .. chapterId,
    headers = app_headers(),
    out = relPath,
    extract = { kind = "json_field", path = {}, field = "content" },
    -- Open only when body is complete (pendingComplete early open left the
    -- native reader stuck on "加载中…" then exit if final_open raced/failed).
    early_bytes = 0,
    max_bytes = 4 * 1024 * 1024,
    timeout_ms = 45000,
    title = openMeta.title or "",
    bookId = tostring(bookId or ""),
    chapterUid = chapterId,
    progressKey = openMeta.progressKey
      or ("jjwxc:" .. tostring(bookId) .. ":" .. chapterId),
    providerId = "jjwxc",
    chapterIndex = openMeta.chapterIndex or 0,
  }
end

function Api.toc_loader_spec(bookId, relPath, openMeta)
  openMeta = openMeta or {}
  return {
    url = API_HOST .. "/androidapi/chapterList?novelId=" .. tostring(bookId)
      .. "&more=0&whole=1",
    headers = app_headers(),
    out = relPath,
    path = { "chapterlist" },
    fields = { "chapterid", "chaptername", "chaptertype", "isvip", "islock" },
    -- 先开前几屏目录, 后台继续增长行数
    early_rows = 24,
    max_bytes = 8 * 1024 * 1024,
    timeout_ms = 45000,
    bookId = tostring(bookId or ""),
    title = openMeta.title or "",
    providerId = "jjwxc",
    currentIndex = openMeta.currentIndex or 0,
    uidField = 0,
    titleField = 1,
    -- isvip 列 (0-based index 3): 宿主 native TOC 据此显示 VIP 标记
    vipField = 3,
  }
end

-- 协作式章节拉取 (每 tick 一步, 中间可刷新 UI 字节数)。
-- step: free_try → vip_dl → vip_cvt → done
function Api.chapter_fetch_begin(bookId, ch)
  return {
    bookId = tostring(bookId or ""),
    chapterUid = tostring((ch and ch.chapterUid) or ""),
    isvip = Api.flag_is_vip(ch and ch.isvip),
    step = "free_try",
    tmp = nil,
    bytes = 0,
    downloaded = 0,
    probed = false,
  }
end

local function fmt_kb(n)
  n = tonumber(n) or 0
  if n < 1024 then return tostring(math.floor(n)) .. "B" end
  return string.format("%.1fKB", n / 1024)
end

local function classify_wap_fail(tmp)
  -- ASCII markers in GBK HTML (VIP page never has content_ul body).
  if file_has_ascii(tmp, "/vip/") or file_has_ascii(tmp, "VIP")
      or file_has_ascii(tmp, "vip") then
    return "vip_unpaid"
  end
  if file_has_ascii(tmp, "login") or file_has_ascii(tmp, "Login") then
    return "login_expired"
  end
  return "no_body"
end

-- 返回: "busy", status_hint, info? | "done", size, info? | "error", err
function Api.chapter_fetch_step(job, relPath)
  if type(job) ~= "table" then return "error", "no_job" end
  if job.cancelled then return "error", "cancelled" end
  if type(relPath) ~= "string" or relPath == "" then return "error", "no_path" end
  local bookId = job.bookId
  local chapterId = job.chapterUid
  if bookId == "" or chapterId == "" then return "error", "no_id" end

  if job.step == "free_try" then
    if job.isvip then
      job.step = "vip_gate"
      return "busy", "VIP 章节…", nil
    end
    if type(dl) ~= "table" or type(dl.jsonGet) ~= "function" then
      job.step = "vip_gate"
      return "busy", "探测章节…", nil
    end
    local url = API_HOST .. "/androidapi/chapterContent?novelId=" .. tostring(bookId)
      .. "&chapterId=" .. chapterId
    local res, jerr = dl.jsonGet({
      url = url,
      headers = app_headers(),
      path = {},
      fields = { "content" },
      file_out = relPath,
      max_bytes = 4 * 1024 * 1024,
      timeout_ms = 30000,
    })
    if type(res) == "table" and res.ok and tonumber(res.size or 0) > 0 then
      local n = tonumber(res.size)
      job.bytes = n
      job.step = "done"
      if Storage and Storage.mark_chapter_complete then
        pcall(Storage.mark_chapter_complete, bookId, chapterId)
      end
      return "done", n, { bytes = n, source = "jsonGet" }
    end
    jerr = tostring(jerr or "")
    -- 空 content / 投影失败: 先短探测 message/code，避免 VIP 被当成 30s 超时
    if jerr == "empty_file_value" or jerr == "file_out_requires_one_scalar"
        or jerr == "unsupported" or jerr == ""
        or jerr:find("timeout", 1, true) or jerr:find("empty", 1, true) then
      job.step = "vip_gate"
      return "busy", "识别章节类型…", nil
    end
    -- TLS/连接抖动 (http_request_failed) 在阅读中预取很常见：先看系统 free_heap——
    -- 阅读中 ~50KB 时 TLS 分配失败正是 http_request_failed 的真凶，此时重试也白搭，
    -- 直接报 low_heap 走 idle 8s Error 节流后重来；堆充足才 soft_release +
    -- collectgarbage 后重试（最多 2 次），仍失败再 error（不再盲走 WAP）。
    local http_ish = (jerr == "http_request_failed" or jerr:find("http_request", 1, true)
      or jerr == "http_begin_failed")
    if http_ish then
      if type(sys) == "table" and type(sys.memInfo) == "function" then
        local mi = sys.memInfo()
        local heap = type(mi) == "table" and tonumber(mi.heap_free or 0) or 0
        if heap > 0 and heap < 90000 then
          return "error", "low_heap"
        end
      end
      local tries = tonumber(job.http_retry) or 0
      if tries < 2 then
        job.http_retry = tries + 1
        if type(soft_release_mem) == "function" then
          pcall(soft_release_mem, "prefetch_retry")
        end
        if collectgarbage then
          collectgarbage("collect")
          collectgarbage("collect")
        end
        return "busy", "网络重试…", nil
      end
    end
    -- 明确网络/HTTP 错误: 不再盲走 WAP
    return "error", jerr ~= "" and jerr or "chapter_file_failed"
  end

  if job.step == "vip_gate" then
    if job.cancelled then return "error", "cancelled" end
    job.probed = true
    local gate, detail = Api.probe_chapter_gate(bookId, chapterId)
    if job.cancelled then return "error", "cancelled" end
    if gate == "free" then
      -- Probe never carries body (OOM). One more file_out free hop only.
      if not job.free_retry then
        job.free_retry = true
        job.step = "free_try"
        return "busy", "重试免费正文…", nil
      end
      return "error", "empty_content"
    end
    if gate == "vip_login" then
      return "error", "login_required"
    end
    if gate == "vip_gate" then
      job.isvip = true
      if type(Auth) ~= "table" or not Auth.has() then
        return "error", "login_required"
      end
      job.step = "vip_dl"
      return "busy", "VIP 章 · 校验购买…", nil
    end
    if gate == "empty" or gate == "error" then
      -- 仍可能是 VIP 或需 WAP; 已登录则试 WAP，否则报错
      if type(Auth) == "table" and Auth.has() then
        job.step = "vip_dl"
        return "busy", "尝试 WAP 正文…", nil
      end
      if gate == "error" and detail and tostring(detail):find("timeout", 1, true) then
        return "error", "timeout"
      end
      return "error", (detail and detail ~= "" and detail) or "empty_content"
    end
    job.step = "vip_dl"
    return "busy", "下载 VIP 页…", nil
  end

  if job.step == "vip_dl" then
    if job.cancelled then return "error", "cancelled" end
    if type(Auth) ~= "table" or not Auth.has() then
      return "error", "login_required"
    end
    local url = WAP_HOST .. "/book2/" .. tostring(bookId) .. "/" .. tostring(chapterId)
    local tmp = "cache/_wap_" .. tostring(bookId) .. "_" .. chapterId .. ".bin"
    job.tmp = tmp
    if type(dl) == "table" and type(dl.download) == "function" then
      local ok, res = dl.download({
        url = url,
        headers = { ["User-Agent"] = WAP_UA, Cookie = Auth.cookie_header() },
        path = tmp,
        max_bytes = 2 * 1024 * 1024,
        timeout_ms = 25000,
      })
      if not ok then
        local es = tostring((type(res) == "string" and res) or "wap_download")
        if es:find("timeout", 1, true) then
          -- 超时前若已有部分 HTML 且含 VIP 购买页，直接判定
          if type(fs) == "table" and type(fs.fileSize) == "function"
              and (tonumber(fs.fileSize(tmp) or 0) or 0) > 200 then
            local ce = classify_wap_fail(tmp)
            if ce == "vip_unpaid" or ce == "login_expired" then
              pcall(fs.writeFile, tmp, "")
              return "error", ce
            end
          end
        end
        return "error", es
      end
      local n = tonumber(type(res) == "table" and res.size or 0) or 0
      if n < 1 and type(fs) == "table" and type(fs.fileSize) == "function" then
        n = tonumber(fs.fileSize(tmp) or 0) or 0
      end
      if n < 1 then return "error", "empty_response" end
      job.downloaded = n
      job.step = "vip_cvt"
      return "busy", "已下 " .. fmt_kb(n) .. " · 解析…", { downloaded = n }
    end
    -- 无 dl.download: 整段进 fetch_vip_chapter_to_file (内部仍尽量只转 body)
    local n, err, info = Api.fetch_vip_chapter_to_file(bookId, chapterId, relPath)
    if n then
      job.bytes = n
      job.downloaded = (info and info.downloaded) or n
      job.step = "done"
      if Storage and Storage.mark_chapter_complete then
        pcall(Storage.mark_chapter_complete, bookId, chapterId)
      end
      return "done", n, info
    end
    return "error", err or "vip_failed"
  end

  if job.step == "vip_cvt" then
    local tmp = job.tmp
    if type(tmp) ~= "string" or tmp == "" then return "error", "no_tmp" end
    local li = find_bytes(tmp, "<li>", find_bytes(tmp, '<ul class="content_ul">', 0) or 0)
    local eli = li and find_bytes(tmp, "</li>", li + 4) or nil
    if not li or not eli or eli <= li + 4 then
      local err = classify_wap_fail(tmp)
      if type(fs) == "table" and type(fs.writeFile) == "function" then pcall(fs.writeFile, tmp, "") end
      if err == "login_expired" and type(Auth) == "table" then Auth.clear() end
      return "error", err
    end
    local body_off = li + 4
    local body_len = eli - body_off
    local headroom = 200000
    if type(sys) == "table" and type(sys.memInfo) == "function" then
      local mi = sys.memInfo()
      if type(mi) == "table" then headroom = tonumber(mi.lua_headroom or headroom) or headroom end
    end
    local text, err
    if body_len < math.floor(headroom / 3) and body_len < 180000
        and type(fs) == "table" and type(fs.readRange) == "function" then
      local body_gbk = fs.readRange(tmp, body_off, body_len)
      if type(fs.writeFile) == "function" then pcall(fs.writeFile, tmp, "") end
      if type(body_gbk) ~= "string" or body_gbk == "" then return "error", "no_body" end
      local utf8, gerr = Gbk.convert(body_gbk)
      body_gbk = nil
      if type(Gbk) == "table" and type(Gbk.unload) == "function" then Gbk.unload() end
      if collectgarbage then collectgarbage("collect") end
      if not utf8 then return "error", gerr or "gbk" end
      text, err = clean_wap_fragment(utf8)
      utf8 = nil
    else
      local tmp_u = "cache/_wap_u_" .. chapterId .. ".txt"
      local n, gerr = Gbk.convert_range_to_file(tmp, body_off, body_len, tmp_u)
      if type(fs) == "table" and type(fs.writeFile) == "function" then pcall(fs.writeFile, tmp, "") end
      if type(Gbk) == "table" and type(Gbk.unload) == "function" then Gbk.unload() end
      if collectgarbage then collectgarbage("collect") end
      if not n then return "error", gerr or "gbk" end
      local utf8 = (type(fs) == "table" and type(fs.readFile) == "function" and fs.readFile(tmp_u)) or nil
      if type(fs) == "table" and type(fs.writeFile) == "function" then pcall(fs.writeFile, tmp_u, "") end
      if type(utf8) ~= "string" then return "error", "gbk_read" end
      text, err = clean_wap_fragment(utf8)
      utf8 = nil
    end
    if collectgarbage then collectgarbage("collect") end
    if not text then return "error", err or "no_body" end
    local okw = false
    if type(fs) == "table" and type(fs.replaceFile) == "function" then
      okw = fs.replaceFile(relPath, text)
    elseif type(fs) == "table" and type(fs.writeFile) == "function" then
      okw = fs.writeFile(relPath, text)
    end
    local n = #text
    text = nil
    if collectgarbage then collectgarbage("collect") end
    if not okw then return "error", "write_failed" end
    job.bytes = n
    job.step = "done"
    if Storage and Storage.mark_chapter_complete then
      pcall(Storage.mark_chapter_complete, bookId, chapterId)
    end
    return "done", n, { bytes = n, downloaded = job.downloaded, source = "wap_hop" }
  end

  if job.step == "done" then
    return "done", job.bytes or 0, { bytes = job.bytes or 0 }
  end
  return "error", "bad_step"
end

-- 免费章正文直接写 SD (dl.jsonGet file_out): 正文不占 Lua 堆。
-- VIP (empty content) 自动走 WAP → 文件, 仍不进 Lua 堆。
function Api.fetch_chapter_to_file(bookId, ch, relPath)
  local chapterId = tostring((ch and ch.chapterUid) or "")
  if chapterId == "" or type(relPath) ~= "string" or relPath == "" then
    return nil, "no_id"
  end
  local vip = Api.flag_is_vip(ch and ch.isvip)
  if type(dl) == "table" and type(dl.jsonGet) == "function" and not vip then
    local url = API_HOST .. "/androidapi/chapterContent?novelId=" .. tostring(bookId)
      .. "&chapterId=" .. chapterId
    local res, jerr = dl.jsonGet({
      url = url,
      headers = app_headers(),
      path = {},
      fields = { "content" },
      file_out = relPath,
      max_bytes = 4 * 1024 * 1024,
      timeout_ms = 30000,
    })
    if type(res) == "table" and res.ok and tonumber(res.size or 0) > 0 then
      return tonumber(res.size), nil, { bytes = tonumber(res.size), source = "jsonGet" }
    end
    jerr = tostring(jerr or "")
    if jerr == "empty_file_value" or jerr == "file_out_requires_one_scalar"
        or jerr == "" or jerr == "unsupported" then
      local gate, detail = Api.probe_chapter_gate(bookId, chapterId)
      if gate == "vip_login" then return nil, "login_required" end
      if gate == "vip_gate" then
        if type(Auth) ~= "table" or not Auth.has() then return nil, "login_required" end
        return Api.fetch_vip_chapter_to_file(bookId, chapterId, relPath)
      end
      if gate == "free" and type(detail) == "string" and #detail > 0 then
        local okw = type(fs) == "table" and (
          (type(fs.replaceFile) == "function" and fs.replaceFile(relPath, detail))
          or (type(fs.writeFile) == "function" and fs.writeFile(relPath, detail)))
        if okw then return #detail, nil, { bytes = #detail, source = "probe" } end
      end
    end
    if jerr ~= "" and jerr ~= "unsupported" and not jerr:find("empty", 1, true) then
      return nil, jerr
    end
  end
  if vip then
    if type(Auth) ~= "table" or not Auth.has() then return nil, "login_required" end
    return Api.fetch_vip_chapter_to_file(bookId, chapterId, relPath)
  end
  -- 最后: 显式尝试 WAP (androidapi 无 content 的常见 VIP)
  if type(Auth) == "table" and Auth.has() then
    local n, err, info = Api.fetch_vip_chapter_to_file(bookId, chapterId, relPath)
    if n then return n, nil, info end
    if err == "login_required" or err == "vip_unpaid" or err == "login_expired" then
      return nil, err
    end
  end
  return nil, "unsupported"
end

-- 单章正文 (测试/旧路径/需 sayBody)。生产 ContentProvider 走 chapter_fetch_step 直写 SD。
function Api.fetch_chapter_text(bookId, ch)
  local chapterId = tostring((ch and ch.chapterUid) or "")
  if chapterId == "" then return nil, "no_chapter_id" end

  if not Api.flag_is_vip(ch and ch.isvip) then
    local url = API_HOST .. "/androidapi/chapterContent?novelId=" .. tostring(bookId)
      .. "&chapterId=" .. chapterId
    local r = net.request("GET", url, { headers = app_headers(), timeout_ms = 25000 })
    if r and r.ok and r.body then
      local doc, derr = decode_or_err(r.body)
      r.body = nil
      if collectgarbage then collectgarbage("collect") end
      if doc then
        if doc.content and doc.content ~= "" then
          local text = doc.content
          local say = Api.jstr(doc.sayBody)
          if say ~= "" then
            text = text .. "\n\n【作者有话说】\n" .. say
          end
          return text, nil
        end
        local code = tostring(doc.code or doc.errCode or "")
        if code == "1004" or (type(doc.message) == "string"
            and doc.message:find("VIP", 1, true)) then
          return Api.fetch_vip_chapter_wap(bookId, chapterId)
        end
        local msg = type(doc.message) == "string" and doc.message or ""
        if code == "2016" then return nil, "ua_rejected" end
        if msg ~= "" then return nil, msg end
        return nil, "empty_content"
      end
      return nil, derr or "json"
    end
    return nil, (r and r.error) or "no_response"
  end

  return Api.fetch_vip_chapter_wap(bookId, chapterId)
end

-- VIP 章: 文本路径 (兼容); 内部尽量只转正文 GBK 段。
function Api.fetch_vip_chapter_wap(bookId, chapterId)
  if type(Auth) ~= "table" or not Auth.has() then
    return nil, "login_required"
  end
  local url = WAP_HOST .. "/book2/" .. tostring(bookId) .. "/" .. tostring(chapterId)
  local r = net.request("GET", url, {
    headers = { ["User-Agent"] = WAP_UA, Cookie = Auth.cookie_header() },
    timeout_ms = 25000,
  })
  if not r or not r.ok then return nil, (r and r.error) or "no_response" end
  local raw = r.body
  r.body = nil
  if not raw or raw == "" then return nil, "empty_response" end
  -- 优先只转换 <li> 正文 GBK, 避免整页 UTF-8 占堆
  local ul = raw:find('<ul class="content_ul">', 1, true)
  local li = ul and raw:find("<li>", ul, true) or nil
  local eli = li and raw:find("</li>", li + 4, true) or nil
  if li and eli and eli > li + 4 then
    local body_gbk = raw:sub(li + 4, eli - 1)
    raw = nil
    if collectgarbage then collectgarbage("collect") end
    local utf8, gerr = Gbk.convert(body_gbk)
    body_gbk = nil
    if type(Gbk) == "table" and type(Gbk.unload) == "function" then Gbk.unload() end
    if collectgarbage then collectgarbage("collect") end
    if not utf8 then return nil, gerr or "gbk" end
    local text, err = clean_wap_fragment(utf8)
    utf8 = nil
    if collectgarbage then collectgarbage("collect") end
    if not text then return nil, err end
    return text, nil
  end
  local utf8_html, gerr = Gbk.convert(raw)
  raw = nil
  if collectgarbage then collectgarbage("collect") end
  if not utf8_html then return nil, gerr or "gbk" end
  local text, err = extract_wap_body(utf8_html)
  utf8_html = nil
  if type(Gbk) == "table" and type(Gbk.unload) == "function" then Gbk.unload() end
  if collectgarbage then collectgarbage("collect") end
  if not text then
    if err == "login_expired" then
      if type(Auth) == "table" then Auth.clear() end
    end
    return nil, err
  end
  return text, nil
end

return Api

