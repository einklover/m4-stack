-- ContentProvider: UI-first 章节正文流水线。
--   ui_first → probe(缓存) → fetch(系统 loader 分片 / VIP hop) → handoff
-- 特殊结果: "login" = VIP 章未登录。
--
-- 速度策略:
--   1) 缓存命中立即 openText (原生 progressive 分页)
--   2) 免费章: loader.chapter 流式写盘 + early openText (约 1–2KB 正文即可首屏)
--   3) VIP: 下载 hop / 转码 hop 分离, 中间刷新字节
--   4) 打开后 provider_pump 预取下一章
-- TLS: 记录流解密, 不必下完整包; 握手+首包仍占 1 次 pump 切片 (~1–3s)
ContentProvider = {}

local SPINNER = { "·", "··", "···", "····" }

local function fmt_bytes(n)
  n = tonumber(n) or 0
  if n < 1 then return "0B" end
  if n < 1024 then return tostring(math.floor(n)) .. "B" end
  if n < 1024 * 1024 then
    local kb = n / 1024
    if kb < 10 then return string.format("%.1fKB", kb) end
    return string.format("%dKB", math.floor(kb + 0.5))
  end
  return string.format("%.1fMB", n / (1024 * 1024))
end

local function elapsed_s(job)
  local t0 = tonumber(job and job.t0) or 0
  if t0 <= 0 or type(sys) ~= "table" or type(sys.millis) ~= "function" then return 0 end
  local d = sys.millis() - t0
  if d < 0 then d = 0 end
  return math.floor(d / 1000)
end

local function spinner(job)
  local sec = elapsed_s(job)
  return SPINNER[(sec % #SPINNER) + 1]
end

local function phase_label(job, font_ok)
  if not job then return font_ok and "加载中…" or "loading…" end
  local lp = job.loader_phase
  if lp == "connecting" then
    return font_ok and "连接服务器 (TLS)…" or "TLS connect…"
  elseif lp == "streaming" then
    return font_ok and "流式下载正文…" or "streaming body…"
  elseif lp == "early" then
    return font_ok and "首屏已打开 · 后台续传…" or "early open · background…"
  elseif lp == "cache" then
    return font_ok and "缓存命中…" or "cache hit…"
  elseif lp == "done" then
    return font_ok and "下载完成" or "done"
  elseif lp == "failed" then
    return font_ok and "下载失败" or "failed"
  end
  if type(job.status) == "string" and job.status ~= "" then
    return job.status
  end
  local ph = job.phase or ""
  if ph == "probe" then
    return font_ok and "检查本地缓存…" or "checking cache…"
  elseif ph == "fetch" then
    return font_ok and "下载章节…" or "downloading…"
  elseif ph == "write" then
    return font_ok and "写入缓存…" or "writing cache…"
  elseif ph == "handoff" then
    return font_ok and "打开阅读器…" or "opening reader…"
  end
  return font_ok and "正在加载章节…" or "loading chapter…"
end

function ContentProvider.fmt_bytes(n)
  return fmt_bytes(n)
end

function ContentProvider.begin(opts)
  opts = opts or {}
  local isvip = opts.isvip
  if type(Api) == "table" and type(Api.flag_is_vip) == "function" then
    isvip = Api.flag_is_vip(isvip)
  else
    isvip = isvip and true or false
  end
  return {
    phase = "ui_first",
    ui_committed = false,
    need_paint = true,
    bookId = opts.bookId or "",
    chapterUid = opts.chapterUid or "",
    title = opts.title or "",
    path = opts.path or "",
    prefer = opts.prefer,
    isvip = isvip and true or false,
    chapterIndex = opts.chapterIndex or 0,
    pct = 0,
    gen = 1,
    t0 = (sys and sys.millis and sys.millis()) or 0,
    status = opts.status or "",
    bytes = 0,
    downloaded = 0,
    rows = 0,
    loader_phase = "",
    fetch = nil,
    pending_text = nil,
    result = nil,
    fail_code = nil,
    fail_title = nil,
    fail_body = nil,
    fail_action = nil,
  }
end

function ContentProvider.loading_body(job, font_ok)
  if not job then
    return font_ok and "正在加载章节…" or "loading chapter…"
  end
  local sec = elapsed_s(job)
  local lines = {}
  -- Always show a step line so e-ink is not a static blank during TLS.
  lines[#lines + 1] = spinner(job) .. " " .. phase_label(job, font_ok)
  local ph = job.phase or ""
  if ph == "probe" then
    lines[#lines + 1] = font_ok and "步骤 1/3 · 查缓存" or "step 1/3 cache"
  elseif ph == "fetch" then
    if job.sys_loader == true then
      lines[#lines + 1] = font_ok and "步骤 2/3 · 流式写盘" or "step 2/3 stream"
      lines[#lines + 1] = font_ok
          and "约 1–2KB 正文即可打开首屏"
          or "early open ~1–2KB body"
    else
      lines[#lines + 1] = font_ok and "步骤 2/3 · 下载/转码" or "step 2/3 hop"
    end
    if (job.downloaded or 0) > 0 or (job.bytes or 0) > 0 then
      local n = math.max(tonumber(job.downloaded) or 0, tonumber(job.bytes) or 0)
      lines[#lines + 1] = (font_ok and "已接收 " or "recv ") .. fmt_bytes(n)
    else
      lines[#lines + 1] = font_ok and "等待首包…" or "waiting first byte…"
    end
    if (job.rows or 0) > 0 then
      lines[#lines + 1] = (font_ok and "已解析 " or "rows ") .. tostring(job.rows)
    end
  elseif ph == "write" then
    lines[#lines + 1] = font_ok and "步骤 2/3 · 写缓存" or "step 2/3 write"
    if (job.bytes or 0) > 0 then
      lines[#lines + 1] = fmt_bytes(job.bytes)
    end
  elseif ph == "handoff" then
    lines[#lines + 1] = font_ok and "步骤 3/3 · 打开阅读器" or "step 3/3 open"
    if (job.bytes or 0) > 0 then
      lines[#lines + 1] = (font_ok and "已缓存 " or "cached ") .. fmt_bytes(job.bytes)
    end
  end
  lines[#lines + 1] = (font_ok and "已用时 " or "elapsed ") .. tostring(sec) .. "s"
  return table.concat(lines, "\n")
end

function ContentProvider.status_line(job, font_ok)
  local base = phase_label(job, font_ok)
  local bits = { spinner(job) .. " " .. base }
  local n = math.max(tonumber(job and job.downloaded) or 0, tonumber(job and job.bytes) or 0)
  if n > 0 then
    bits[#bits + 1] = fmt_bytes(n)
  end
  local sec = elapsed_s(job)
  if sec >= 1 then
    bits[#bits + 1] = tostring(sec) .. "s"
  end
  return table.concat(bits, " · ")
end

function ContentProvider.request_paint(job, status)
  if not job then return end
  if status then job.status = status end
  job.need_paint = true
end

function ContentProvider.should_paint_only(job)
  if not job then return false end
  return not job.ui_committed
end

function ContentProvider.mark_painted(job)
  if not job then return end
  job.ui_committed = true
  job.need_paint = false
  if job.phase == "ui_first" then
    job.phase = "probe"
  end
end

local function fail(job, code, title, body, action)
  job.phase = "fail"
  job.result = "fail"
  job.fail_code = code
  job.fail_title = title
  job.fail_body = body
  job.fail_action = action or "retry_chapter"
  job.pending_text = nil
  job.fetch = nil
end

-- 一步协作后端。仅当非 should_paint_only 时调用。
-- 返回: "continue" | "done" | "fail" | "login"
function ContentProvider.step(job, deps)
  if not job or not deps then return "fail" end
  if job.cancelled or job.phase == "cancelled" then
    return "fail"
  end
  local font_ok = deps.font_ok and true or false
  local Storage = deps.Storage
  local Api = deps.Api
  local log = deps.log or function() end

  if job.phase == "ui_first" then
    job.phase = "probe"
  end

  if job.phase == "probe" then
    local path = job.path
    if path == "" and Storage and Storage.chapter_path then
      path = Storage.chapter_path(job.bookId, job.chapterUid)
      job.path = path
    end
    -- Only fully-finished chapters (.ok). Partial progressive leftovers are NOT cache.
    local complete = Storage and Storage.chapter_complete
        and Storage.chapter_complete(job.bookId, job.chapterUid)
    local fsz = Storage and Storage.chapter_file_size
        and Storage.chapter_file_size(job.bookId, job.chapterUid) or nil
    if complete and fsz and fsz > 0 then
      job.bytes = fsz
      job.phase = "handoff"
      job.status = font_ok
          and ("缓存命中 " .. fmt_bytes(fsz) .. " · 打开…")
          or ("cache hit " .. fmt_bytes(fsz))
      log(string.format("[JJ] cache_hit t=%s bytes=%s", tostring(sys.millis()), tostring(fsz)))
      local ok, err = deps.open_native_reader(path, job.title, job.bookId, job.chapterUid)
      if ok then
        job.phase = "done"
        job.result = "handoff"
        return "done"
      end
      fail(job, "E_HOST", font_ok and "需要固件支持" or "Firmware required",
        font_ok and ("原生阅读桥不可用: " .. tostring(err or "no_reader"))
          or ("native reader unavailable: " .. tostring(err or "no_reader")))
      return "fail"
    end
    -- Stale partial without .ok: clear so we never open previous half-body.
    if fsz and fsz > 0 and Storage.clear_chapter_cache then
      pcall(Storage.clear_chapter_cache, job.bookId, job.chapterUid)
    end
    -- 释放 GBK 表等, 为下载腾 headroom; 下一帧再拉网, 先让「下载」状态上屏
    if deps.soft_release_mem then pcall(deps.soft_release_mem, "chapter_fetch") end
    job.phase = "fetch"
    job.status = font_ok and "下载章节…" or "downloading…"
    job.sys_loader = nil
    if Api and type(Api.chapter_fetch_begin) == "function" then
      job.fetch = Api.chapter_fetch_begin(job.bookId, {
        chapterUid = job.chapterUid,
        isvip = job.isvip,
      })
    else
      job.fetch = { legacy = true, isvip = job.isvip }
    end
    log(string.format("[JJ] fetch_announce t=%s", tostring(sys.millis())))
    return "continue"
  end

  if job.phase == "fetch" then
    local online, nerr = true, nil
    if deps.ensure_network then
      online, nerr = deps.ensure_network()
    end
    if not online then
      local title = deps.net_error_title and deps.net_error_title(nerr) or "Network"
      local body = deps.net_error_body and deps.net_error_body(nerr) or tostring(nerr or "")
      fail(job, "E_NET", title,
        font_ok and ("本章未缓存\n" .. body) or ("not cached\n" .. body))
      return "fail"
    end

    local path = job.path
    if (not path or path == "") and Storage and Storage.chapter_path then
      path = Storage.chapter_path(job.bookId, job.chapterUid)
      job.path = path
    end

    -- 系统 progressive loader (免费章 JSON content 流式写盘 + early openText)
    -- begin 只装载任务不握手; 每 tick 一次 pump, 中间刷新 phase/bytes。
    if job.sys_loader ~= false and type(loader) == "table"
        and type(loader.chapter) == "function" and Api.chapter_loader_spec then
      if job.sys_loader == nil then
        local spec = Api.chapter_loader_spec(job.bookId, { chapterUid = job.chapterUid }, path, {
          title = job.title,
          chapterIndex = job.chapterIndex,
        })
        if not spec or job.isvip then
          job.sys_loader = false  -- VIP 等: 不走免费 content 流式
          if job.isvip then
            job.status = font_ok and "VIP 章节…" or "VIP chapter…"
          else
            job.status = font_ok and "改用分段下载…" or "hop download…"
          end
        else
          job.status = font_ok and "准备流式下载…" or "arm stream…"
          job.loader_phase = "connecting"
          local ok, err = loader.chapter(spec)
          if not ok then
            log(string.format("[JJ] loader.chapter fail %s", tostring(err)))
            job.sys_loader = false
            job.loader_phase = ""
            job.status = font_ok and "流式不可用 · 回退…" or "loader fallback…"
          else
            job.sys_loader = true
            job.status = font_ok and "连接服务器 (TLS)…" or "TLS connect…"
            -- 本 tick 只 arm, 下一帧再 pump, 让「连接中」先上屏
            return "continue"
          end
        end
      end
      if job.sys_loader == true then
        if job.cancelled then return "fail" end
        -- 连接切片可能要数秒; 下载切片短预算, 便于每秒刷新字节数
        if type(loader.pump) == "function" then
          local st0 = (type(loader.status) == "function" and loader.status()) or {}
          if st0.phase == "connecting" then
            loader.pump({ ms = 50, bytes = 1024 })  -- connect returns after handshake
          else
            loader.pump({ ms = 120, bytes = 24 * 1024 })
          end
        end
        if job.cancelled then return "fail" end
        local st = (type(loader.status) == "function" and loader.status()) or {}
        job.loader_phase = tostring(st.phase or job.loader_phase or "")
        if (st.bytes or 0) > 0 then
          job.bytes = st.bytes
          job.downloaded = st.bytes
        end
        if job.loader_phase == "connecting" then
          job.status = font_ok and "连接服务器 (TLS)…" or "TLS connect…"
        elseif job.loader_phase == "streaming" then
          if (job.bytes or 0) > 0 then
            job.status = (font_ok and "流式下载 " or "dl ") .. fmt_bytes(job.bytes)
          else
            job.status = font_ok and "等待首包…" or "waiting first byte…"
          end
        elseif job.loader_phase == "early" then
          job.status = (font_ok and "首屏已开 · 续传 " or "early · ")
              .. fmt_bytes(job.bytes or 0)
        end
        if st.error and st.error ~= "" and not st.early and not st.done then
          -- empty content often means VIP → fall back to hop path
          if tostring(st.error):find("empty", 1, true) or tostring(st.error):find("path", 1, true)
              or tostring(st.error):find("http", 1, true) then
            job.sys_loader = false
            job.loader_phase = ""
            log(string.format("[JJ] loader fail → hop fallback err=%s", tostring(st.error)))
            job.status = font_ok and ("回退下载 · " .. tostring(st.error)) or tostring(st.error)
          else
            fail(job, "E_BODY", font_ok and "正文失败" or "Chapter failed", tostring(st.error))
            return "fail"
          end
        elseif st.done then
          -- Body complete.  Prefer plugin open_native_reader (not only host
          -- queueOpen): if host handoff was lost/raced, returning "done" alone
          -- sets screen=native_reader while the e-ink still shows 加载中 forever.
          job.bytes = st.bytes or job.bytes
          job.loader_phase = "done"
          job.status = font_ok and "打开阅读器…" or "opening…"
          log(string.format("[JJ] loader_done bytes=%s → open_native",
            tostring(job.bytes)))
          if (job.bytes or 0) < 1 then
            fail(job, "E_BODY", font_ok and "正文失败" or "Chapter failed",
              font_ok and "正文为空" or "empty body")
            return "fail"
          end
          if Storage and Storage.mark_chapter_complete then
            pcall(Storage.mark_chapter_complete, job.bookId, job.chapterUid)
          end
          local path = job.path
          if (not path or path == "") and Storage and Storage.chapter_path then
            path = Storage.chapter_path(job.bookId, job.chapterUid)
            job.path = path
          end
          local ok, err = deps.open_native_reader(path, job.title, job.bookId, job.chapterUid)
          if ok then
            job.phase = "done"
            job.result = "handoff"
            return "done"
          end
          fail(job, "E_HOST", font_ok and "需要固件支持" or "Firmware required",
            font_ok and ("原生阅读桥不可用: " .. tostring(err or "no_reader"))
              or ("native reader unavailable: " .. tostring(err or "no_reader")))
          return "fail"
        elseif st.early then
          -- Keep pumping until done; do not leave loading on early alone.
          job.status = (font_ok and "下载中 " or "dl ")
              .. fmt_bytes(job.bytes or 0)
          return "continue"
        else
          return "continue"
        end
      end
    end

    -- 多 hop: 每 tick 一步, 中间刷新字节/阶段
    if job.fetch and not job.fetch.legacy and Api and type(Api.chapter_fetch_step) == "function" then
      log(string.format("[JJ] fetch_step t=%s sub=%s",
        tostring(sys.millis()), tostring(job.fetch.step or "?")))
      local st, payload, info = Api.chapter_fetch_step(job.fetch, path)
      if type(info) == "table" then
        if info.bytes then job.bytes = tonumber(info.bytes) or job.bytes end
        if info.downloaded then job.downloaded = tonumber(info.downloaded) or job.downloaded end
      end
      if st == "busy" then
        if type(payload) == "string" and payload ~= "" then
          job.status = payload
        else
          job.status = font_ok and "下载章节…" or "downloading…"
        end
        return "continue"
      end
      if st == "error" then
        local err = tostring(payload or "unknown")
        if err == "cancelled" or err:find("cancel", 1, true) then
          job.cancelled = true
          return "fail"
        end
        if err == "login_required" or err == "login_expired" then
          job.fetch = nil
          return "login"
        end
        local detail = err
        if #detail > 120 then detail = detail:sub(1, 117) .. "..." end
        local title = font_ok and "正文失败" or "Chapter failed"
        local action = "retry_chapter"
        local code = "E_BODY"
        if err == "vip_unpaid" then
          title = font_ok and "VIP 未购买" or "VIP unpaid"
          detail = font_ok
              and "该章节为 VIP，需在晋江 App 购买后阅读\n\n可点按底部返回章节列表"
              or "VIP unpaid — purchase in Jinjiang app\n\nTap to return to chapter list"
          action = "toc"
          code = "E_VIP"
        elseif err:find("timeout", 1, true) then
          title = font_ok and "加载超时" or "Timeout"
          detail = font_ok
              and "请求超时。若为 VIP 章，请确认已登录并已购买\n\n可返回章节列表重选"
              or "timeout — if VIP, check login/purchase"
          -- 超时也允许明确回目录（避免卡在失败页无出口）
          action = "toc"
        end
        fail(job, code, title, detail, action)
        return "fail"
      end
      -- done: payload = byte size
      local n = tonumber(payload) or job.bytes or 0
      job.bytes = n
      job.fetch = nil
      job.pending_text = nil
      job.phase = "handoff"
      job.status = font_ok
          and ("已缓存 " .. fmt_bytes(n) .. " · 打开…")
          or ("cached " .. fmt_bytes(n))
      if collectgarbage then collectgarbage("collect") end
      log(string.format("[JJ] fetch_done t=%s bytes=%s", tostring(sys.millis()), tostring(n)))
      return "continue"
    end

    -- Legacy 单步 (测试/旧 API)
    log(string.format("[JJ] fetch_begin t=%s legacy=1", tostring(sys.millis())))
    if Api.fetch_chapter_to_file and path then
      local n, ferr, info = Api.fetch_chapter_to_file(job.bookId, { chapterUid = job.chapterUid }, path)
      if type(info) == "table" then
        job.bytes = tonumber(info.bytes or n) or n
        job.downloaded = tonumber(info.downloaded or 0) or 0
      end
      if n and tonumber(n) > 0 then
        job.bytes = tonumber(n)
        job.phase = "handoff"
        job.status = font_ok
            and ("已缓存 " .. fmt_bytes(n) .. " · 打开…")
            or ("cached " .. fmt_bytes(n))
        job.pending_text = nil
        if collectgarbage then collectgarbage("collect") end
        return "continue"
      end
      ferr = tostring(ferr or "unknown")
      if ferr == "login_required" then return "login" end
      if ferr == "vip_unpaid" then
        fail(job, "E_VIP", font_ok and "VIP 未购买" or "VIP unpaid",
          font_ok and "该章节需购买后阅读\n\n点按返回章节列表" or "not purchased\n\nTap for list",
          "toc")
        return "fail"
      end
      if ferr ~= "unsupported" then
        local detail = ferr
        if #detail > 120 then detail = detail:sub(1, 117) .. "..." end
        fail(job, "E_BODY", font_ok and "正文失败" or "Chapter failed", detail)
        return "fail"
      end
    end
    local t0 = sys.millis()
    local text, err = Api.fetch_chapter_text(job.bookId, { chapterUid = job.chapterUid })
    log(string.format("[JJ] fetch_end t=%s ms=%s bytes=%s err=%s",
      tostring(sys.millis()), tostring(sys.millis() - t0),
      text and tostring(#text) or "0", tostring(err or "")))
    if not text then
      err = tostring(err or "unknown")
      if err == "login_required" then return "login" end
      if err == "use_file_path" then
        local fsz = Storage.chapter_file_size and Storage.chapter_file_size(job.bookId, job.chapterUid)
        if fsz and fsz > 0 then
          job.bytes = fsz
          job.phase = "handoff"
          job.status = font_ok and ("已缓存 " .. fmt_bytes(fsz) .. " · 打开…") or "opening…"
          return "continue"
        end
      end
      local detail = err
      if #detail > 120 then detail = detail:sub(1, 117) .. "..." end
      local title = font_ok and "正文失败" or "Chapter failed"
      local action = "retry_chapter"
      if err == "vip_unpaid" then
        title = font_ok and "VIP 未购买" or "VIP unpaid"
        detail = font_ok and "该章节需购买后阅读\n\n点按返回章节列表" or "not purchased\n\nTap for list"
        action = "toc"
        fail(job, "E_VIP", title, detail, action)
        return "fail"
      end
      fail(job, "E_BODY", title, detail, action)
      return "fail"
    end
    job.pending_text = text
    job.bytes = #text
    job.phase = "write"
    job.status = font_ok and ("写入缓存 " .. fmt_bytes(#text)) or ("writing " .. fmt_bytes(#text))
    return "continue"
  end

  if job.phase == "write" then
    local body = job.pending_text
    job.pending_text = nil
    if not body or body == "" then
      fail(job, "E_BODY", font_ok and "正文失败" or "Chapter failed",
        font_ok and "正文为空" or "empty body")
      return "fail"
    end
    job.bytes = #body
    log(string.format("[JJ] cache_write_begin t=%s bytes=%s atomic=1",
      tostring(sys.millis()), tostring(#body)))
    local ok_write = Storage.save_chapter_text(job.bookId, job.chapterUid, body)
    body = nil
    if collectgarbage then collectgarbage("collect") end
    if not ok_write then
      fail(job, "E_CACHE", font_ok and "缓存失败" or "Cache failed",
        font_ok and "章节写入 SD 失败" or "chapter SD write failed")
      return "fail"
    end
    local fsz = Storage.chapter_file_size(job.bookId, job.chapterUid)
    if not fsz or fsz <= 0 then
      fail(job, "E_CACHE", font_ok and "缓存失败" or "Cache failed",
        font_ok and "章节缓存大小无效" or "invalid chapter cache size")
      return "fail"
    end
    job.bytes = fsz
    job.phase = "handoff"
    job.status = font_ok and ("已缓存 " .. fmt_bytes(fsz) .. " · 打开…") or ("cached " .. fmt_bytes(fsz))
  end

  if job.phase == "handoff" then
    local path = job.path
    if (not path or path == "") and Storage and Storage.chapter_path then
      path = Storage.chapter_path(job.bookId, job.chapterUid)
      job.path = path
    end
    if (job.bytes or 0) < 1 and Storage and Storage.chapter_file_size then
      job.bytes = Storage.chapter_file_size(job.bookId, job.chapterUid) or 0
    end
    local ok, err = deps.open_native_reader(path, job.title, job.bookId, job.chapterUid)
    if ok then
      job.phase = "done"
      job.result = "handoff"
      return "done"
    end
    fail(job, "E_HOST", font_ok and "需要固件支持" or "Firmware required",
      font_ok and ("原生阅读桥不可用: " .. tostring(err or "no_reader"))
        or ("native reader unavailable: " .. tostring(err or "no_reader")))
    return "fail"
  end

  return "continue"
end

return ContentProvider
