-- ContentProvider: UI-first chapter pipeline (FanQie).
-- Uses host `loader` for progressive stream + early openText when available.
-- Loading UI: spinner / steps / bytes / elapsed (see ui_loading.lua).
ContentProvider = {}

local function U()
  if type(UiLoading) == "table" then return UiLoading end
  return nil
end

local function fmt_bytes(n)
  local u = U()
  if u and u.fmt_bytes then return u.fmt_bytes(n) end
  n = tonumber(n) or 0
  if n < 1024 then return tostring(math.floor(n)) .. "B" end
  return string.format("%.1fKB", n / 1024)
end

local function elapsed_s(job)
  local u = U()
  if u and u.elapsed_s then return u.elapsed_s(job and job.t0) end
  return 0
end

local function spinner(job)
  local u = U()
  if u and u.spinner then return u.spinner(elapsed_s(job)) end
  return "·"
end

local function phase_label(job, font_ok)
  if not job then return font_ok and "加载中…" or "loading…" end
  local u = U()
  if u and u.loader_phase_label and job.loader_phase and job.loader_phase ~= "" then
    local lp = u.loader_phase_label(job.loader_phase, font_ok)
    if lp ~= "" then return lp end
  end
  if type(job.status) == "string" and job.status ~= "" then return job.status end
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
  return {
    phase = "ui_first",
    ui_committed = false,
    need_paint = true,
    bookId = opts.bookId or "",
    chapterUid = opts.chapterUid or "",
    title = opts.title or "",
    path = opts.path or "",
    prefer = opts.prefer,
    chapterIndex = opts.chapterIndex or 0,
    pct = 0,
    gen = 1,
    t0 = (sys and sys.millis and sys.millis()) or 0,
    status = opts.status or "",
    bytes = 0,
    downloaded = 0,
    loader_phase = "",
    sys_loader = nil,
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
  lines[#lines + 1] = spinner(job) .. " " .. phase_label(job, font_ok)
  local ph = job.phase or ""
  if ph == "probe" then
    lines[#lines + 1] = font_ok and "步骤 1/3 · 查缓存" or "step 1/3 cache"
  elseif ph == "fetch" then
    if job.sys_loader == true then
      lines[#lines + 1] = font_ok and "步骤 2/3 · 宿主流式写盘" or "step 2/3 host stream"
      lines[#lines + 1] = font_ok
          and "约 2KB 正文即可打开首屏"
          or "early open ~2KB body"
    else
      lines[#lines + 1] = font_ok and "步骤 2/3 · 下载正文" or "step 2/3 download"
    end
    if (job.bytes or 0) > 0 or (job.downloaded or 0) > 0 then
      local n = math.max(tonumber(job.bytes) or 0, tonumber(job.downloaded) or 0)
      lines[#lines + 1] = (font_ok and "已接收 " or "recv ") .. fmt_bytes(n)
    else
      lines[#lines + 1] = font_ok and "等待首包…" or "waiting first byte…"
    end
  elseif ph == "write" then
    lines[#lines + 1] = font_ok and "步骤 2/3 · 写缓存" or "step 2/3 write"
    if (job.bytes or 0) > 0 then lines[#lines + 1] = fmt_bytes(job.bytes) end
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
  if n > 0 then bits[#bits + 1] = fmt_bytes(n) end
  local sec = elapsed_s(job)
  if sec >= 1 then bits[#bits + 1] = tostring(sec) .. "s" end
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
  if job.phase == "ui_first" then job.phase = "probe" end
end

local function fail(job, code, title, body, action)
  job.phase = "fail"
  job.result = "fail"
  job.fail_code = code
  job.fail_title = title
  job.fail_body = body
  job.fail_action = action or "retry_chapter"
  job.pending_text = nil
end

function ContentProvider.step(job, deps)
  if not job or not deps then return "fail" end
  local font_ok = deps.font_ok and true or false
  local Storage = deps.Storage
  local Api = deps.Api
  local log = deps.log or function() end

  if job.phase == "ui_first" then job.phase = "probe" end

  if job.phase == "probe" then
    local path = job.path
    if path == "" and Storage and Storage.chapter_path then
      path = Storage.chapter_path(job.bookId, job.chapterUid)
      job.path = path
    end
    local fsz = Storage and Storage.chapter_file_size
        and Storage.chapter_file_size(job.bookId, job.chapterUid) or nil
    -- Prefer complete cache (.ok) when helper exists (host progressive finish).
    local complete = true
    if Storage and Storage.chapter_complete then
      complete = Storage.chapter_complete(job.bookId, job.chapterUid)
    end
    if complete and fsz and fsz > 0 then
      job.bytes = fsz
      job.phase = "handoff"
      job.status = font_ok
          and ("缓存命中 " .. fmt_bytes(fsz) .. " · 打开…")
          or ("cache hit " .. fmt_bytes(fsz))
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
    if fsz and fsz > 0 and not complete and Storage.clear_chapter_cache then
      pcall(Storage.clear_chapter_cache, job.bookId, job.chapterUid)
    end
    job.phase = "fetch"
    job.status = font_ok and "下载章节…" or "downloading…"
    job.sys_loader = nil
    log(string.format("[FQ] fetch_announce t=%s", tostring(sys.millis())))
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

    -- Host progressive loader: arm once, pump each tick, refresh bytes/phase.
    if job.sys_loader ~= false and type(loader) == "table"
        and type(loader.chapter) == "function" and Api.chapter_loader_spec then
      if job.sys_loader == nil then
        local spec = Api.chapter_loader_spec(job.bookId, { chapterUid = job.chapterUid }, path, {
          title = job.title,
          chapterIndex = job.chapterIndex or 0,
        })
        if not spec then
          job.sys_loader = false
        else
          job.status = font_ok and "准备流式下载…" or "arm stream…"
          job.loader_phase = "connecting"
          local ok, err = loader.chapter(spec)
          if not ok then
            log(string.format("[FQ] loader.chapter fail %s", tostring(err)))
            job.sys_loader = false
            job.loader_phase = ""
            job.status = font_ok and "流式不可用 · 回退…" or "loader fallback…"
          else
            job.sys_loader = true
            job.status = font_ok and "连接服务器 (TLS)…" or "TLS connect…"
            return "continue"  -- paint connecting before first pump
          end
        end
      end
      if job.sys_loader == true then
        if type(loader.pump) == "function" then
          local st0 = (type(loader.status) == "function" and loader.status()) or {}
          if st0.phase == "connecting" then
            loader.pump({ ms = 50, bytes = 1024 })
          else
            loader.pump({ ms = 120, bytes = 48 * 1024 })
          end
        end
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
          job.sys_loader = false
          job.loader_phase = ""
          log(string.format("[FQ] loader fail → hop err=%s", tostring(st.error)))
          job.status = font_ok and ("回退下载 · " .. tostring(st.error)) or tostring(st.error)
        elseif st.done then
          -- Explicit plugin open after body complete.  Relying only on host
          -- finishOk/queueOpen left the loading screen forever when handoff
          -- was dropped (same bug as jjwxc).
          job.bytes = st.bytes or job.bytes
          job.loader_phase = "done"
          job.status = font_ok and "打开阅读器…" or "opening…"
          log(string.format("[FQ] loader_done bytes=%s → open_native",
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
          job.status = (font_ok and "下载中 " or "dl ")
              .. fmt_bytes(job.bytes or 0)
          return "continue"
        else
          return "continue"
        end
      end
    end

    -- Legacy / fallback: one-shot to file or text.
    log(string.format("[FQ] fetch_begin t=%s", tostring(sys.millis())))
    if Api.fetch_chapter_to_file and path then
      local n, ferr = Api.fetch_chapter_to_file(job.bookId,
        { chapterUid = job.chapterUid }, path)
      if n and tonumber(n) > 0 then
        job.bytes = tonumber(n)
        job.phase = "handoff"
        job.status = font_ok
            and ("已缓存 " .. fmt_bytes(n) .. " · 打开…")
            or ("cached " .. fmt_bytes(n))
        job.pending_text = nil
        if Storage and Storage.mark_chapter_complete then
          pcall(Storage.mark_chapter_complete, job.bookId, job.chapterUid)
        end
        if collectgarbage then collectgarbage("collect") end
        return "continue"
      end
      if ferr and ferr ~= "unsupported" then
        log(string.format("[FQ] chapter_file_fallback err=%s", tostring(ferr)))
      end
    end
    local t0 = sys.millis()
    local text, err = Api.fetch_chapter_text(job.bookId, { chapterUid = job.chapterUid })
    log(string.format("[FQ] fetch_end t=%s ms=%s bytes=%s",
      tostring(sys.millis()), tostring(sys.millis() - t0),
      text and tostring(#text) or "0"))
    if not text then
      err = tostring(err or "unknown")
      local detail = err
      if #detail > 120 then detail = detail:sub(1, 117) .. "..." end
      fail(job, "E_BODY", font_ok and "正文失败" or "Chapter failed", detail)
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
    log(string.format("[FQ] cache_write_begin t=%s bytes=%s atomic=1",
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
    job.status = font_ok and ("已缓存 " .. fmt_bytes(fsz) .. " · 打开…") or "opening…"
  end

  if job.phase == "handoff" then
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
  end

  return "continue"
end

return ContentProvider
