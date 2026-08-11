-- com.weread.client — modular WeRead plugin for Murphy M4
-- Loads: storage/layout/auth/api/ui_* via sys.load

sys.load("storage.lua")
sys.load("catalog.lua")
sys.load("layout.lua")
sys.load("auth.lua")
sys.load("api.lua")
sys.load("content_provider.lua")
sys.load("ui_template.lua")
sys.load("ui_login.lua")
sys.load("ui_shelf.lua")
sys.load("ui_toc.lua")
sys.load("ui_reader.lua")

UI_METRICS = Layout.metrics()
PAGE_SIZE = UI_METRICS.shelf_page_size
TOC_PAGE = UI_METRICS.toc_page_size
READER_MARGIN = 24
READER_LINE = UI_METRICS.reader_line_h

-- screens: startup | login | shelf | toc | reader | loading | native_reader | native_toc | message
-- native_reader / native_toc: parent must not repaint over native first frame
-- (no Lua displayBuffer — races e-ink BUSY and leaves residual UI).
screen = "startup"
status_line = ""
books = {}
shelf_page = 1
message_title, message_body, message_hint = "", "", "tap/back"
-- message_action: "exit" | "retry_shelf" | "toc" | "shelf" | "login" | "retry_chapter"
message_action = "exit"
message_code = ""  -- short machine-readable error code for plugin error page
dirty = true
-- Host may keep pumping (dirty) while frame_changed is false so cooperative
-- chapter load continues without reflashing identical AA pixels.
frame_changed = true
last_loading_frame = nil

-- Startup is deliberately cooperative. init() only publishes this state, so
-- AppRuntime can commit an interactive first frame before SD/session/network
-- work begins. Each later draw advances at most one phase.
startup_job = nil
-- Network operations entered from a touch callback yield one painted frame
-- before the synchronous bridge request, so a slow TLS/JSON call cannot look
-- like a dead tap.
network_job = nil  -- { kind, data, label, body, return_screen, ui_committed }
local STARTUP_SPINNER = { "|", "/", "-", "\\" }

function begin_startup(phase, pct, label)
  screen = "startup"
  status_line = label or "starting"
  startup_job = {
    phase = phase or "font",
    pct = pct or 0,
    label = label or "starting",
    frame = 0,
    ui_committed = false,
  }
  dirty = true
end

function startup_status(job)
  local pct = tonumber(job and job.pct) or 0
  local label = tostring(job and job.label or "starting")
  local frame = (tonumber(job and job.frame) or 0) % #STARTUP_SPINNER + 1
  return STARTUP_SPINNER[frame] .. "  " .. label .. "  " .. tostring(pct) .. "%"
end

function draw_startup()
  UiTemplate.page({ UI_METRICS = UI_METRICS }, "WeRead", nil,
    startup_status(startup_job), "back / exit")
end

function begin_network_job(kind, data, label, body)
  network_job = {
    kind = kind,
    data = data,
    return_screen = screen,
    label = label or (font_ok and "加载中…" or "loading…"),
    body = body or (font_ok and "正在联网读取，请稍候…" or "network request in progress…"),
    ui_committed = false,
  }
  status_line = network_job.label
  screen = "loading"
  dirty = true
  frame_changed = true
  last_loading_frame = nil
end

function cancel_network_job()
  local job = network_job
  network_job = nil
  if type(job) ~= "table" then return false end
  screen = (job.kind == "toc" and "shelf") or job.return_screen or "shelf"
  dirty = true
  return true
end

function advance_network_job()
  local job = network_job
  if type(job) ~= "table" then return end
  network_job = nil
  if job.kind == "toc" then
    load_toc_sync(job.data)
  elseif job.kind == "shelf" then
    load_shelf_sync()
  end
end

-- Set by provider.takeResume during startup; applied before login when cache exists.
pending_history_resume = nil
-- After a cold history open, optionally refresh TOC/session without blocking the reader.
history_bg_restore = nil  -- { bookId, needToc, needSession }
-- Bare wordCount==0 is trusted only for a current 3-column catalog or a
-- versioned toc.json. Old toc.json rows defaulted missing wordCount to zero.
toc_wordcount_reliable = false

-- Resolve 1-based chapter index for history reopen.
-- Priority (must NOT default to chapter 1 when host/progress know better):
--   1) resume.chapterUid (from history originalSourcePath / session)
--   2) progress.chapterUid
--   3) resume.chapterIndex (0-based from host)
--   4) progress.chapterIdx (1-based)
--   5) 1
function resolve_history_chapter_idx(bookId, chaptersList, resume)
  local list = chaptersList
  if type(list) ~= "table" or #list < 1 then return 1 end
  local function idx_for_uid(uid)
    if not uid or tostring(uid) == "" then return nil end
    local u = tostring(uid)
    for i = 1, #list do
      if tostring(list[i].chapterUid or "") == u then return i end
    end
    return nil
  end
  if type(resume) == "table" then
    local fromResume = idx_for_uid(resume.chapterUid)
    if fromResume then return fromResume end
  end
  local prog = Storage.load_progress and Storage.load_progress(bookId) or nil
  if type(prog) == "table" then
    local fromProg = idx_for_uid(prog.chapterUid)
    if fromProg then return fromProg end
  end
  if type(resume) == "table" and type(resume.chapterIndex) == "number"
      and resume.chapterIndex >= 0 then
    local i = math.floor(resume.chapterIndex) + 1
    if i >= 1 and i <= #list then return i end
  end
  if type(prog) == "table" and type(prog.chapterIdx) == "number"
      and prog.chapterIdx >= 1 and prog.chapterIdx <= #list then
    return math.floor(prog.chapterIdx)
  end
  return 1
end

-- Seed native_progress so open_native_reader restores page offset immediately.
function seed_history_native_progress(bookId, chapterUid, resume)
  local uid = tostring(chapterUid or "")
  local bid = tostring(bookId or "")
  if bid == "" or uid == "" then return end
  local off = nil
  if type(resume) == "table" and type(resume.byteOffset) == "number" and resume.byteOffset >= 0 then
    off = math.floor(resume.byteOffset)
  end
  local prog = Storage.load_progress and Storage.load_progress(bid) or nil
  if type(prog) == "table" and tostring(prog.chapterUid or "") == uid then
    if off == nil and type(prog.byteOffset) == "number" and prog.byteOffset >= 0 then
      off = math.floor(prog.byteOffset)
    end
    native_progress = {
      bookId = bid,
      chapterUid = uid,
      page = math.max(1, math.floor(tonumber(prog.page) or 1)),
      total = prog.total,
      byteOffset = off or 0,
      complete = prog.complete and true or false,
      percent = prog.percent,
    }
    return
  end
  if off ~= nil then
    native_progress = {
      bookId = bid,
      chapterUid = uid,
      page = 1,
      byteOffset = off,
      complete = false,
    }
  end
end

-- True when a chapter body for this resume is already on SD (login not required).
function history_resume_cache_ready()
  local r = pending_history_resume
  if type(r) ~= "table" or not r.bookId or r.bookId == "" then return false end
  if not Storage.chapter_file_size then return false end
  local bookId = tostring(r.bookId)

  -- Host-carried cache path / chapterUid is the authoritative last-read location.
  if r.chapterUid and tostring(r.chapterUid) ~= "" then
    local fsz = Storage.chapter_file_size(bookId, tostring(r.chapterUid))
    if fsz and fsz > 0 then return true end
  end
  if r.cacheRelPath and tostring(r.cacheRelPath) ~= "" and type(fs.fileSize) == "function" then
    local n = fs.fileSize(tostring(r.cacheRelPath))
    if n and n > 0 then return true end
  end

  local toc = Storage.load_toc and Storage.load_toc(bookId) or nil
  if type(toc) == "table" and #toc > 0 then
    local idx = resolve_history_chapter_idx(bookId, toc, r)
    local ch = toc[idx]
    if ch and ch.chapterUid then
      local fsz = Storage.chapter_file_size(bookId, ch.chapterUid)
      if fsz and fsz > 0 then return true end
    end
  end
  return false
end

-- Open book from Home m4cp:// resume without shelf UI / login when cache exists.
function try_apply_history_resume()
  local r = pending_history_resume
  if type(r) ~= "table" or not r.bookId or r.bookId == "" then return false end
  pending_history_resume = nil
  local book = { bookId = tostring(r.bookId), title = tostring(r.title or r.bookId) }
  books = books or {}
  local found = false
  for i = 1, #books do
    if tostring(books[i].bookId or "") == book.bookId then
      book = books[i]
      found = true
      break
    end
  end
  if not found then books[#books + 1] = book end
  log(string.format("[WRCP] apply_history_resume book=%s ch=%s cache=%s",
    book.bookId, tostring(r.chapterUid or ""), tostring(r.cacheRelPath or "")))

  local toc = Storage.load_toc and Storage.load_toc(book.bookId) or nil
  local hadFullToc = type(toc) == "table" and #toc > 0

  -- Cold history reopen can use the persisted FileRows metadata without
  -- loading toc.json or scanning the catalog in Lua.
  local meta = Storage.load_catalog_meta and Storage.load_catalog_meta(book.bookId) or nil
  local metaSize = meta and type(fs) == "table" and type(fs.fileSize) == "function"
    and fs.fileSize(tostring(meta.source or "")) or nil
  if not hadFullToc and meta and metaSize and metaSize > 0 then
    chapter_catalog = Catalog.spec(meta.source, meta.count, meta.uid_field, meta.title_field,
      meta.word_count_field)
    toc_source = chapter_catalog.source
    chapters = Catalog.virtual_rows(chapter_catalog, "weread", book.bookId)
    cur_book = book
    local p = Storage.load_progress and Storage.load_progress(book.bookId) or nil
    chapter_idx = (type(p) == "table" and tonumber(p.chapterIdx))
      or (tonumber(r.chapterIndex) and tonumber(r.chapterIndex) + 1) or 1
    if chapter_idx < 1 or chapter_idx > #chapters then chapter_idx = 1 end
    pcall(provider_register_current_book)
    local uid = tostring(r.chapterUid or (p and p.chapterUid) or "")
    if uid ~= "" and Storage.chapter_file_size then
      local fsz = Storage.chapter_file_size(book.bookId, uid)
      if fsz and fsz > 0 then
        seed_history_native_progress(book.bookId, uid, r)
        history_bg_restore = { bookId = book.bookId, needToc = false, needSession = true }
        open_chapter(chapter_idx)
        return true
      end
    end
    load_toc(book)
    return true
  end

  if hadFullToc then
    chapters = toc
    cur_book = book
    chapter_idx = resolve_history_chapter_idx(book.bookId, chapters, r)
    -- If host chapterUid is not in TOC (stale TOC), still prefer host cache body.
    if r.chapterUid and tostring(r.chapterUid) ~= "" then
      local matched = false
      for i = 1, #chapters do
        if tostring(chapters[i].chapterUid or "") == tostring(r.chapterUid) then
          chapter_idx = i
          matched = true
          break
        end
      end
      if not matched and Storage.chapter_file_size then
        local fsz = Storage.chapter_file_size(book.bookId, tostring(r.chapterUid))
        if fsz and fsz > 0 then
          chapters[#chapters + 1] = {
            chapterUid = tostring(r.chapterUid),
            title = tostring(r.title or r.chapterUid),
          }
          chapter_idx = #chapters
        end
      end
    end
    pcall(provider_register_current_book)
    local ch = chapters[chapter_idx]
    if ch and Storage.chapter_file_size then
      local fsz = Storage.chapter_file_size(book.bookId, ch.chapterUid)
      if fsz and fsz > 0 then
        seed_history_native_progress(book.bookId, ch.chapterUid, r)
        -- Background: refresh session/TOC later if logged in (non-blocking).
        history_bg_restore = { bookId = book.bookId, needToc = false, needSession = true }
        open_chapter(chapter_idx)
        return true
      end
    end
    -- TOC present but body missing: need network; do not pretend success offline.
    load_toc(book)
    return true
  end

  -- Cold reopen: only last chapter cache (no TOC). Open that chapter immediately;
  -- schedule full TOC restore after native handoff when session allows.
  if r.chapterUid and tostring(r.chapterUid) ~= "" and Storage.chapter_file_size then
    local uid = tostring(r.chapterUid)
    local title = tostring(r.title or uid)
    chapters = {{ chapterUid = uid, title = title }}
    cur_book = book
    chapter_idx = 1
    pcall(provider_register_current_book)
    local fsz = Storage.chapter_file_size(book.bookId, uid)
    if fsz and fsz > 0 then
      seed_history_native_progress(book.bookId, uid, r)
      history_bg_restore = { bookId = book.bookId, needToc = true, needSession = true }
      open_chapter(1)
      return true
    end
  end

  -- cacheRelPath known but chapterUid missing: still try fs size then open via Storage path.
  if r.cacheRelPath and tostring(r.cacheRelPath) ~= "" and type(fs.fileSize) == "function" then
    local n = fs.fileSize(tostring(r.cacheRelPath))
    if n and n > 0 then
      local uid = tostring(r.chapterUid or "")
      if uid == "" then
        -- Best-effort: ch_<uid>.txt
        local base = tostring(r.cacheRelPath):match("([^/]+)$") or ""
        uid = base:match("^ch_(.+)%.txt$") or base:match("^ch_(.+)%.TXT$") or base
      end
      if uid ~= "" then
        chapters = {{ chapterUid = uid, title = tostring(r.title or uid) }}
        cur_book = book
        chapter_idx = 1
        pcall(provider_register_current_book)
        seed_history_native_progress(book.bookId, uid, r)
        history_bg_restore = { bookId = book.bookId, needToc = true, needSession = true }
        open_chapter(1)
        return true
      end
    end
  end

  -- No usable cache: normal load_toc (may require network/login).
  load_toc(book)
  return true
end

-- After native reader owns the panel, refresh TOC/session without blocking UI.
function maybe_history_bg_restore()
  local job = history_bg_restore
  if type(job) ~= "table" or not job.bookId then return end
  if screen == "native_reader" or screen == "loading" or screen == "native_toc" then
    -- Defer until reader closes, or only do non-UI work: register is already done.
    -- Full TOC fetch needs network and would fight native ownership — wait for close.
    return
  end
  history_bg_restore = nil
  if not cur_book or tostring(cur_book.bookId) ~= tostring(job.bookId) then return end
  if job.needToc and Auth.has and Auth.has() then
    -- Soft refresh: replace synthetic single-chapter TOC with full list when online.
    pcall(function()
      local file_mode = type(dl) == "table" and type(dl.jsonToFile) == "function"
      if file_mode and type(Api.fetch_toc_to_file) == "function" then
        local tocRel = Storage.toc_rows_path and Storage.toc_rows_path(cur_book.bookId)
          or ("cache/" .. tostring(cur_book.bookId) .. "/toc_rows.txt")
        local n = Api.fetch_toc_to_file(cur_book.bookId, tocRel)
        if n and tonumber(n) > 0 then
          chapter_catalog = Catalog.spec(tocRel, n, 0, 1, 2)
          toc_source = tocRel
          chapters = Catalog.virtual_rows(chapter_catalog, "weread", cur_book.bookId)
          toc_wordcount_reliable = true
          Storage.save_catalog_meta(cur_book.bookId, chapter_catalog)
          pcall(provider_register_current_book)
        end
        return
      end
      local list = Api.fetch_toc and Api.fetch_toc(cur_book.bookId) or nil
      if type(list) == "table" and #list > 0 then
        chapters = list
        toc_wordcount_reliable = true
        Storage.save_toc(cur_book.bookId, chapters)
        pcall(provider_register_current_book)
        log(string.format("[WRCP] history_bg_toc book=%s n=%d", tostring(job.bookId), #list))
      end
    end)
  end
end

function finish_startup_shelf(list, prefix)
  books = merge_local_progress_into_books(list)
  shelf_page = 1
  screen = "shelf"
  status_line = (prefix or "") .. tostring(#books) .. (font_ok and " 本书" or " books")
  startup_job = nil
  dirty = true
  if try_apply_history_resume() then return end
end

function advance_startup()
  local job = startup_job
  if not job then return end
  job.frame = (job.frame or 0) + 1

  if job.phase == "font" then
    refresh_font_info()
    -- Diagnostic only; the host always provides a drawable system face.
    if not font_ok then
      log("[WR05] font_diag result=not_full_cjk available=" .. tostring(font_available)
        .. " readerFontId=" .. tostring(font_reader_id)
        .. " hint=" .. tostring(font_hint))
    end
    job.phase, job.pct, job.label = "session", 20, "loading session"
  elseif job.phase == "session" then
    Auth.load()
    -- Home history reopen (m4cp://) queues a resume intent before AppRuntime starts.
    -- Consume immediately (host may clear session registry later).
    if type(provider) == "table" and type(provider.takeResume) == "function" then
      local resume = provider.takeResume()
      if type(resume) == "table" and resume.bookId and tostring(resume.bookId) ~= "" then
        pending_history_resume = {
          bookId = tostring(resume.bookId),
          title = tostring(resume.title or resume.bookId),
          providerId = tostring(resume.providerId or "weread"),
          chapterUid = tostring(resume.chapterUid or ""),
          cacheRelPath = tostring(resume.cacheRelPath or ""),
          chapterIndex = tonumber(resume.chapterIndex),
          byteOffset = tonumber(resume.byteOffset),
        }
        log(string.format("[WRCP] history_resume book=%s ch=%s cache=%s off=%s",
          pending_history_resume.bookId,
          tostring(pending_history_resume.chapterUid or ""),
          tostring(pending_history_resume.cacheRelPath or ""),
          tostring(pending_history_resume.byteOffset or "-")))
      end
    end
    -- Cached history is a local fast path. Apply BEFORE Auth.has() so reboot
    -- never flashes login/session for an already-cached chapter body.
    if pending_history_resume and history_resume_cache_ready() then
      startup_job = nil
      if try_apply_history_resume() then return end
    end
    -- Resume present but body not cached: still apply (load_toc) only when
    -- logged in; otherwise login first then finish_startup_shelf applies resume.
    if pending_history_resume and Auth.has() then
      startup_job = nil
      if try_apply_history_resume() then return end
    end
    if not Auth.has() then
      -- Routing is its own bounded phase. Login networking starts on the
      -- following tick, never in the session-load tick.
      job.phase, job.pct, job.label = "login", 40, "preparing login"
    else
      job.phase, job.pct, job.label = "network", 40, "connecting"
    end
  elseif job.phase == "login" then
    startup_job = nil
    begin_login_flow()
    return
  elseif job.phase == "network" then
    local online, nerr = ensure_network()
    if not online then
      local cached = Storage.load_shelf_cache()
      if cached then
        finish_startup_shelf(cached, font_ok and "离线缓存 · " or "offline · ")
      else
        startup_job = nil
        set_message(net_error_title(nerr), net_error_body(nerr),
          font_ok and "点按重试 · 返回退出" or "tap retry / back exit", "retry_shelf")
      end
      return
    end
    job.phase, job.pct, job.label = "shelf", 70, "loading shelf"
  elseif job.phase == "shelf" then
    local list, err = Api.fetch_shelf()
    if err == "2012" then
      Auth.clear()
      startup_job = nil
      begin_login_flow()
      return
    end
    if list then
      finish_startup_shelf(list)
      return
    end
    local cached = Storage.load_shelf_cache()
    if cached then
      finish_startup_shelf(cached, font_ok and "缓存 · " or "cache · ")
      return
    end
    startup_job = nil
    local title = font_ok and "书架失败" or "Shelf failed"
    local body = tostring(err or "unknown")
    if err == "wifi_not_connected" then
      title, body = net_error_title(err), net_error_body(err)
    elseif type(err) == "string" and
        (err:find("ssl", 1, true) or err:find("tls", 1, true) or err:find("TLS", 1, true)) then
      title = font_ok and "TLS 失败" or "TLS failed"
    elseif type(err) == "string" and err:match("^%d+$") then
      title = "HTTP " .. err
    end
    set_message(title, body,
      font_ok and "点按重试 · 返回退出" or "tap retry / back exit", "retry_shelf")
    return
  end
  dirty = true
end

cur_book = nil
chapters = {}
chapter_catalog = nil -- FileRows spec; rows resolve one-at-a-time
toc_source = nil
toc_page = 1
-- Phase 5A: chapter body stays on SD. Lua holds path + compact page starts only.
reader_path = nil          -- SD rel path to .txt
reader_file_size = 0
reader_page_starts = {}    -- 0-based absolute offsets (compact numbers)
reader_page_chunk = nil    -- current page window string for draw (<= 16 KiB)
reader_text = nil          -- legacy; must stay nil for large chapters
reader_pages = {}          -- unused in file mode (kept for state compat)
reader_page = 1
reader_title = ""
chapter_idx = 1
progress_upload_ok = nil  -- nil=untried, true/false
login_retry_after = 0

-- Chapter open job (WeRead 0.5.x UI-first ContentProvider handoff):
--   Front-end paints loading before any network/SD heavy work.
--   Back-end (ContentProvider.step) does at most one hop per draw tick:
--   probe cache → announce → incremental fetch hops → atomic write → openText.
--   Progressive layout is native only. "paginate" remains legacy for hosts
--   without native reader / simulator file tests.
chapter_job = nil
-- Monotonic handoff timing (host millis); no secrets.
native_handoff_t0 = 0
-- Ops (fit_line / newline actions) per inner paginate step (legacy Lua only).
PAGINATE_OPS_PER_STEP = 40
-- Wall-time slice (ms) for legacy Lua paginate work inside one draw/tick.
PAGINATE_SLICE_MS = 120
-- Preferred page once pagination completes: number | "last" | nil
reader_page_prefer = nil
-- Chapter index to reopen on "retry_chapter"
retry_chapter_idx = nil

-- Font supply is host-owned (SETTINGS.getReaderFontId / EpdFontLoader / GfxRenderer).
-- font_ok = full canonical SD CJK promoted (diagnostic only). Never hard-block UI:
-- without canonical, system still paints via builtin UI subset + safe fallback.
font_ok = true
font_hint = ""
font_reader_id = 0
font_available = true

function refresh_font_info()
  if type(sys.fontInfo) ~= "function" then
    -- Host without fontInfo: system fonts still draw (simulator / minimal tests).
    font_ok = true
    font_hint = ""
    font_reader_id = 0
    font_available = true
    return
  end
  local fi = sys.fontInfo()
  if type(fi) == "table" then
    font_ok = fi.ok and true or false
    font_hint = tostring(fi.hint or "")
    font_reader_id = tonumber(fi.readerFontId) or 0
    if fi.available == nil then
      font_available = true
    else
      font_available = fi.available and true or false
    end
  else
    font_ok = true
    font_hint = ""
    font_reader_id = 0
    font_available = true
  end
end

-- Plugins never hard-block Chinese UI on a missing SD font.
function can_enter_chinese_ui()
  return true
end

-- Map connectSaved / request errors to short user-facing text.
-- Always Chinese chrome: host system font covers I18n subset offline.
function net_error_title(err)
  if err == "no_saved_wifi" then return "无已存 Wi-Fi" end
  if err == "timeout" then return "Wi-Fi 超时" end
  if err == "cancelled" then return "已取消" end
  if err == "connect_failed" then return "Wi-Fi 失败" end
  if err == "wifi_not_connected" then return "无网络" end
  if err == "tls_error" or err == "ssl_error" then return "TLS 失败" end
  return "网络错误"
end

function net_error_body(err)
  if err == "no_saved_wifi" then
    return "没有已保存的 Wi-Fi\n请先在系统设置连接"
  end
  if err == "timeout" then
    return "连接已保存网络超时\n点按重试或检查设置"
  end
  if err == "connect_failed" then
    return "已保存网络连接失败\n点按重试或检查设置"
  end
  if err == "cancelled" then
    return "连接已取消"
  end
  return tostring(err or "未知错误")
end

-- Ensure STA link via net.connectSaved before offline fallback.
-- Returns ok, err_code (err only when not ok).
function ensure_network()
  if net.isConnected() then
    return true, nil
  end
  if type(net.connectSaved) ~= "function" then
    return false, "wifi_not_connected"
  end
  local r = net.connectSaved(20000)
  if type(r) == "table" and r.ok then
    return true, nil
  end
  local err = (type(r) == "table" and r.error and r.error ~= "") and r.error or "connect_failed"
  return false, err
end

function set_message(title, body, hint, action, code)
  screen = "message"
  message_title = title or ""
  message_body = body or ""
  message_hint = hint or (font_ok and "点按返回" or "tap/back")
  message_action = action or "exit"
  message_code = code or ""
  dirty = true
end

function state_table()
  return {
    status_line = status_line,
    books = books,
    shelf_page = shelf_page,
    PAGE_SIZE = PAGE_SIZE,
    cur_book = cur_book,
    chapters = chapters,
    toc_page = toc_page,
    TOC_PAGE = TOC_PAGE,
    chapter_idx = chapter_idx,
    reader_title = reader_title,
    reader_text = reader_text,
    reader_path = reader_path,
    reader_file_size = reader_file_size,
    reader_page_starts = reader_page_starts,
    reader_page_chunk = reader_page_chunk,
    reader_pages = reader_pages,
    reader_page = reader_page,
    reader_page_count = reader_page_count(),
    READER_MARGIN = READER_MARGIN,
    READER_LINE = READER_LINE,
    UI_METRICS = UI_METRICS,
    chapter_job = chapter_job,
  }
end

function reader_page_count()
  if reader_page_starts and #reader_page_starts > 0 then
    return #reader_page_starts
  end
  if reader_pages then return #reader_pages end
  return 0
end

function layout_fp_for_reader()
  local metrics = UI_METRICS or Layout.metrics()
  local line_h = math.max(READER_LINE or 36, metrics.reader_line_h)
  local maxW = gui.width() - (READER_MARGIN or 24) * 2
  if maxW < 1 then maxW = 1 end
  local top = metrics.content_y
  local bottom = metrics.content_bottom
  local maxLines = math.max(1, math.floor((bottom - top) / line_h))
  return Storage.layout_fingerprint(READER_MARGIN, line_h, 12, maxW, maxLines)
end

-- Drop temporary chapter load/paginate state so cancel or reopen can restart cleanly.
function clear_chapter_job()
  if chapter_job then
    chapter_job.pending_text = nil
    if chapter_job.paginate then
      local pj = chapter_job.paginate
      pj.win = nil
      pj.text = nil
    end
  end
  chapter_job = nil
  if collectgarbage then collectgarbage("collect") end
end

function cancel_chapter_load()
  clear_chapter_job()
  reader_text = nil
  reader_path = nil
  reader_file_size = 0
  reader_page_starts = {}
  reader_page_chunk = nil
  reader_pages = {}
  reader_page = 1
  reader_page_prefer = nil
  progress_upload_ok = nil
  if collectgarbage then collectgarbage("collect") end
end

function chapter_load_fail(code, title, body, action)
  local idx = chapter_idx
  cancel_chapter_load()
  retry_chapter_idx = idx
  set_message(title, body,
    font_ok and "点按重试 · 返回目录" or "tap retry / back toc",
    action or "retry_chapter", code)
end

-- Start SD-windowed pagination. Does NOT hold full chapter text in Lua.
function begin_paginate_file(path, file_size)
  reader_path = path
  reader_file_size = tonumber(file_size) or 0
  reader_text = nil
  reader_pages = {}
  reader_page_starts = {}
  reader_page_chunk = nil
  reader_page = 1
  local job = Layout.file_paginate_begin(path, reader_file_size, READER_MARGIN, READER_LINE, 12)
  chapter_job = {
    phase = "paginate",
    paginate = job,
    pct = 0,
    gen = (chapter_job and chapter_job.gen or 0) + 1,
    path = path,
    size = reader_file_size,
  }
  status_line = font_ok and "排版 0%" or "layout 0%"
  screen = "loading"
  dirty = true
end

-- Legacy in-memory path (tiny strings / tests without fileSize).
function begin_paginate(text)
  reader_text = text or ""
  reader_path = nil
  reader_file_size = #reader_text
  reader_pages = {}
  reader_page_starts = {}
  reader_page_chunk = nil
  reader_page = 1
  local job = Layout.paginate_begin(reader_text, READER_MARGIN, READER_LINE, 12)
  chapter_job = {
    phase = "paginate",
    paginate = job,
    pct = 0,
    gen = (chapter_job and chapter_job.gen or 0) + 1,
  }
  status_line = font_ok and "排版 0%" or "layout 0%"
  screen = "loading"
  dirty = true
end

function finish_paginate()
  local job = chapter_job and chapter_job.paginate
  if not job then
    chapter_load_fail("E_PAGE", font_ok and "排版失败" or "Layout failed",
      font_ok and "分页状态丢失" or "paginate state lost")
    return
  end
  if job.mode == "file" then
    reader_page_starts = job.page_starts or { 0 }
    if #reader_page_starts == 0 then reader_page_starts = { 0 } end
    reader_path = job.path
    reader_file_size = job.n or 0
    reader_text = nil
    reader_pages = {}
    -- Drop window string from job before discard.
    job.win = nil
    -- Persist compact index (best-effort; failure does not block reading).
    local ch = chapters[chapter_idx]
    if cur_book and ch then
      local meta = {
        size = reader_file_size,
        layout = layout_fp_for_reader(),
      }
      pcall(Storage.save_pidx, cur_book.bookId, ch.chapterUid, meta, reader_page_starts)
    end
  else
    -- Memory mode: convert pages to synthetic page_starts if possible; keep pages.
    reader_pages = job.pages or {}
    if #reader_pages == 0 then
      reader_pages = { { lines = { { start = 1, finish = 0 } } } }
    end
    reader_page_starts = {}
  end

  local nPages = reader_page_count()
  if nPages < 1 then nPages = 1 end
  local prefer = reader_page_prefer
  reader_page_prefer = nil
  if prefer == "last" then
    reader_page = nPages
  elseif type(prefer) == "number" then
    reader_page = math.max(1, math.min(prefer, nPages))
  else
    reader_page = 1
    local ch = chapters[chapter_idx]
    if cur_book and ch then
      local prog = Storage.load_progress(cur_book.bookId)
      if prog and prog.chapterUid == ch.chapterUid and prog.page then
        reader_page = math.max(1, math.min(prog.page, nPages))
      end
    end
  end
  clear_chapter_job()
  reader_page_chunk = nil
  status_line = string.format("%d/%d", reader_page, nPages)
  screen = "reader"
  dirty = true
  if collectgarbage then collectgarbage("collect") end
  save_progress_local()
  pcall(try_upload_progress)
end

function try_open_cached_pidx(bookId, chapterUid, file_size)
  local starts = Storage.load_pidx(bookId, chapterUid, file_size, layout_fp_for_reader())
  if not starts then return false end
  reader_path = Storage.chapter_path(bookId, chapterUid)
  reader_file_size = file_size
  reader_page_starts = starts
  reader_text = nil
  reader_pages = {}
  reader_page_chunk = nil
  local prefer = reader_page_prefer
  reader_page_prefer = nil
  local nPages = #starts
  if prefer == "last" then
    reader_page = nPages
  elseif type(prefer) == "number" then
    reader_page = math.max(1, math.min(prefer, nPages))
  else
    reader_page = 1
    local prog = Storage.load_progress(bookId)
    if prog and prog.chapterUid == chapterUid and prog.page then
      reader_page = math.max(1, math.min(prog.page, nPages))
    end
  end
  clear_chapter_job()
  status_line = string.format("%d/%d", reader_page, nPages)
  screen = "reader"
  dirty = true
  save_progress_local()
  pcall(try_upload_progress)
  return true
end

-- One bounded step of chapter load. Safe to call from draw()/tick AFTER UI paint.
function step_chapter_load()
  local job = chapter_job
  if not job then return end

  -- Legacy Lua paginate path (simulator / hosts without native reader).
  if job.phase == "paginate" then
    local pj = job.paginate
    if not pj then
      chapter_load_fail("E_PAGE", font_ok and "排版失败" or "Layout failed",
        font_ok and "分页任务丢失" or "paginate job lost")
      return
    end
    local ok, st, pct = pcall(Layout.paginate_step, pj, PAGINATE_OPS_PER_STEP)
    if not ok then
      local detail = tostring(st)
      if #detail > 120 then detail = detail:sub(1, 117) .. "..." end
      chapter_load_fail("E_PAGE", font_ok and "排版失败" or "Layout failed", detail)
      return
    end
    if st == "error" then
      local detail = tostring(pj.io_error or "paginate_error")
      if detail == "too_many_pages" then
        chapter_load_fail("E_PAGE", font_ok and "章节过长" or "Too long",
          font_ok and "分页页数超限" or "page limit exceeded")
      else
        chapter_load_fail("E_PAGE", font_ok and "排版失败" or "Layout failed", detail)
      end
      return
    end
    job.pct = pct or job.pct or 0
    if st == "done" then
      finish_paginate()
    else
      status_line = string.format(font_ok and "排版 %d%%" or "layout %d%%", job.pct or 0)
      screen = "loading"
      dirty = true
    end
    return
  end

  -- UI-first ContentProvider pipeline (probe / fetch hops / write / handoff).
  if not ContentProvider or type(ContentProvider.step) ~= "function" then
    chapter_load_fail("E_STATE", font_ok and "正文失败" or "Chapter failed",
      font_ok and "ContentProvider 不可用" or "ContentProvider missing")
    return
  end
  if not cur_book or not chapters[chapter_idx] then
    chapter_load_fail("E_STATE", font_ok and "正文失败" or "Chapter failed",
      font_ok and "书籍/章节状态无效" or "invalid book/chapter")
    return
  end

  local r = ContentProvider.step(job, {
    font_ok = font_ok,
    Storage = Storage,
    Api = Api,
    ensure_network = ensure_network,
    open_native_reader = open_native_reader,
    log = log,
    net_error_title = net_error_title,
    net_error_body = net_error_body,
  })
  if not chapter_job then return end

  if r == "done" then
    -- open_native_reader already set screen=native_reader; do not clobber it.
    clear_chapter_job()
    return
  end
  if r == "login" then
    cancel_chapter_load()
    Auth.clear()
    begin_login_flow()
    return
  end
  if r == "empty" then
    local from = chapter_idx
    local next_idx = resolve_real_chapter_index((chapter_idx or 0) + 1)
    cancel_chapter_load()
    if next_idx then
      log(string.format("[WR05] empty_body_redirect from=%d to=%d", from or 0, next_idx))
      open_chapter(next_idx)
    else
      set_message(font_ok and "本节无正文" or "empty section",
        font_ok and "后面没有可打开的正文" or "no readable chapter below",
        font_ok and "点按返回目录" or "tap to toc", "toc")
    end
    return
  end
  if r == "fail" then
    local code = job.fail_code or "E_BODY"
    local title = job.fail_title or (font_ok and "正文失败" or "Chapter failed")
    local body = job.fail_body or ""
    local action = job.fail_action or "retry_chapter"
    chapter_load_fail(code, title, body, action)
    return
  end
  -- continue: still loading — refresh status for paint after this step.
  if job.status and job.status ~= "" then
    status_line = job.status
  end
  if screen ~= "native_reader" and screen ~= "native_toc" then
    screen = "loading"
  end
  dirty = true  -- keep host pumping cooperative steps until done
end

-- Native reader progress (byte-stable). Do not depend on Lua page indexes.
native_progress = nil  -- { chapterUid, page, total, byteOffset, complete, percent }

function save_progress_local()
  if not cur_book then return end
  local ch = chapters[chapter_idx]
  local entry = {
    chapterIdx = chapter_idx,
    page = reader_page,
    title = cur_book.title,
    bookId = cur_book.bookId,
    chapterUid = ch and ch.chapterUid or "",
  }
  if type(native_progress) == "table" then
    -- Only persist native progress when book+chapter still match current context.
    local sameBook = tostring(native_progress.bookId or cur_book.bookId) == tostring(cur_book.bookId)
    local sameCh = tostring(native_progress.chapterUid or "") == tostring(entry.chapterUid)
    if sameBook and sameCh then
      entry.byteOffset = tonumber(native_progress.byteOffset) or 0
      entry.percent = native_progress.percent
      entry.total = native_progress.total
      entry.complete = native_progress.complete and true or false
      if type(native_progress.page) == "number" then
        entry.page = math.max(1, math.floor(native_progress.page))
      end
      if native_progress.chapterUid and native_progress.chapterUid ~= "" then
        entry.chapterUid = native_progress.chapterUid
      end
    end
  end
  -- Always store a shelf-visible percent when possible (cloud upload may be off).
  if type(entry.percent) ~= "number" then
    if type(entry.total) == "number" and entry.total > 0 and type(entry.page) == "number" then
      local p = math.max(1, math.floor(entry.page))
      local t = math.max(1, math.floor(entry.total))
      if p > t then p = t end
      entry.percent = math.floor(p * 100 / t)
    elseif type(entry.byteOffset) == "number" and entry.byteOffset > 0
        and type(reader_file_size) == "number" and reader_file_size > 0 then
      entry.percent = math.min(100, math.floor(entry.byteOffset * 100 / reader_file_size))
    elseif type(entry.byteOffset) == "number" and entry.byteOffset > 0 then
      -- Reading mid-chapter without size: show at least 1% so shelf is not stuck at 0.
      entry.percent = 1
    else
      entry.percent = 0
    end
  end
  Storage.save_progress_entry(cur_book.bookId, entry)
  -- Keep in-memory shelf row in sync so Home/书架 show local progress immediately.
  if type(books) == "table" then
    for i = 1, #books do
      local b = books[i]
      if b and tostring(b.bookId) == tostring(cur_book.bookId) then
        b.progress = entry.percent or 0
        if entry.chapterUid and entry.chapterUid ~= "" then
          b.progressChapterUid = entry.chapterUid
        end
        break
      end
    end
  end
end

-- Merge local progress.json into shelf rows (prefer local when present).
function merge_local_progress_into_books(list)
  if type(list) ~= "table" then return list end
  for i = 1, #list do
    local b = list[i]
    if b and b.bookId then
      local lp = Storage.load_progress(b.bookId)
      if type(lp) == "table" then
        if type(lp.percent) == "number" then
          b.progress = math.max(0, math.min(100, math.floor(lp.percent)))
        elseif type(lp.byteOffset) == "number" and lp.byteOffset > 0 then
          b.progress = math.max(tonumber(b.progress) or 0, 1)
        end
        if lp.chapterUid and lp.chapterUid ~= "" then
          b.progressChapterUid = lp.chapterUid
        end
      end
    end
  end
  return list
end

function try_upload_progress()
  -- Disabled by default (Api.ENABLE_PROGRESS_UPLOAD); never block UI/draw.
  if not Api.ENABLE_PROGRESS_UPLOAD then
    progress_upload_ok = false
    return
  end
  if not cur_book then return end
  local ch = chapters[chapter_idx]
  if not ch then return end
  local pct = 0
  local page = reader_page
  if type(native_progress) == "table" and native_progress.chapterUid == ch.chapterUid then
    page = tonumber(native_progress.page) or page
    if type(native_progress.percent) == "number" then
      pct = math.floor(native_progress.percent)
    elseif native_progress.complete and type(native_progress.total) == "number" and native_progress.total > 0 then
      pct = math.floor(page * 100 / native_progress.total)
    elseif type(native_progress.byteOffset) == "number" then
      -- Percent unknown without file size; leave 0 rather than invent from empty Lua index.
      pct = 0
    end
  else
    -- Legacy Lua index only when native progress absent.
    local nPages = reader_page_count()
    if nPages > 0 then
      pct = math.floor(reader_page * 100 / nPages)
    end
  end
  local ok = Api.upload_progress(cur_book.bookId, ch.chapterUid, page, pct)
  progress_upload_ok = ok and true or false
end

function load_shelf_sync()
  local online, nerr = ensure_network()
  if not online then
    local cached = Storage.load_shelf_cache()
    if cached then
      books = merge_local_progress_into_books(cached)
      screen = "shelf"
      status_line = (font_ok and "离线缓存 · " or "offline · ") .. tostring(#books)
      dirty = true
      return true
    end
    set_message(net_error_title(nerr), net_error_body(nerr),
      font_ok and "点按重试 · 返回退出" or "tap retry / back exit", "retry_shelf")
    return false
  end
  status_line = font_ok and "正在加载书架..." or "loading shelf..."
  local list, err = Api.fetch_shelf()
  if err == "2012" then
    Auth.clear()
    begin_login_flow()
    return false
  end
  if not list then
    local cached = Storage.load_shelf_cache()
    if cached then
      books = merge_local_progress_into_books(cached)
      screen = "shelf"
      status_line = (font_ok and "缓存 · " or "cache · ") .. tostring(#books) .. " (" .. tostring(err) .. ")"
      dirty = true
      return true
    end
    -- Distinguish HTTP/TLS/request errors from pure offline.
    local title = font_ok and "书架失败" or "Shelf failed"
    local body = tostring(err or "unknown")
    if err == "wifi_not_connected" then
      title = net_error_title(err)
      body = net_error_body(err)
    elseif type(err) == "string" and (err:find("ssl", 1, true) or err:find("tls", 1, true) or err:find("TLS", 1, true)) then
      title = font_ok and "TLS 失败" or "TLS failed"
    elseif type(err) == "string" and err:match("^%d+$") then
      title = font_ok and ("HTTP " .. err) or ("HTTP " .. err)
    end
    set_message(title, body, font_ok and "点按重试 · 返回退出" or "tap retry / back exit", "retry_shelf")
    return false
  end
  books = merge_local_progress_into_books(list)
  shelf_page = 1
  screen = "shelf"
  status_line = tostring(#books) .. (font_ok and " 本书" or " books")
  dirty = true
  if try_apply_history_resume() then return true end
  return true
end

function load_shelf()
  begin_network_job("shelf", nil,
    font_ok and "加载书架…" or "loading shelf…",
    font_ok and "正在获取书架，请稍候…" or "fetching shelf…")
  return true
end

function load_toc(book)
  if type(book) ~= "table" then return false end
  if not cur_book or cur_book.bookId ~= book.bookId then
    native_progress = nil
  end
  cur_book = book
  chapters = {}
  chapter_catalog = nil
  toc_wordcount_reliable = false
  toc_page = 1
  begin_network_job("toc", book,
    font_ok and "加载目录…" or "loading table of contents…",
    font_ok and "正在读取章节目录，请稍候…" or "fetching chapter list…")
  return true
end

function load_toc_sync(book)
  -- Switching book: drop stale native_progress so it cannot be written under another chapter.
  if not cur_book or not book or cur_book.bookId ~= book.bookId then
    native_progress = nil
  end
  cur_book = book
  chapters = {}
  chapter_catalog = nil
  toc_page = 1
  status_line = font_ok and "加载目录..." or "loading toc..."
  local tocRel = Storage.toc_rows_path and Storage.toc_rows_path(book.bookId)
    or ("cache/" .. tostring(book.bookId) .. "/toc_rows.txt")
  toc_source = tocRel
  local list, err, loaded = nil, nil, false
  local file_mode = type(dl) == "table" and type(dl.jsonToFile) == "function"

  -- Local-first: a TOC is durable metadata, not a session-only view.  Do not
  -- contact the network when a previously downloaded catalog is still valid.
  -- This also makes opening a book from history work offline and avoids the
  -- long "loading" pause on every reopen.
  local meta = Storage.load_catalog_meta and Storage.load_catalog_meta(book.bookId) or nil
  -- 0.6.8 adds a third row field (wordCount) used to identify empty volume
  -- headings.  Do not keep using an older two-column FileRows cache after an
  -- upgrade when we are online; refresh it once, with the old JSON cache as a
  -- safe offline fallback below.
  local stale_catalog = meta and tonumber(meta.word_count_field or -1) < 0
  local cached_toc, cached_version = Storage.load_toc(book.bookId)
  local stale_json = type(cached_toc) == "table" and #cached_toc > 0
      and tonumber(cached_version or 0) < (Storage.TOC_VERSION or 2)
  local metaSize = meta and type(fs) == "table" and type(fs.fileSize) == "function"
    and fs.fileSize(tostring(meta.source or "")) or nil
  if meta and not stale_catalog and metaSize and metaSize > 0 then
    chapter_catalog = Catalog.spec(meta.source, meta.count, meta.uid_field, meta.title_field,
      meta.word_count_field)
    toc_source = chapter_catalog.source
    chapters = Catalog.virtual_rows(chapter_catalog, "weread", book.bookId)
    toc_wordcount_reliable = tonumber(meta.word_count_field or -1) >= 0
    -- A virtual FileRows table resolves rows through the host provider
    -- registry.  Register it before handing control to the native TOC;
    -- otherwise the first native row callback can observe a nil Lua row.
    pcall(provider_register_current_book)
    loaded = true
  end
  if not loaded and not stale_catalog and not stale_json then
    if type(cached_toc) == "table" and #cached_toc > 0 then
      chapters = cached_toc
      toc_wordcount_reliable = true
      loaded = true
    end
  end

  if file_mode
      and not loaded
      and type(Api.fetch_toc_to_file) == "function" then
    local n, ferr = Api.fetch_toc_to_file(book.bookId, tocRel)
    if n and tonumber(n) > 0 then
      chapter_catalog = Catalog.spec(tocRel, n, 0, 1, 2)
      chapters = Catalog.virtual_rows(chapter_catalog, "weread", book.bookId)
      toc_wordcount_reliable = true
      pcall(Storage.save_catalog_meta, book.bookId, chapter_catalog)
      pcall(provider_register_current_book)
      loaded = true
    else
      err = ferr
    end
  end
  -- FileRows is an optimization.  If jsonToFile cannot serve this request
  -- (for example an HTTP 404 from an older bridge), fall back to the normal
  -- net.request path before consulting the local cache.
  if not loaded then
    list, err = Api.fetch_toc(book.bookId)
    if err == "2012" then
      Auth.clear()
      begin_login_flow()
      return false
    end
    if list then
      chapters = list
      toc_wordcount_reliable = true
      pcall(Storage.save_toc, book.bookId, chapters)
      loaded = #chapters > 0
    end
  end
  if not loaded then
    if type(cached_toc) == "table" and #cached_toc > 0 then
      chapters = cached_toc
      toc_wordcount_reliable = false
      loaded = true
      err = err or "offline_cached_toc"
    end
  end
  if not loaded then
    set_message(font_ok and "目录失败" or "TOC failed", err or "unknown",
      font_ok and "点按回书架" or "tap to shelf", "shelf")
    return false
  end

  local prog = Storage.load_progress(book.bookId)
  if prog and prog.chapterIdx and prog.chapterIdx >= 1 and prog.chapterIdx <= #chapters then
    chapter_idx = prog.chapterIdx
  elseif book.progressChapterUid and book.progressChapterUid ~= "" then
    chapter_idx = 1
    if not chapter_catalog then
      for i = 1, #chapters do
        if chapters[i].chapterUid == book.progressChapterUid then
          chapter_idx = i
          break
        end
      end
    end
  else
    chapter_idx = 1
  end
  toc_page = math.floor((chapter_idx - 1) / TOC_PAGE) + 1
  status_line = #chapters .. (font_ok and " 章 · " or " ch · ") .. book.title
  dirty = true
  -- Prefer system chapter list (full-CJK reader font, ±100 jump, no Lua repaint race).
  if open_native_toc() then
    return true
  end
  screen = "toc"
  return true
end

-- System TOC handoff (TxtReaderChapterSelectionActivity + toc.json titles).
function open_native_toc()
  if not cur_book or #chapters < 1 then return false end
  if type(reader) ~= "table" or type(reader.openToc) ~= "function" then
    return false
  end
  local tocPath = "cache/" .. tostring(cur_book.bookId) .. "/toc.json"
  local hasTocPath = type(fs.fileSize) == "function" and (fs.fileSize(tocPath) or 0) > 0
  if not hasTocPath then
    return false
  end
  local opts = {
    bookId = tostring(cur_book.bookId or ""),
    title = tostring(cur_book.title or "目录"),
    currentIndex = math.max(0, (chapter_idx or 1) - 1),  -- 0-based for native
  }
  if hasTocPath then opts.tocPath = tocPath end
  local ok, err = reader.openToc(opts)
  if ok then
    screen = "native_toc"
    dirty = false
    log(string.format("[WR05] openToc_accepted t=%s ch=%d", tostring(sys.millis()), chapter_idx or 0))
    return true
  end
  log(string.format("[WR05] openToc_fail t=%s err=%s", tostring(sys.millis()), tostring(err)))
  return false
end

function onTocClosed(r)
  log(string.format("[WR05] onTocClosed t=%s", tostring(sys.millis())))
  -- Host closed native TOC before this callback (same ownership rule as reader).
  if screen == "native_toc" then
    screen = "loading"
    dirty = true
    frame_changed = true
    last_loading_frame = nil
  end
  if type(r) == "table" and not r.cancelled and type(r.chapterIndex) == "number" then
    local idx = math.floor(r.chapterIndex) + 1  -- native 0-based → Lua 1-based
    if idx >= 1 and idx <= #chapters then
      if open_chapter(idx) then
        return
      end
    end
  end
  -- Cancel / back / invalid index → shelf (not Lua toc to avoid dual UIs).
  if #books > 0 then
    screen = "shelf"
  else
    screen = "login"
  end
  status_line = font_ok and "已返回书架" or "back to shelf"
  dirty = true
end

-- WeRead occasionally returns volume/section headings in `updated`.  They
-- have a title and UID so they look like normal rows, but carry no body
-- (wordCount == 0).  Treat them as navigation anchors: selecting one opens
-- the first real chapter below it instead of starting a doomed empty fetch.
function chapter_is_empty_parent(ch)
  if type(ch) ~= "table" then return false end
  if ch.isParent == true or ch.is_empty_parent == true or ch.isEmpty == true
      or ch.empty == true then
    return true
  end
  local kind = string.lower(tostring(ch.chapterType or ch.type or ""))
  if kind == "parent" or kind == "section" or kind == "volume" then
    return true
  end
  if toc_wordcount_reliable and ch.wordCount ~= nil then
    local words = tonumber(ch.wordCount)
    if words and words <= 0 then return true end
  end
  return false
end

function resolve_real_chapter_index(idx)
  local first = math.floor(tonumber(idx) or 0)
  if first < 1 or first > #chapters then return nil end
  local first_row = chapters[first]
  -- FileRows resolution is lazy.  A missing row is an unavailable chapter,
  -- not a normal chapter and must never reach open_chapter's dereference.
  if type(first_row) ~= "table"
      or tostring(first_row.chapterUid or first_row.uid or "") == "" then
    return nil
  end
  if not chapter_is_empty_parent(first_row) then return first end
  for i = first + 1, #chapters do
    local row = chapters[i]
    if type(row) == "table"
        and tostring(row.chapterUid or row.uid or "") ~= ""
        and not chapter_is_empty_parent(row) then
      return i
    end
  end
  return nil
end

-- Start chapter open quickly: switch to loading and return. Backend work
-- advances only AFTER the first loading frame is painted (UI-first).
-- prefer: nil | number | "last"
function open_chapter(idx, prefer)
  if not cur_book or idx < 1 or idx > #chapters then return false end
  -- Cannot start a chapter while native reader still owns the session.
  -- native_toc is closed by host before onTocClosed; allow open from that path.
  if screen == "native_reader" then
    return false
  end
  local requested_idx = idx
  local real_idx = resolve_real_chapter_index(idx)
  if not real_idx then
    set_message(font_ok and "本节无正文" or "empty section",
      font_ok and "已到目录末尾，请选择一个有正文的章节" or "choose a chapter with text",
      font_ok and "点按返回目录" or "tap to toc", "toc")
    log(string.format("[WR05] empty_parent_no_next idx=%d", requested_idx))
    return true
  end
  if real_idx ~= requested_idx then
    log(string.format("[WR05] empty_parent_redirect from=%d to=%d", requested_idx, real_idx))
    idx = real_idx
  end
  -- Cancel any in-flight job before starting a new one.
  cancel_chapter_load()
  local ch = chapters[idx]
  if type(ch) ~= "table" or tostring(ch.chapterUid or ch.uid or "") == "" then
    set_message(font_ok and "章节读取失败" or "chapter unavailable",
      font_ok and "章节索引尚未准备好，请稍后重试" or "chapter index unavailable; retry",
      font_ok and "点按返回目录" or "tap to toc", "toc")
    log(string.format("[WR05] chapter_resolve_failed idx=%d", idx))
    return true
  end
  -- Switching chapter: reset native_progress unless it already matches this chapter.
  if type(native_progress) == "table" then
    local same = tostring(native_progress.bookId or "") == tostring(cur_book.bookId or "")
      and tostring(native_progress.chapterUid or "") == tostring(ch.chapterUid or "")
    if not same then
      native_progress = nil
    end
  end
  chapter_idx = idx
  retry_chapter_idx = idx
  reader_title = ch.title or ""
  reader_page_prefer = prefer
  status_line = font_ok and "准备打开…" or "preparing…"
  screen = "loading"
  local path = Storage.chapter_path(cur_book.bookId, ch.chapterUid)
  chapter_job = ContentProvider.begin({
    bookId = cur_book.bookId,
    chapterUid = ch.chapterUid,
    title = ch.title or reader_title or "",
    path = path,
    prefer = prefer,
    status = status_line,
  })
  log(string.format("[WR05] chapter_tap t=%s book=%s ch=%s ui_first=1",
    tostring(sys.millis()), tostring(cur_book.bookId), tostring(ch.chapterUid)))
  dirty = true
  return true
end

function begin_login_flow()
  screen = "login"
  dirty = true
  draw()
  local now = sys.millis()
  if now < login_retry_after then
    Auth.login_msg = "请稍候再重试"
    dirty = true
    return
  end
  local ok = Auth.begin_login()
  if not ok and Auth.login_fatal then
    -- AppRuntimeActivity observes wantsExit after start/draw, then host.stop()
    -- closes Lua and releases all app-session memory.
    sys.exit()
    return
  end
  if not ok then login_retry_after = sys.millis() + 10000 end
  dirty = true
end

function handle_message_action()
  local a = message_action or "exit"
  if a == "retry_shelf" then
    if Auth.has() then
      begin_startup("network", 40, "connecting")
    else
      begin_startup("login", 40, "preparing login")
    end
  elseif a == "retry_chapter" then
    local idx = retry_chapter_idx or chapter_idx
    if cur_book and idx and idx >= 1 and idx <= #chapters then
      open_chapter(idx)
    elseif #chapters > 0 then
      screen = "toc"
      dirty = true
    else
      screen = "shelf"
      dirty = true
    end
  elseif a == "toc" and #chapters > 0 then
    screen = "toc"
    dirty = true
  elseif a == "shelf" and #books > 0 then
    screen = "shelf"
    dirty = true
  elseif a == "login" then
    begin_login_flow()
  else
    -- Prefer soft recovery when possible; else exit.
    if #chapters > 0 then
      screen = "toc"
      dirty = true
    elseif #books > 0 then
      screen = "shelf"
      dirty = true
    elseif Auth.has() then
      load_shelf()
    else
      sys.exit()
    end
  end
end

function draw_loading()
  local sl = status_line or ""
  if chapter_job and ContentProvider and ContentProvider.status_line then
    local s2 = ContentProvider.status_line(chapter_job, font_ok)
    if s2 and s2 ~= "" then sl = s2 end
  end
  local body
  if network_job and network_job.body then
    body = network_job.body
  elseif ContentProvider and ContentProvider.loading_body then
    body = ContentProvider.loading_body(chapter_job, font_ok)
  else
    body = font_ok and "正在加载章节…" or "loading chapter…"
  end
  local hint = font_ok and "点按/返回键 · 回目录" or "tap/back · toc"
  UiTemplate.page(state_table(), reader_title or "", sl, body, hint)
end
function advance_chapter_load_slice()
  if not chapter_job then return end
  -- ContentProvider phases: exactly one cooperative step per host draw/tick.
  if chapter_job.phase ~= "paginate" then
    step_chapter_load()
    return
  end
  local t0 = sys.millis()
  local slice = PAGINATE_SLICE_MS or 120
  for _ = 1, 80 do
    if not chapter_job or chapter_job.phase ~= "paginate" then return end
    step_chapter_load()
    if not chapter_job then return end
    if sys.millis() - t0 >= slice then return end
  end
end

function draw()
  -- Native reader/TOC owns the physical display after handoff is accepted.
  -- Do not clear/refresh — that overwrites the native first frame with loading.
  if screen == "native_reader" or screen == "native_toc" then
    dirty = false
    frame_changed = false
    return
  end

  -- UI-first: never run backend work until loading UI has been committed once
  -- (and again after each status transition that sets need_paint).
  local paint_only = false
  if chapter_job and ContentProvider and ContentProvider.should_paint_only then
    paint_only = ContentProvider.should_paint_only(chapter_job)
  elseif chapter_job and not chapter_job.ui_committed then
    paint_only = true
  end

  if chapter_job and not paint_only then
    advance_chapter_load_slice()
  end

  -- Handoff may have completed during advance; skip paint if native owns display.
  if screen == "native_reader" or screen == "native_toc" then
    dirty = false
    frame_changed = false
    return
  end

  -- Cooperative fetch ticks often leave the visible loading frame unchanged.
  -- Keep pumping backend work, but do not physically refresh the same pixels
  -- (AA makes those redundant writes especially noticeable).
  if screen == "loading" then
    local loading_body = network_job and network_job.body
      or (ContentProvider and ContentProvider.loading_body
        and ContentProvider.loading_body(chapter_job, font_ok) or "")
    local loading_frame = tostring(status_line or "") .. "\n" .. tostring(loading_body)
    frame_changed = loading_frame ~= last_loading_frame
    last_loading_frame = loading_frame
  else
    frame_changed = true
    last_loading_frame = nil
  end

  gui.clear()
  local st = state_table()
  if screen == "startup" then
    draw_startup()
  elseif screen == "login" then
    UiLogin.draw(st)
  elseif screen == "shelf" then
    UiShelf.draw(st)
  elseif screen == "toc" then
    UiToc.draw(st)
  elseif screen == "reader" then
    UiReader.draw(st)
  elseif screen == "loading" then
    draw_loading()
  elseif screen == "message" then
    local title = message_title or ""
    if message_code and message_code ~= "" then
      title = message_code .. " · " .. title
    end
    UiTemplate.page(state_table(), title, nil, message_body or "", message_hint or "返回")
  else
    gui.drawText(12, 24, 80, "...")
  end

  -- After first / transition paint, mark committed and keep dirty so host
  -- schedules another Tick for backend work (network never blocks first frame).
  local keep_pumping = startup_job ~= nil or network_job ~= nil
  if chapter_job then
    if paint_only and ContentProvider and ContentProvider.mark_painted then
      ContentProvider.mark_painted(chapter_job)
    elseif paint_only then
      chapter_job.ui_committed = true
      chapter_job.need_paint = false
      if chapter_job.phase == "ui_first" then chapter_job.phase = "probe" end
    end
    keep_pumping = true
  end
  -- Do NOT call gui.refresh() here. AppRuntimeActivity owns the single
  -- displayBuffer after callDraw. A second refresh inside Lua raced the
  -- native reader handoff on e-ink BUSY and left "打开阅读器" stuck.
  dirty = keep_pumping
end

function onKey(key)
  if key == "back" or key == "left" then
    if screen == "startup" then
      startup_job = nil
      sys.exit()
      return
    end
    if screen == "login" then
      Auth.cancel_login()
      sys.exit()
      return
    end
    if screen == "native_reader" or screen == "native_toc" then
      -- Native session is owned by AppRuntime; cannot cancel from Lua.
      return
    end
    if screen == "loading" then
      if network_job then
        cancel_network_job()
        cancel_chapter_load()
        return
      end
      cancel_chapter_load()
      if not open_native_toc() then
        screen = "toc"
        dirty = true
      end
      return
    end
    if screen == "reader" then
      save_progress_local()
      pcall(try_upload_progress)
      if not open_native_toc() then
        screen = "toc"
        dirty = true
      end
    elseif screen == "toc" then
      screen = "shelf"
      dirty = true
    elseif screen == "message" then
      -- Back from chapter error: always TOC when available (do not auto-retry).
      if message_action == "retry_chapter" and #chapters > 0 then
        cancel_chapter_load()
        screen = "toc"
        dirty = true
        return
      end
      -- Back always exits message flow to a safe place or app exit.
      if #chapters > 0 then
        screen = "toc"
        dirty = true
      elseif #books > 0 then
        screen = "shelf"
        dirty = true
      else
        sys.exit()
      end
    else
      sys.exit()
    end
    return
  end
  if key == "confirm" or key == "right" or key == "center" then
    if screen == "login" then
      if Auth.login_uid then Auth.poll_login_step() else begin_login_flow() end
      dirty = true
    elseif screen == "native_reader" or screen == "native_toc" then
      -- Native owns input; ignore.
      return
    elseif screen == "loading" then
      -- Confirm while loading also aborts to TOC (same as back).
      if network_job then
        cancel_network_job()
        cancel_chapter_load()
        return
      end
      cancel_chapter_load()
      if not open_native_toc() then
        screen = "toc"
        dirty = true
      end
    elseif screen == "shelf" then
      if shelf_page < UiShelf.page_count(books, PAGE_SIZE) then
        shelf_page = shelf_page + 1
        dirty = true
      end
    elseif screen == "toc" then
      if toc_page < UiToc.page_count(chapters, TOC_PAGE) then
        toc_page = toc_page + 1
        dirty = true
      end
    elseif screen == "reader" then
      local nPages = reader_page_count()
      if reader_page < nPages then
        reader_page = reader_page + 1
        reader_page_chunk = nil
        status_line = string.format("%d/%d", reader_page, nPages)
        save_progress_local()
        dirty = true
      elseif chapter_idx < #chapters then
        open_chapter(chapter_idx + 1)
      end
    elseif screen == "message" then
      handle_message_action()
    end
  end
end

function onTouch(x, y, phase)
  if phase and phase ~= "tap" and phase ~= "up" and phase ~= "press" then return end
  local w = gui.width()

  if screen == "login" then
    local m = UI_METRICS
    local buttons = Layout.home_button_rects(m)
    local button = Layout.button_index_from_point(x, y, buttons)
    -- Explicit touch UI: content is not actionable; only painted buttons hit.
    if not button then return end
    if button == 1 then
      Auth.cancel_login()
      sys.exit()
      return
    end
    if Auth.login_uid then
      local st = Auth.poll_login_step()
      if st == "ok" then load_shelf() end
    else
      begin_login_flow()
    end
    dirty = true
    return
  end

  if screen == "message" then
    handle_message_action()
    return
  end

  if screen == "native_reader" or screen == "native_toc" then
    -- Native session owns input; ignore Lua touch.
    return
  end

  if screen == "loading" then
    -- Any tap during load cancels and returns to TOC (only before openText accepted).
    if network_job then
      cancel_network_job()
      cancel_chapter_load()
      return
    end
    cancel_chapter_load()
    if not open_native_toc() then
      screen = "toc"
      dirty = true
    end
    return
  end

  if screen == "shelf" then
    local m = UI_METRICS
    if Layout.home_footer_hit(y, m) then
      if x < w / 2 then
        if shelf_page > 1 then shelf_page = shelf_page - 1; dirty = true end
      else
        if shelf_page < UiShelf.page_count(books, PAGE_SIZE) then
          shelf_page = shelf_page + 1
          dirty = true
        end
      end
      return
    end
    local rows = UiShelf.slice(books, shelf_page, PAGE_SIZE)
    local idx = Layout.home_row_from_point(y, m, #rows, m.shelf_row_h)
    if idx then load_toc(rows[idx]) end
    return
  end

  if screen == "toc" then
    local m = UI_METRICS
    if y < m.content_y then
      screen = "shelf"
      dirty = true
      return
    end
    if Layout.home_footer_hit(y, m) then
      if x < w / 2 then
        if toc_page > 1 then toc_page = toc_page - 1; dirty = true end
      else
        if toc_page < UiToc.page_count(chapters, TOC_PAGE) then
          toc_page = toc_page + 1
          dirty = true
        end
      end
      return
    end
    local rows, start = UiToc.slice(chapters, toc_page, TOC_PAGE)
    local idx = Layout.home_row_from_point(y, m, #rows, m.toc_row_h)
    if idx then open_chapter(start + idx - 1) end
    return
  end

  if screen == "reader" then
    if y < UI_METRICS.content_y then
      save_progress_local()
      pcall(try_upload_progress)
      screen = "toc"
      dirty = true
      return
    end
    local nPages = reader_page_count()
    if x < w / 2 then
      if reader_page > 1 then
        reader_page = reader_page - 1
        reader_page_chunk = nil
        status_line = string.format("%d/%d", reader_page, nPages)
        save_progress_local()
        dirty = true
      elseif chapter_idx > 1 then
        -- Prefer last page once async pagination finishes.
        open_chapter(chapter_idx - 1, "last")
      end
    else
      if reader_page < nPages then
        reader_page = reader_page + 1
        reader_page_chunk = nil
        status_line = string.format("%d/%d", reader_page, nPages)
        save_progress_local()
        dirty = true
      elseif chapter_idx < #chapters then
        open_chapter(chapter_idx + 1)
      end
    end
  end
end

function onReaderClosed(prog)
  -- Native TxtReaderActivity returned. Restore TOC; persist native progress.
  log(string.format("[WR05] onReaderClosed t=%s", tostring(sys.millis())))
  clear_chapter_job()
  -- Host tears down the native reader before this callback. Drop Lua ownership
  -- so open_chapter (in-reader TOC chapter switch) is not rejected by the
  -- native_reader guard — that left screen stuck and freezes the UI.
  if screen == "native_reader" then
    screen = "loading"
    dirty = true
    frame_changed = true
    last_loading_frame = nil
  end
  -- Finish deferred TOC restore now that we can safely touch book state.
  pcall(maybe_history_bg_restore)
  if type(prog) == "table" and cur_book then
    -- Explicit open/load failure: do not treat as successful close at page 1.
    if prog.openFailed or (type(prog.error) == "string" and prog.error ~= "") then
      status_line = font_ok and ("打开失败: " .. tostring(prog.error or "error"))
        or ("open failed: " .. tostring(prog.error or "error"))
      if not open_native_toc() then
        screen = "toc"
        dirty = true
      end
      return
    end
    -- System TOC while reading picked another chapter (0-based index).
    if type(prog.switchChapterIndex) == "number" then
      local idx = math.floor(prog.switchChapterIndex) + 1
      if idx >= 1 and idx <= #chapters then
        -- Persist progress for the chapter we left, then open the new one.
        local ch = chapters[chapter_idx]
        local expectUid = ch and tostring(ch.chapterUid or "") or ""
        local expectBook = tostring(cur_book.bookId or "")
        local expectKey = "weread:" .. expectBook .. ":" .. expectUid
        local gotUid = tostring(prog.chapterUid or "")
        local gotBook = tostring(prog.bookId or "")
        local gotKey = tostring(prog.progressKey or "")
        local idsOk = (gotBook ~= "" and gotBook == expectBook)
          and (gotUid ~= "" and gotUid == expectUid)
          and (gotKey == "" or gotKey == expectKey)
        if idsOk then
          local page1 = math.floor(tonumber(prog.page) or 1)
          if page1 < 1 then page1 = 1 end
          native_progress = {
            bookId = expectBook,
            chapterUid = expectUid,
            page = page1,
            total = prog.total,
            byteOffset = tonumber(prog.byteOffset) or 0,
            complete = prog.complete and true or false,
          }
          pcall(save_progress_local)
        end
        if open_chapter(idx) then
          return
        end
        -- open_chapter failed: fall through to TOC/shelf recovery.
      end
    end
    local ch = chapters[chapter_idx]
    local expectUid = ch and tostring(ch.chapterUid or "") or ""
    local expectBook = tostring(cur_book.bookId or "")
    local expectKey = "weread:" .. expectBook .. ":" .. expectUid
    local gotUid = tostring(prog.chapterUid or "")
    local gotBook = tostring(prog.bookId or "")
    local gotKey = tostring(prog.progressKey or "")
    -- Prefer current chapter; if chapter_idx drifted, still trust host book+chapterUid.
    local saveBook = expectBook
    local saveUid = expectUid
    local idsOk = (gotBook ~= "" and gotBook == expectBook)
      and (gotUid ~= "" and gotUid == expectUid)
      and (gotKey == "" or gotKey == expectKey)
    if (not idsOk) and gotBook ~= "" and gotUid ~= "" and gotBook == expectBook then
      -- chapter_idx may not match the chapter that was open; trust bridge ids.
      idsOk = true
      saveUid = gotUid
      if not chapter_catalog then
        for i = 1, #chapters do
          if tostring(chapters[i].chapterUid or "") == gotUid then
            chapter_idx = i
            break
          end
        end
      end
      log(string.format("[WR05] onReaderClosed soft_match uid=%s chapter_idx=%s", gotUid, tostring(chapter_idx)))
    end
    if idsOk then
      -- prog.page is 1-based (converted once in native host from 0-based PluginProgress.page).
      local page1 = math.floor(tonumber(prog.page) or 1)
      if page1 < 1 then page1 = 1 end
      reader_page = page1
      local total = prog.total
      local complete = prog.complete and true or false
      local byteOffset = tonumber(prog.byteOffset) or 0
      -- Percent uses the closed chapter's size: after in-reader chapter
      -- switches reader_file_size may still hold an earlier chapter's size.
      if Storage and Storage.chapter_file_size then
        local fsz = Storage.chapter_file_size(saveBook, saveUid)
        if fsz and fsz > 0 then reader_file_size = fsz end
      end
      local percent = nil
      if complete and type(total) == "number" and total > 0 then
        local t = math.floor(total)
        if t < 1 then t = 1 end
        local p = page1
        if p > t then p = t end
        percent = math.floor(p * 100 / t)
      elseif type(reader_file_size) == "number" and reader_file_size > 0 and byteOffset >= 0 then
        percent = math.min(100, math.floor(byteOffset * 100 / reader_file_size))
      end
      native_progress = {
        bookId = saveBook,
        chapterUid = saveUid,
        page = page1,
        total = total,
        byteOffset = byteOffset,
        complete = complete,
        percent = percent,
      }
      pcall(save_progress_local)
      if complete then
        pcall(try_upload_progress)
      end
      log(string.format(
        "[WR05] onReaderClosed saved off=%s page=%s pct=%s",
        tostring(byteOffset), tostring(page1), tostring(percent)))
    else
      log(string.format(
        "[WR05] onReaderClosed drop_progress expect=%s/%s key=%s got=%s/%s key=%s",
        expectBook, expectUid, expectKey, gotBook, gotUid, gotKey))
    end
  end
  cancel_chapter_load()
  -- Prefer system TOC again (no Lua toc repaint race).
  if #chapters > 0 and open_native_toc() then
    status_line = font_ok and "已返回目录" or "back to toc"
    return
  end
  if #chapters > 0 then
    screen = "toc"
  elseif #books > 0 then
    screen = "shelf"
  else
    screen = "login"
  end
  status_line = font_ok and "已返回目录" or "back to toc"
  dirty = true
end

function provider_register_current_book()
  if type(provider) ~= "table" or type(provider.register) ~= "function" then return false end
  if not cur_book or #chapters < 1 then return false end
  if chapter_catalog then
    local spec = Catalog.provider_spec(chapter_catalog)
    if not spec then return false end
    return provider.register({
      providerId = "weread",
      bookId = tostring(cur_book.bookId or ""),
      title = tostring(cur_book.title or ""),
      catalog = spec,
      currentIndex = math.max(0, (chapter_idx or 1) - 1),
    })
  end
  local chs = {}
  for i = 1, #chapters do
    chs[#chs + 1] = {
      uid = tostring(chapters[i].chapterUid or ""),
      title = tostring(chapters[i].title or ""),
    }
  end
  return provider.register({
    providerId = "weread",
    bookId = tostring(cur_book.bookId or ""),
    title = tostring(cur_book.title or ""),
    chapters = chs,
    currentIndex = math.max(0, (chapter_idx or 1) - 1),
  })
end

function provider_set_chapter_ready(bookId, chapterUid, path, index0)
  if type(provider) ~= "table" or type(provider.setChapter) ~= "function" then return end
  provider.setChapter({
    providerId = "weread",
    bookId = tostring(bookId or ""),
    chapterUid = tostring(chapterUid or ""),
    index = index0,
    state = "ready",
    path = path,
    pct = 100,
  })
end

-- Background TOC restore while native reader owns the panel (no paint).
-- Writes toc.json so the system chapter list can list all chapters; does not
-- steal display ownership or require leaving the reader.
function history_bg_restore_toc_step()
  local job = history_bg_restore
  if type(job) ~= "table" or not job.needToc or not job.bookId then return false end
  if not Auth or not Auth.has or not Auth.has() then
    -- Offline / not logged in: leave synthetic TOC; next online session can refresh.
    job.needToc = false
    return false
  end
  if not Api or (type(Api.fetch_toc) ~= "function" and type(Api.fetch_toc_to_file) ~= "function") then
    job.needToc = false
    return false
  end
  if ensure_network and not ensure_network() then
    return false  -- retry next pump
  end
  job.needToc = false
  -- Preserve current chapter index by uid when replacing synthetic list.
  local keepUid = nil
  if chapters and chapters[chapter_idx] then
    keepUid = tostring(chapters[chapter_idx].chapterUid or "")
  end
  local tocRel = Storage.toc_rows_path and Storage.toc_rows_path(job.bookId)
    or ("cache/" .. tostring(job.bookId) .. "/toc_rows.txt")
  local n, err
  if type(dl) == "table" and type(dl.jsonToFile) == "function"
      and type(Api.fetch_toc_to_file) == "function" then
    n, err = Api.fetch_toc_to_file(job.bookId, tocRel)
  end
  if n and tonumber(n) > 0 then
    chapter_catalog = Catalog.spec(tocRel, n, 0, 1, 2)
    toc_source = tocRel
    chapters = Catalog.virtual_rows(chapter_catalog, "weread", job.bookId)
    toc_wordcount_reliable = true
    pcall(Storage.save_catalog_meta, job.bookId, chapter_catalog)
    -- FileRows has no UID scan API; retain the host-provided chapter index.
  else
    local list
    list, err = Api.fetch_toc(job.bookId)
    if type(list) ~= "table" or #list < 1 then
      log(string.format("[WRCP] history_bg_toc fail book=%s err=%s",
        tostring(job.bookId), tostring(err or "?")))
      return false
    end
    chapter_catalog = nil
    chapters = list
    toc_wordcount_reliable = true
    Storage.save_toc(job.bookId, chapters)
    if keepUid and keepUid ~= "" then
      for i = 1, #chapters do
        if tostring(chapters[i].chapterUid or "") == keepUid then
          chapter_idx = i
          break
        end
      end
    end
  end
  pcall(provider_register_current_book)
  log(string.format("[WRCP] history_bg_toc book=%s n=%d", tostring(job.bookId), #chapters))
  return true
end

-- Background prefetch while native reader owns the display (cooperative).
function provider_pump_work()
  -- History cold-open: refresh full TOC to SD so chapter list works mid-read.
  pcall(history_bg_restore_toc_step)

  if type(provider) ~= "table" or type(provider.pollWork) ~= "function" then return end
  if type(Storage) ~= "table" or type(Api) ~= "table" then return end
  local w = provider.pollWork()
  if type(w) ~= "table" or w.type ~= "prefetch" then return end
  local bookId = tostring(w.bookId or "")
  local idx = tonumber(w.index) or -1
  local pid = tostring(w.providerId or "weread")
  if bookId == "" then return end
  -- FileRows work intentionally carries an empty UID. Resolve the bounded row
  -- before touching cache, network, or provider state.
  local chapterUid, title, resolveErr = Catalog.resolve_work(w)
  if not chapterUid or chapterUid == "" then
    log(string.format("[WRCP] prefetch_resolve_fail book=%s idx=%s err=%s",
      bookId, tostring(idx), tostring(resolveErr or "empty_uid")))
    return
  end
  if type(provider.setChapter) == "function" then
    provider.setChapter({
      providerId = pid, bookId = bookId, chapterUid = chapterUid, index = idx,
      title = title, state = "fetching", pct = 5,
    })
  end
  -- Cache hit: mark ready without network.
  if Storage.chapter_file_size then
    local fsz = Storage.chapter_file_size(bookId, chapterUid)
    if fsz and fsz > 0 then
      local path = Storage.chapter_path and Storage.chapter_path(bookId, chapterUid) or nil
      if path then
        provider_set_chapter_ready(bookId, chapterUid, path, idx)
        return
      end
    end
  end
  if not ensure_network or not ensure_network() then
    if type(provider.setChapter) == "function" then
      provider.setChapter({
        providerId = pid, bookId = bookId, chapterUid = chapterUid, index = idx,
        title = title, state = "error", error = "net",
      })
    end
    return
  end
  local text, err = Api.fetch_chapter_text(bookId, { chapterUid = chapterUid })
  if not text then
    if type(provider.setChapter) == "function" then
      provider.setChapter({
        providerId = pid, bookId = bookId, chapterUid = chapterUid, index = idx,
        title = title, state = "error", error = tostring(err or "fetch"),
      })
    end
    return
  end
  local ok_write = Storage.save_chapter_text and Storage.save_chapter_text(bookId, chapterUid, text)
  text = nil
  if collectgarbage then collectgarbage("collect") end
  if not ok_write then
    if type(provider.setChapter) == "function" then
      provider.setChapter({
        providerId = pid, bookId = bookId, chapterUid = chapterUid, index = idx,
        title = title, state = "error", error = "cache",
      })
    end
    return
  end
  local path = Storage.chapter_path and Storage.chapter_path(bookId, chapterUid) or nil
  if path then provider_set_chapter_ready(bookId, chapterUid, path, idx) end
  log(string.format("[WRCP] prefetch_ok book=%s ch=%s idx=%s", bookId, chapterUid, tostring(idx)))
end

function open_native_reader(path, title, bookId, chapterUid)
  if type(reader) ~= "table" or type(reader.openText) ~= "function" then
    return false, "no_reader"
  end
  status_line = font_ok and "打开阅读器…" or "opening reader…"
  -- Keep chapter size for progress % even when ContentProvider path skips paginate.
  if Storage.chapter_file_size then
    local fsz = Storage.chapter_file_size(bookId, chapterUid)
    if fsz and fsz > 0 then reader_file_size = fsz end
  end
  pcall(provider_register_current_book)
  provider_set_chapter_ready(bookId, chapterUid, path, math.max(0, (chapter_idx or 1) - 1))
  local opts = {
    path = path,
    title = title or "",
    bookId = bookId or "",
    chapterUid = chapterUid or "",
    progressKey = "weread:" .. tostring(bookId) .. ":" .. tostring(chapterUid),
    providerId = "weread",
    -- System TOC from in-reader menu: titles from toc.json, not scanning chapter body.
    tocPath = "cache/" .. tostring(bookId) .. "/toc.json",
    chapterIndex = math.max(0, (chapter_idx or 1) - 1),
  }
  -- Restore raw-byte offset when saved progress matches this book+chapter.
  -- Prefer native_progress, then progress.json byteOffset, then page→pidx mapping.
  local restored = false
  local function apply_saved(saved)
    if type(saved) ~= "table" then return false end
    if tostring(saved.chapterUid or "") ~= tostring(chapterUid or "") then return false end
    if type(saved.byteOffset) == "number" and saved.byteOffset >= 0 then
      opts.initialByteOffset = math.floor(saved.byteOffset)
      return true
    end
    -- Older saves only had page (1-based). Map via pidx when available.
    local page1 = tonumber(saved.page)
    if page1 and page1 >= 1 and Storage.load_pidx and Storage.chapter_file_size then
      local fsz = Storage.chapter_file_size(bookId, chapterUid)
      if fsz and fsz > 0 then
        local starts = Storage.load_pidx(bookId, chapterUid, fsz, layout_fp_for_reader and layout_fp_for_reader() or nil)
        if type(starts) == "table" and #starts > 0 then
          local idx = math.floor(page1)
          if idx > #starts then idx = #starts end
          local off = starts[idx]
          if type(off) == "number" and off >= 0 then
            opts.initialByteOffset = math.floor(off)
            return true
          end
        end
      end
    end
    return false
  end
  if type(native_progress) == "table"
     and tostring(native_progress.bookId or "") == tostring(bookId or "") then
    restored = apply_saved(native_progress)
  end
  if not restored then
    restored = apply_saved(Storage.load_progress(bookId))
  end
  if not restored then
    -- Avoid carrying stale native_progress into a different chapter.
    if type(native_progress) == "table"
       and (tostring(native_progress.chapterUid or "") ~= tostring(chapterUid or "")
            or tostring(native_progress.bookId or "") ~= tostring(bookId or "")) then
      native_progress = nil
    end
  end
  log(string.format("[WR05] openText_call t=%s path=%s off=%s",
    tostring(sys.millis()), tostring(path),
    opts.initialByteOffset and tostring(opts.initialByteOffset) or "-"))
  local ok, err = reader.openText(opts)
  if ok then
    -- Accepted: parent yields display to native. No further loading repaints.
    screen = "native_reader"
    native_handoff_t0 = sys.millis()
    dirty = false
    frame_changed = false
    last_loading_frame = nil
    log(string.format("[WR05] openText_accepted t=%s", tostring(sys.millis())))
    return true
  end
  log(string.format("[WR05] openText_fail t=%s err=%s", tostring(sys.millis()), tostring(err)))
  return false, err
end

function init()
  log("weread plugin init v0.6.8 empty-parent-redirect")
  begin_startup("font", 0, "starting")
end

-- Auto login poll during draw() so UI keeps refreshing without key spam.
local _last_draw_poll = 0
local _last_provider_pump = 0
local _orig_draw = draw
function draw()
  if screen == "startup" and startup_job then
    if startup_job.ui_committed then
      advance_startup()
    else
      -- The first call after init is paint-only. AppRuntime displays this
      -- buffer after draw returns, before any storage or network call.
      startup_job.ui_committed = true
      dirty = true
    end
  end
  if network_job then
    if network_job.ui_committed then
      advance_network_job()
    else
      network_job.ui_committed = true
      dirty = true
    end
  end
  if screen == "login" and Auth.login_uid and not Auth.cancelled then
    local now = sys.millis()
    if now - _last_draw_poll > 500 then
      _last_draw_poll = now
      local st = Auth.poll_login_step()
      if st == "ok" then
        load_shelf()
        return
      end
    end
  end
  _orig_draw()
end
