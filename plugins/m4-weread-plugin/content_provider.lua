-- ContentProvider: UI-first chapter content pipeline (Apple-style progressive UX).
--
-- Contract:
--   * Front-end (draw) always paints a loading frame before any heavy backend step.
--   * Back-end step() performs at most one network hop or one SD write.
--   * Plugins own content source; native reader only opens finalized SD files.
--
-- Phase machine (job.phase):
--   ui_first  → force paint-only until ui_committed
--   probe     → SD size check (cache hit → open_native)
--   announce  → set "下载章节…" status, yield for paint
--   fetch     → Api.chapter_fetch_step (one hop per call)
--   write     → atomic SD cache write
--   handoff   → reader.openText
--   done / fail

ContentProvider = {}

local function status_for(job, font_ok)
  if not job then return "" end
  if type(job.status) == "string" and job.status ~= "" then
    return job.status
  end
  local ph = job.phase or ""
  if ph == "fetch" then
    return font_ok and "下载章节…" or "downloading…"
  elseif ph == "write" then
    return font_ok and "写入缓存…" or "writing cache…"
  elseif ph == "handoff" or ph == "probe" then
    return font_ok and "打开阅读器…" or "opening reader…"
  elseif ph == "announce" then
    return font_ok and "下载章节…" or "downloading…"
  end
  return font_ok and "正在加载章节…" or "loading chapter…"
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
    pct = 0,
    gen = 1,
    t0 = (sys and sys.millis and sys.millis()) or 0,
    status = opts.status or "",
    fetch = nil,
    pending_text = nil,
    result = nil,       -- nil | "handoff" | "fail" | "login"
    fail_code = nil,
    fail_title = nil,
    fail_body = nil,
    fail_action = nil,
  }
end

function ContentProvider.loading_body(job, font_ok)
  local sl = status_for(job, font_ok)
  if job and job.phase == "paginate" then
    local pct = job.pct or 0
    return font_ok and string.format("正在排版… %d%%", pct) or string.format("layout… %d%%", pct)
  end
  if string.find(sl, "下载", 1, true) or string.find(sl, "download", 1, true) then
    return font_ok and "下载章节…" or "downloading…"
  elseif string.find(sl, "写入", 1, true) or string.find(sl, "cache", 1, true) then
    return font_ok and "写入缓存…" or "writing cache…"
  elseif string.find(sl, "打开", 1, true) or string.find(sl, "open", 1, true) then
    return font_ok and "打开阅读器…" or "opening reader…"
  elseif string.find(sl, "准备", 1, true) or string.find(sl, "prepar", 1, true) then
    return font_ok and "准备打开…" or "preparing…"
  end
  return font_ok and "正在加载章节…" or "loading chapter…"
end

function ContentProvider.status_line(job, font_ok)
  return status_for(job, font_ok)
end

-- Mark that UI must paint before the next heavy step.
function ContentProvider.request_paint(job, status)
  if not job then return end
  if status then job.status = status end
  job.need_paint = true
end

-- True when draw should skip backend work (first loading frame only).
-- Status transitions still paint after advance in the same draw — only the
-- very first frame must commit before any network/SD work (UI-first).
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
  if job.fetch and Api and type(Api.release_chapter_buffers) == "function" then
    Api.release_chapter_buffers(job.fetch)
  end
  job.fetch = nil
end

-- One cooperative backend step. Call only when not should_paint_only.
-- deps:
--   font_ok, Storage, Api, ensure_network, open_native_reader, log, net_error_title, net_error_body
-- Returns: "continue" | "need_paint" | "done" | "empty" | "fail" | "login"
function ContentProvider.step(job, deps)
  if not job or not deps then return "fail" end
  local font_ok = deps.font_ok and true or false
  local Storage = deps.Storage
  local Api = deps.Api
  local log = deps.log or function() end

  if job.phase == "ui_first" then
    -- Should have been handled by paint-only; advance phase defensively.
    job.phase = "probe"
  end

  if job.phase == "probe" then
    local path = job.path
    if path == "" and Storage and Storage.chapter_path then
      path = Storage.chapter_path(job.bookId, job.chapterUid)
      job.path = path
    end
    local fsz = Storage and Storage.chapter_file_size
        and Storage.chapter_file_size(job.bookId, job.chapterUid) or nil
    if fsz and fsz > 0 then
      log(string.format("[WR05] cache_hit t=%s bytes=%s",
        tostring(sys.millis()), tostring(fsz)))
      job.phase = "handoff"
      job.status = font_ok and "打开阅读器…" or "opening reader…"
      -- Cache hit is cheap: open native in this same step (UI already painted once).
      local path2 = path
      if (not path2 or path2 == "") and Storage and Storage.chapter_path then
        path2 = Storage.chapter_path(job.bookId, job.chapterUid)
        job.path = path2
      end
      local ok, err = deps.open_native_reader(path2, job.title, job.bookId, job.chapterUid)
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
    -- Announce download; first network hop on next tick so "下载章节…" can paint.
    job.phase = "fetch"
    job.status = font_ok and "下载章节…" or "downloading…"
    if Api and Api.chapter_fetch_begin then
      job.fetch = Api.chapter_fetch_begin(job.bookId, { chapterUid = job.chapterUid })
    else
      job.fetch = { legacy = true }
    end
    log(string.format("[WR05] fetch_announce t=%s", tostring(sys.millis())))
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

    -- Incremental hop-by-hop fetch (one network call per host draw/tick).
    if job.fetch and not job.fetch.legacy and Api and Api.chapter_fetch_step then
      if not job.fetch then
        job.fetch = Api.chapter_fetch_begin(job.bookId, { chapterUid = job.chapterUid })
      end
      log(string.format("[WR05] fetch_step t=%s sub=%s",
        tostring(sys.millis()), tostring(job.fetch.step or "?")))
      local st, payload = Api.chapter_fetch_step(job.fetch)
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
        if err == "2012" then
          job.result = "login"
          job.phase = "fail"
          return "login"
        end
        if string.find(err, "EPUB ZIP", 1, true) then
          fail(job, "E_EPUB", font_ok and "不支持" or "Unsupported",
            font_ok and "整本 EPUB ZIP 暂不支持\n请换 TXT/分片章节" or "full EPUB ZIP not supported")
          return "fail"
        end
        -- A volume/section heading can have a valid UID but no body.  Let
        -- the host advance to the next real chapter instead of showing a
        -- misleading network/body error.
        if err == "empty" then
          job.phase = "empty"
          job.result = "empty"
          job.fetch = nil
          return "empty"
        end
        local detail = err
        if #detail > 120 then detail = detail:sub(1, 117) .. "..." end
        fail(job, "E_BODY", font_ok and "正文失败" or "Chapter failed", detail)
        return "fail"
      end
      -- done
      job.pending_text = payload
      if job.fetch and Api and type(Api.release_chapter_buffers) == "function" then
        Api.release_chapter_buffers(job.fetch)
      end
      job.fetch = nil
      job.phase = "write"
      job.status = font_ok and "写入缓存…" or "writing cache…"
      return "continue"
    end

    -- Legacy: single blocking Api.fetch_chapter_text (tests / old hosts).
    log(string.format("[WR05] fetch_begin t=%s whole_chapter=1 legacy=1", tostring(sys.millis())))
    local t0 = sys.millis()
    local text, err = Api.fetch_chapter_text(job.bookId, { chapterUid = job.chapterUid })
    log(string.format("[WR05] fetch_end t=%s ms=%s bytes=%s",
      tostring(sys.millis()), tostring(sys.millis() - t0),
      text and tostring(#text) or "0"))
    if not text then
      err = tostring(err or "unknown")
      if err == "2012" then
        job.result = "login"
        job.phase = "fail"
        return "login"
      end
      if string.find(err, "EPUB ZIP", 1, true) then
        fail(job, "E_EPUB", font_ok and "不支持" or "Unsupported",
          font_ok and "整本 EPUB ZIP 暂不支持\n请换 TXT/分片章节" or "full EPUB ZIP not supported")
        return "fail"
      end
      if err == "empty" then
        job.phase = "empty"
        job.result = "empty"
        return "empty"
      end
      local detail = err
      if #detail > 120 then detail = detail:sub(1, 117) .. "..." end
      fail(job, "E_BODY", font_ok and "正文失败" or "Chapter failed", detail)
      return "fail"
    end
    job.pending_text = text
    job.fetch = nil
    job.phase = "write"
    job.status = font_ok and "写入缓存…" or "writing cache…"
    return "continue"
  end

  if job.phase == "write" then
    local body = job.pending_text
    job.pending_text = nil
    if not body or body == "" then
      job.phase = "empty"
      job.result = "empty"
      return "empty"
    end
    log(string.format("[WR05] cache_write_begin t=%s bytes=%s atomic=1",
      tostring(sys.millis()), tostring(#body)))
    local t0 = sys.millis()
    local ok_write = Storage.save_chapter_text(job.bookId, job.chapterUid, body)
    body = nil
    if collectgarbage then collectgarbage("collect") end
    log(string.format("[WR05] cache_write_end t=%s ms=%s ok=%s",
      tostring(sys.millis()), tostring(sys.millis() - t0), tostring(ok_write and true or false)))
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
    -- Cache is durable: open native in this same step (same as cache-hit probe).
    -- Skipping the extra cooperative tick avoids a redundant "打开阅读器" frame
    -- without weakening TLS/network or streaming semantics.
    job.phase = "handoff"
    job.status = font_ok and "打开阅读器…" or "opening reader…"
    -- fall through to handoff
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
