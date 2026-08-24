-- com.fanqie.client — 番茄小说 M4x plugin (legado-style book source).
-- Loads: storage/layout/api/content_provider/ui_* via sys.load

sys.load("storage.lua")
sys.load("catalog.lua")
sys.load("layout.lua")
sys.load("api.lua")
sys.load("content_provider.lua")
sys.load("ui_template.lua")
sys.load("ui_shelf.lua")
sys.load("ui_category.lua")
sys.load("ui_booklist.lua")
sys.load("ui_toc.lua")
sys.load("ui_reader.lua")

UI_METRICS = Layout.metrics()
SHELF_PAGE = UI_METRICS.shelf_page_size
TOC_PAGE = UI_METRICS.toc_page_size
CATEGORY_PAGE = UI_METRICS.toc_page_size
BOOKLIST_PAGE = UI_METRICS.shelf_page_size
READER_MARGIN = 24
READER_LINE = UI_METRICS.reader_line_h

-- Cap TOC list. Each chapter costs ~230B of Lua heap (~1200/ch==275KB,
-- which on top of the ~245KB baseline would overflow the 512KB sandbox).
-- 900 keeps the whole read path inside the heap with margin.
MAX_TOC = 900

-- Static category browser (subset of the book source exploreUrl).
CATEGORIES = {
  { group = "男生", title = "都市", category_id = 1, gender = 1 },
  { group = "男生", title = "玄幻", category_id = 7, gender = 1 },
  { group = "男生", title = "科幻", category_id = 8, gender = 1 },
  { group = "男生", title = "悬疑", category_id = 10, gender = 1 },
  { group = "男生", title = "历史", category_id = 12, gender = 1 },
  { group = "男生", title = "武侠", category_id = 16, gender = 1 },
  { group = "男生", title = "都市生活", category_id = 2, gender = 1 },
  { group = "男生", title = "体育", category_id = 15, gender = 1 },
  { group = "男生", title = "系统", category_id = 19, gender = 1 },
  { group = "男生", title = "神豪", category_id = 20, gender = 1 },
  { group = "男生", title = "种田", category_id = 23, gender = 1 },
  { group = "男生", title = "重生", category_id = 36, gender = 1 },
  { group = "男生", title = "穿越", category_id = 37, gender = 1 },
  { group = "男生", title = "无限流", category_id = 70, gender = 1 },
  { group = "男生", title = "诸天万界", category_id = 71, gender = 1 },
  { group = "男生", title = "末世", category_id = 68, gender = 1 },
  { group = "男生", title = "洪荒", category_id = 66, gender = 1 },
  { group = "男生", title = "网游", category_id = 372, gender = 1 },
  { group = "男生", title = "修仙", category_id = 517, gender = 1 },
  { group = "男生", title = "灵异", category_id = 100, gender = 1 },
  { group = "女生", title = "现代言情", category_id = 3, gender = 0 },
  { group = "女生", title = "古代言情", category_id = 5, gender = 0 },
  { group = "女生", title = "校园", category_id = 4, gender = 0 },
  { group = "女生", title = "幻想言情", category_id = 32, gender = 0 },
  { group = "女生", title = "婚恋", category_id = 34, gender = 0 },
  { group = "女生", title = "萌宝", category_id = 28, gender = 0 },
  { group = "女生", title = "豪门总裁", category_id = 29, gender = 0 },
  { group = "女生", title = "宠妻", category_id = 30, gender = 0 },
  { group = "女生", title = "快穿", category_id = 24, gender = 0 },
  { group = "女生", title = "宫斗宅斗", category_id = 246, gender = 0 },
  { group = "女生", title = "医术", category_id = 247, gender = 0 },
  { group = "女生", title = "玄幻言情", category_id = 248, gender = 0 },
  { group = "女生", title = "甜宠", category_id = 96, gender = 0 },
  { group = "女生", title = "团宠", category_id = 94, gender = 0 },
  { group = "女生", title = "女强", category_id = 86, gender = 0 },
  { group = "出版", title = "名著经典", category_id = 51, genre_type = 160 },
  { group = "出版", title = "中国名著", category_id = 98, genre_type = 160 },
  { group = "出版", title = "外国名著", category_id = 99, genre_type = 160 },
  { group = "出版", title = "国学", category_id = 116, genre_type = 160 },
  { group = "出版", title = "历史传记", category_id = 404, genre_type = 160 },
  { group = "出版", title = "哲学宗教", category_id = 406, genre_type = 160 },
  { group = "出版", title = "心理学", category_id = 407, genre_type = 160 },
  { group = "出版", title = "政治军事", category_id = 408, genre_type = 160 },
}

-- screens: startup | shelf | category | booklist | toc | reader | loading | native_reader | native_toc | message
screen = "startup"
status_line = ""
books = {}
shelf_page = 1
message_title, message_body, message_hint = "", "", "tap/back"
message_action = "exit"
message_code = ""
dirty = true
frame_changed = true
last_loading_frame = nil

startup_job = nil
-- Network work is deferred by one frame so every touch gets a visible
-- loading screen before a synchronous HTTP/JSON operation begins.
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
  if job.kind == "booklist" then
    screen = "category"
    if type(open_category_scene) == "function" then open_category_scene() end
  elseif job.kind == "bookpage" then
    booklist_loading = false
    screen = "booklist"
    open_booklist_scene()
  elseif job.kind == "toc" then
    screen = "shelf"
    if type(open_shelf_scene) == "function" then open_shelf_scene() end
  else
    screen = job.return_screen or "shelf"
  end
  dirty = true
  return true
end

-- Phased like JJWXC advance_network_job: each draw advances at most one
-- phase so a slow Wi-Fi/TLS/TOC hop can never freeze the loading frame for
-- tens of seconds. TOC phases: paint -> cache -> download -> connect -> open.
function advance_network_job()
  local job = network_job
  if type(job) ~= "table" then return end

  if job.kind == "toc" then
    if job.phase == "paint" or job.phase == nil then
      job.phase = "cache"
      status_line = font_ok and "检查目录缓存…" or "checking toc cache…"
      dirty = true
      return
    end
    if job.phase == "cache" then
      -- Cache-only pass; never touches the network in this tick.
      begin_toc_load(job.data)
      if type(network_job) ~= "table" or network_job.kind ~= "toc" then return end
      if network_job.toc_prefetched then
        -- Local catalog/TOC is enough: finish (progress/open) next tick.
        network_job.phase = "open"
        status_line = font_ok and "打开目录…" or "opening toc…"
      else
        -- Needs the network; Wi-Fi bring-up gets its own painted tick below.
        network_job.phase = "download"
        status_line = font_ok and "下载目录…" or "downloading toc…"
      end
      dirty = true
      return
    end
    if job.phase == "download" then
      -- connectSaved can block for tens of seconds: isolate it from the TOC
      -- request so the loading frame stays interactive between the two hops.
      network_job.phase = "connect"
      local online, nerr = ensure_network()
      if not online then
        network_job = nil
        set_message(net_error_title(nerr), net_error_body(nerr),
          font_ok and "点按重试 · 返回书架" or "tap retry / back shelf", "shelf")
        return
      end
      status_line = font_ok and "连接服务器…" or "connecting…"
      dirty = true
      return
    end
    if job.phase == "connect" then
      local book = job.data
      network_job = nil
      finish_toc_load(book, {})
      return
    end
    if job.phase == "open" then
      local book = job.data
      local cached_toc = job.cached_toc
      network_job = nil
      finish_toc_load(book, { toc_prefetched = true, cached_toc = cached_toc })
      return
    end
    -- Unknown/legacy state: behave like the old single-shot path.
    local book = job.data
    network_job = nil
    finish_toc_load(book, {})
    return
  end

  if job.kind == "booklist" then
    network_job = nil
    open_booklist_sync(job.data)
    return
  elseif job.kind == "bookpage" then
    network_job = nil
    fetch_book_page_sync(job.data)
    return
  end

  network_job = nil
end

function draw_startup()
  UiTemplate.page({ UI_METRICS = UI_METRICS }, "FanQie", nil,
    startup_status(startup_job), "back / exit")
end

-- Set by provider.takeResume during startup; applied before shelf when cache exists.
pending_history_resume = nil
history_bg_restore = nil  -- { bookId, needToc }

-- Resolve 1-based chapter index for history reopen.
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

function history_resume_cache_ready()
  local r = pending_history_resume
  if type(r) ~= "table" or not r.bookId or r.bookId == "" then return false end
  if not Storage.chapter_file_size then return false end
  local bookId = tostring(r.bookId)
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

-- Open book from Home m4cp:// resume without shelf UI when cache exists.
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
  book.bookId = tostring(book.bookId or "")
  book.title = tostring(book.title or book.bookId)
  book.author = tostring(book.author or "")
  log(string.format("[FQ] apply_history_resume book=%s ch=%s cache=%s",
    book.bookId, tostring(r.chapterUid or ""), tostring(r.cacheRelPath or "")))

  local toc = Storage.load_toc and Storage.load_toc(book.bookId) or nil
  local hadFullToc = type(toc) == "table" and #toc > 0

  -- Cold history reopen can use the persisted FileRows metadata without
  -- loading toc.json or scanning the catalog in Lua.
  local meta = Storage.load_catalog_meta and Storage.load_catalog_meta(book.bookId) or nil
  local metaSize = meta and type(fs) == "table" and type(fs.fileSize) == "function"
    and fs.fileSize(tostring(meta.source or "")) or nil
  if not hadFullToc and meta and metaSize and metaSize > 0 then
    chapter_catalog = Catalog.spec(meta.source, meta.count, meta.uid_field, meta.title_field)
    toc_source = chapter_catalog.source
    chapters = Catalog.virtual_rows(chapter_catalog, "fanqie", book.bookId)
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
        history_bg_restore = { bookId = book.bookId, needToc = false }
        open_chapter_uid(chapter_idx, uid, tostring(r.title or uid))
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
        history_bg_restore = { bookId = book.bookId, needToc = false }
        open_chapter(chapter_idx)
        return true
      end
    end
    -- TOC present but body missing: try fetch (needs network).
    load_toc(book)
    return true
  end

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
      history_bg_restore = { bookId = book.bookId, needToc = true }
      open_chapter(1)
      return true
    end
  end

  if r.cacheRelPath and tostring(r.cacheRelPath) ~= "" and type(fs.fileSize) == "function" then
    local n = fs.fileSize(tostring(r.cacheRelPath))
    if n and n > 0 then
      local uid = tostring(r.chapterUid or "")
      if uid == "" then
        local base = tostring(r.cacheRelPath):match("([^/]+)$") or ""
        uid = base:match("^ch_(.+)%.txt$") or base:match("^ch_(.+)%.TXT$") or base
      end
      if uid ~= "" then
        chapters = {{ chapterUid = uid, title = tostring(r.title or uid) }}
        cur_book = book
        chapter_idx = 1
        pcall(provider_register_current_book)
        seed_history_native_progress(book.bookId, uid, r)
        history_bg_restore = { bookId = book.bookId, needToc = true }
        open_chapter(1)
        return true
      end
    end
  end

  -- No usable cache: normal load_toc (may require network).
  load_toc(book)
  return true
end

function maybe_history_bg_restore()
  local job = history_bg_restore
  if type(job) ~= "table" or not job.bookId then return end
  if screen == "native_reader" or screen == "loading" or screen == "native_toc" then
    return
  end
  history_bg_restore = nil
  if not cur_book or tostring(cur_book.bookId) ~= tostring(job.bookId) then return end
  if job.needToc then
    pcall(function()
      local file_mode = type(dl) == "table" and type(dl.jsonToFile) == "function"
      if file_mode and type(Api.fetch_toc_to_file) == "function" then
        local tocRel = Storage.toc_rows_path and Storage.toc_rows_path(cur_book.bookId)
          or ("cache/" .. tostring(cur_book.bookId) .. "/toc_rows.txt")
        local n = Api.fetch_toc_to_file(cur_book.bookId, tocRel)
        if n and tonumber(n) > 0 then
          chapter_catalog = Catalog.spec(tocRel, n, 0, 1)
          toc_source = tocRel
          chapters = Catalog.virtual_rows(chapter_catalog, "fanqie", cur_book.bookId)
          Storage.save_catalog_meta(cur_book.bookId, chapter_catalog)
          pcall(provider_register_current_book)
        end
        return
      end
      local list, err = Api.fetch_toc and Api.fetch_toc(cur_book.bookId) or nil
      if type(list) == "table" and #list > 0 then
        if #list > MAX_TOC then
          for i = #list, MAX_TOC + 1, -1 do list[i] = nil end
          if collectgarbage then collectgarbage("collect") end
        end
        chapters = list
        Storage.save_toc(cur_book.bookId, chapters)
        pcall(provider_register_current_book)
        log(string.format("[FQ] history_bg_toc book=%s n=%d", tostring(job.bookId), #list))
      end
    end)
  end
end

function finish_startup_shelf()
  books = merge_local_progress_into_books(Storage.load_shelf() or {})
  shelf_page = 1
  screen = "shelf"
  status_line = font_ok and ("书架 " .. tostring(#books) .. " 本 · 无账号免登录") or "shelf"
  startup_job = nil
  dirty = true
  if try_apply_history_resume() then return end
  open_shelf_scene()
end

function advance_startup()
  local job = startup_job
  if not job then return end
  job.frame = (job.frame or 0) + 1

  if job.phase == "font" then
    refresh_font_info()
    job.phase, job.pct, job.label = "resume", 40, "loading"
  elseif job.phase == "resume" then
    if type(provider) == "table" and type(provider.takeResume) == "function" then
      local resume = provider.takeResume()
      if type(resume) == "table" and resume.bookId and tostring(resume.bookId) ~= "" then
        pending_history_resume = {
          bookId = tostring(resume.bookId),
          title = tostring(resume.title or resume.bookId),
          providerId = tostring(resume.providerId or "fanqie"),
          chapterUid = tostring(resume.chapterUid or ""),
          cacheRelPath = tostring(resume.cacheRelPath or ""),
          chapterIndex = tonumber(resume.chapterIndex),
          byteOffset = tonumber(resume.byteOffset),
        }
        log(string.format("[FQ] history_resume book=%s ch=%s cache=%s",
          pending_history_resume.bookId,
          tostring(pending_history_resume.chapterUid or ""),
          tostring(pending_history_resume.cacheRelPath or "")))
      end
    end
    if pending_history_resume and history_resume_cache_ready() then
      startup_job = nil
      if try_apply_history_resume() then return end
    end
    startup_job = nil
    finish_startup_shelf()
    return
  end
  dirty = true
end

cur_book = nil
chapters = {}
chapter_catalog = nil -- FileRows spec; rows resolve one-at-a-time
toc_source = nil
chapter_uid = ""    -- current chapter uid
toc_page = 1
reader_path = nil
reader_file_size = 0
reader_page_starts = {}
reader_page_chunk = nil
reader_text = nil
reader_pages = {}
reader_page = 1
reader_title = ""
chapter_idx = 1

-- Chapter open job (same UI-first ContentProvider contract as weread).
chapter_job = nil
PAGINATE_OPS_PER_STEP = 40
PAGINATE_SLICE_MS = 120
reader_page_prefer = nil
retry_chapter_idx = nil

font_ok = true
font_hint = ""
font_reader_id = 0
font_available = true

function refresh_font_info()
  if type(sys.fontInfo) ~= "function" then
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
    font_available = (fi.available == nil) or (fi.available and true or false)
  else
    font_ok = true
    font_hint = ""
    font_reader_id = 0
    font_available = true
  end
end

function can_enter_chinese_ui()
  return true
end

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
    PAGE_SIZE = SHELF_PAGE,
    categories = CATEGORIES,
    category_page = category_page,
    CATEGORY_PAGE = CATEGORY_PAGE,
    cur_category = cur_category,
    booklist = booklist,
    booklist_page = booklist_page,
    BOOKLIST_PAGE = BOOKLIST_PAGE,
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
    job.win = nil
    local ch = chapters[chapter_idx]
    if cur_book and ch then
      local meta = {
        size = reader_file_size,
        layout = layout_fp_for_reader(),
      }
      pcall(Storage.save_pidx, cur_book.bookId, ch.chapterUid, meta, reader_page_starts)
    end
  else
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
  return true
end

function step_chapter_load()
  local job = chapter_job
  if not job then return end

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

  if not ContentProvider or type(ContentProvider.step) ~= "function" then
    chapter_load_fail("E_STATE", font_ok and "正文失败" or "Chapter failed",
      font_ok and "ContentProvider 不可用" or "ContentProvider missing")
    return
  end
  local curUid = chapter_uid
  if curUid == "" then
    local ch = chapters[chapter_idx]
    curUid = ch and ch.chapterUid or ""
  end
  if not cur_book or curUid == "" then
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
    -- open_native_reader should have set native_reader; never fake it while still loading.
    if screen == "loading" or screen == "toc" then
      local path = job.path
      if (not path or path == "") and Storage.chapter_path and cur_book then
        path = Storage.chapter_path(cur_book.bookId, job.chapterUid or chapter_uid)
      end
      if path and path ~= "" then
        local ok = open_native_reader(path, job.title or reader_title,
          job.bookId or (cur_book and cur_book.bookId), job.chapterUid or chapter_uid)
        if not ok and (screen == "loading" or screen == "toc") then
          chapter_load_fail("E_HOST", font_ok and "打开失败" or "Open failed",
            font_ok and "正文已下载但无法打开阅读器" or "body ready but openText failed")
          return
        end
      end
    end
    if screen == "native_reader" or screen == "native_toc" then
      dirty = false
      frame_changed = false
    end
    clear_chapter_job()
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
  if job.status and job.status ~= "" then
    status_line = job.status
  end
  if screen ~= "native_reader" and screen ~= "native_toc" then
    screen = "loading"
  end
  dirty = true
end

native_progress = nil

function save_progress_local()
  if not cur_book then return end
  local ch = chapters[chapter_idx]
  local entry = {
    chapterIdx = chapter_idx,
    page = reader_page,
    title = cur_book.title,
    bookId = cur_book.bookId,
    chapterUid = chapter_uid ~= "" and chapter_uid or (ch and ch.chapterUid or ""),
  }
  if type(native_progress) == "table" then
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
      entry.percent = 1
    else
      entry.percent = 0
    end
  end
  Storage.save_progress_entry(cur_book.bookId, entry)
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

function merge_local_progress_into_books(list)
  if type(list) ~= "table" then return list end
  for i = 1, #list do
    local b = list[i]
    if b and b.bookId then
      -- Shelf/history records from older plugin versions may omit display
      -- fields. Normalize them before either Lua fallback UI or host scenes
      -- consume the record.
      b.bookId = tostring(b.bookId)
      b.title = tostring(b.title or b.bookId)
      b.author = tostring(b.author or "")
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

-- Add/refresh book in the local shelf.
function shelf_add_book(book)
  if not book or not book.bookId then return end
  book.bookId = tostring(book.bookId)
  book.title = tostring(book.title or book.bookId)
  book.author = tostring(book.author or "")
  local list = Storage.load_shelf() or {}
  local found = false
  for i = 1, #list do
    if list[i] and tostring(list[i].bookId or "") == tostring(book.bookId) then
      list[i].bookId = tostring(list[i].bookId or book.bookId)
      list[i].title = tostring(book.title or list[i].title or list[i].bookId)
      list[i].author = tostring(book.author or list[i].author or "")
      found = true
      break
    end
  end
  if not found then
    list[#list + 1] = { bookId = book.bookId, title = book.title or "", author = book.author or "" }
  end
  Storage.save_shelf(list)
  books = merge_local_progress_into_books(list)
end

-- Screens: category browser / book list state.
-- category/booklist/toc are rendered by the HOST via ui.list* (host owns
-- pagination, footer buttons and input); Lua only keeps the data + callbacks.
category_page = 1
cur_category = nil
booklist = {}
booklist_api_page = 1    -- current fetched API page (15 books)
booklist_page = 1        -- legacy Lua-fallback render page (host scenes ignore it)
booklist_loading = false
booklist_has_next = true  -- API page may be fetched on demand
-- Session cache of visited API pages (page -> {list, has_next}).  Refetching
-- a page we already saw over the network made every back-flip slow; each
-- cached page is ~1KB of Lua heap (6 books), so keeping a bounded window is
-- negligible.  The cache is dropped when the category changes.
BOOKLIST_CACHE_MAX = 8
booklist_cache = {}
booklist_cache_order = {}
BOOKLIST_FETCH_SIZE = 6
toc_book = nil           -- book whose toc is open in a ui.list scene

local function booklist_cache_put(page, list, has_next)
  if not booklist_cache[page] then
    booklist_cache[page] = { list = list, has_next = has_next and true or false }
    booklist_cache_order[#booklist_cache_order + 1] = page
    if #booklist_cache_order > BOOKLIST_CACHE_MAX then
      booklist_cache[table.remove(booklist_cache_order, 1)] = nil
    end
  end
end

local function booklist_cache_get(page)
  return booklist_cache[page]
end

local function booklist_cache_clear()
  booklist_cache = {}
  booklist_cache_order = {}
end

-- The shelf uses the same host-owned list template as category/book/TOC.
-- This keeps typography, row spacing, footer navigation and touch hitboxes
-- identical across providers. The Lua renderer remains only as an old-host
-- fallback for firmware without ui.listOpen.
function shelf_rows()
  local rows = {{ title = "分类浏览", sub = "按分类查找书籍" }}
  for i = 1, #books do
    local b = books[i]
    local meta = (b.author and b.author ~= "" and (b.author .. " · ") or "")
      .. tostring(b.progress or 0) .. "%"
    rows[#rows + 1] = { title = b.title or "", sub = meta }
  end
  return rows
end

function open_shelf_scene()
  if type(ui) ~= "table" or type(ui.listOpen) ~= "function" then
    dirty = true
    return false
  end
  ui.listOpen({
    title = "番茄小说",
    rows = shelf_rows(),
    page_size = 8,
    footer = tostring(#books) .. " 本 · 点书打开目录",
    on_row = "on_shelf_row",
    on_back = "on_shelf_back",
  })
  return true
end

function on_shelf_row(idx, title, sub)
  ui.listClose()
  if tonumber(idx) == 0 then
    open_category_screen()
    return
  end
  -- Row 0 is the synthetic category entry; subsequent host rows map to
  -- books[1..n] without an extra offset.
  local b = books[tonumber(idx) or -1]
  if b then load_toc(b) end
end

function on_shelf_back()
  ui.listClose()
  sys.exit()
end

-- Memory self-protection: refuse heavy work when the Lua heap is nearly gone
-- instead of letting the sandbox OOM mid-flight. dl.jsonGet parses on the host
-- (does not consume Lua heap), so the threshold only guards json.decode paths.
function ensure_mem(min_headroom)
  if type(sys.memInfo) ~= "function" then return true end
  local mi = sys.memInfo()
  if type(mi) == "table" and tonumber(mi.lua_headroom or 0) < tonumber(min_headroom or 0) then
    log("[FQ] ensure_mem block headroom=" .. tostring(mi.lua_headroom or "?")
      .. " need=" .. tostring(min_headroom or "?"))
    return false
  end
  return true
end

-- Category browser (host list scene).
function open_category_screen()
  local rows = {}
  for i = 1, #CATEGORIES do
    rows[#rows + 1] = { title = CATEGORIES[i].title, sub = "[" .. tostring(CATEGORIES[i].group) .. "]" }
  end
  if type(ui) ~= "table" or type(ui.listOpen) ~= "function" then
    -- Old host without ui.list: fall back to the Lua category screen.
    screen = "category"
    category_page = 1
    dirty = true
    return
  end
  ui.listOpen({
    title = "分类浏览",
    rows = rows,
    page_size = 14,
    footer = "左右翻页 · 点分类看书 · 顶栏返回",
    on_row = "on_category_row",
    on_back = "on_category_back",
  })
end

function on_category_row(idx, title, sub)
  ui.listClose()
  local cat = CATEGORIES[idx + 1]
  if cat then open_booklist(cat) end
end

function on_category_back()
  ui.listClose()
  screen = "shelf"
  dirty = true
  open_shelf_scene()
end

-- Booklist for one category (host list scene; API pages appended on demand).
function open_booklist(cat)
  if type(cat) ~= "table" then return false end
  cur_category = cat
  booklist = {}
  if collectgarbage then collectgarbage("collect") end
  booklist_api_page = 1
  booklist_loading = false
  begin_network_job("booklist", cat,
    font_ok and "加载书单…" or "loading book list…",
    font_ok and "正在获取分类书籍，请稍候…" or "fetching books for this category…")
  return true
end

function open_booklist_sync(cat)
  cur_category = cat
  booklist = {}
  booklist_cache_clear()
  if collectgarbage then collectgarbage("collect") end
  status_line = font_ok and "联网获取书单…" or "fetching book list…"
  local online, nerr = ensure_network()
  if not online then
    set_message(net_error_title(nerr), net_error_body(nerr),
      font_ok and "点按重试 · 返回书架" or "tap retry / back shelf", "retry_category")
    return
  end
  if not ensure_mem(16384) then
    set_message(font_ok and "内存不足" or "low memory", font_ok and "请关闭其他应用后重试" or "restart the app",
      font_ok and "点按返回" or "tap back", "shelf")
    return
  end
  local list, err = Api.fetch_category(cat.category_id, cat.gender, 1, cat.genre_type, 6)
  if not list then
    local title = font_ok and "书单失败" or "List failed"
    local body = tostring(err or "unknown")
    if err == "wifi_not_connected" then
      title, body = net_error_title(err), net_error_body(err)
    end
    set_message(title, body, font_ok and "点按重试 · 返回书架" or "tap retry / back shelf", "retry_category")
    return
  end
  booklist = list
  booklist_api_page = 1
  booklist_has_next = #list >= BOOKLIST_FETCH_SIZE
  if type(ui) ~= "table" or type(ui.listOpen) ~= "function" then
    booklist_page = 1
    screen = "booklist"
    dirty = true
    return
  end
  open_booklist_scene()
end

function booklist_rows()
  local rows = {}
  for i = 1, #booklist do
    rows[#rows + 1] = { title = booklist[i].title, sub = tostring(booklist[i].author or "") }
  end
  return rows
end

-- Category results are remote pages.  The host renders only the currently
-- resident API page while keeping the logical page count in the footer; this
-- avoids padding earlier pages with blank sentinel rows (and avoids retaining
-- every fetched book in the Lua heap).
function booklist_scene_rows()
  local rows = booklist_rows()
  local page = math.max(1, tonumber(booklist_api_page) or 1)
  local total_pages = page + (booklist_has_next and 1 or 0)
  return rows, total_pages
end

function open_booklist_scene()
  if type(ui) ~= "table" or type(ui.listOpen) ~= "function" or not cur_category then return false end
  local rows, total_pages = booklist_scene_rows()
  ui.listOpen({
    title = tostring(cur_category.title),
    rows = rows,
    page_size = BOOKLIST_FETCH_SIZE,
    page_count = total_pages,
    initial_page = booklist_api_page,
    remote_page = true,
    footer = "第" .. tostring(booklist_api_page) .. "页 · "
      .. tostring(#booklist) .. " 本 · 翻页继续加载",
    on_row = "on_book_row",
    on_page = "on_book_page",
    on_back = "on_book_back",
  })
  return true
end

function on_book_row(idx, title, sub)
  if tostring(title or "") == "" then return end
  ui.listClose()
  local local_idx = (tonumber(idx) or -1) % BOOKLIST_FETCH_SIZE + 1
  local b = booklist[local_idx]
  if b then load_toc(b) end
end

function fetch_book_page_sync(job)
  local direction = type(job) == "table" and job.direction or "next"
  local page = direction == "prev" and booklist_api_page - 1 or booklist_api_page + 1
  if page < 1 then page = 1 end

  -- Keep only the visible API page.  Holding the old page, the new decoded
  -- response, a merged table, and UI rows at the same time is what made page
  -- two hit Lua's low-memory guard.  Going back simply fetches the previous
  -- page again, which is bounded and deterministic.
  booklist = {}
  if collectgarbage then collectgarbage("collect") end
  local online, nerr = ensure_network()
  if not online then
    booklist_loading = false
    set_message(net_error_title(nerr), net_error_body(nerr),
      font_ok and "点按重试 · 返回书架" or "tap retry / back shelf", "retry_category")
    return
  end
  if not ensure_mem(16384) then
    booklist_loading = false
    set_message(font_ok and "内存不足" or "low memory",
      font_ok and "请关闭其他应用后重试" or "restart the app",
      font_ok and "点按重试 · 返回书架" or "tap retry / back shelf", "retry_category")
    return
  end
  local list, err = Api.fetch_category(cur_category.category_id, cur_category.gender,
    page, cur_category.genre_type, 6)
  booklist_loading = false
  if not list then
    if err == "empty" then
      -- True end of the category: the previous page was the last one.  Stay
      -- put and drop the next-page affordance instead of showing an error.
      booklist_has_next = false
      open_booklist_scene()
      return
    end
    set_message(font_ok and "书单加载失败" or "book list failed", tostring(err or "unknown"),
      font_ok and "点按重试 · 返回书架" or "tap retry / back shelf", "retry_category")
    return
  end
  booklist_cache_put(page, list, #list >= BOOKLIST_FETCH_SIZE)
  booklist = list
  booklist_api_page = page
  booklist_has_next = #list >= BOOKLIST_FETCH_SIZE
  if collectgarbage then collectgarbage("collect") end
  open_booklist_scene()
end

function on_book_page(page, total)
  if booklist_loading or type(cur_category) ~= "table" then return end
  local direction = nil
  if page > booklist_api_page and booklist_has_next then
    direction = "next"
  elseif page < booklist_api_page and booklist_api_page > 1 then
    direction = "prev"
  end
  if not direction then return end
  -- Revisiting a page loaded earlier this session: no network, instant.
  local cached = booklist_cache_get(page)
  if cached then
    booklist = cached.list
    booklist_api_page = page
    booklist_has_next = cached.has_next
    ui.listClose()
    open_booklist_scene()
    return
  end
  -- Remember the page we are leaving so a revisit can skip the network.
  booklist_cache_put(booklist_api_page, booklist, booklist_has_next)
  booklist_loading = true
  ui.listClose()
  begin_network_job("bookpage", { direction = direction },
    direction == "next"
      and (font_ok and "加载下一页…" or "loading next page…")
      or (font_ok and "加载上一页…" or "loading previous page…"),
    font_ok and "正在获取更多书籍，请稍候…" or "fetching more books…")
end

function on_book_back()
  ui.listClose()
  screen = "shelf"
  dirty = true
  open_shelf_scene()
end

-- Load a book's TOC. FileRows keeps the complete UID/title catalog on SD;
-- Lua resolves only the row currently being opened or prefetched.
function load_toc(book)
  if type(book) ~= "table" then return false end
  if not cur_book or not book or cur_book.bookId ~= book.bookId then
    native_progress = nil
  end
  cur_book = book
  chapters = {}
  chapter_catalog = nil
  chapter_uid = ""
  toc_page = 1
  status_line = font_ok and "加载目录..." or "loading toc..."
  begin_network_job("toc", book,
    font_ok and "加载目录…" or "loading table of contents…",
    font_ok and "正在读取章节目录，请稍候…" or "fetching chapter list…")
  return true
end

-- Cache-only half of the TOC load. Runs inside an already-painted tick and
-- never touches the network. Arms a follow-up toc job; when local metadata is
-- enough the job is flagged toc_prefetched so finish_toc_load skips download.
function begin_toc_load(book)
  if type(book) ~= "table" then return false end
  -- Switching book: drop stale native_progress so it cannot be written under another chapter.
  if not cur_book or cur_book.bookId ~= book.bookId then
    native_progress = nil
  end
  cur_book = book
  chapters = {}
  chapter_catalog = nil
  chapter_uid = ""
  toc_page = 1
  status_line = font_ok and "加载目录..." or "loading toc..."
  local function rearm()
    begin_network_job("toc", book,
      font_ok and "加载目录…" or "loading table of contents…",
      font_ok and "正在读取章节目录，请稍候…" or "fetching chapter list…")
  end

  -- Prefer a valid local catalog/TOC before touching the network.  Chapter
  -- metadata is durable and can be reused by history/offline opens just like
  -- cached chapter bodies.
  local meta = Storage.load_catalog_meta and Storage.load_catalog_meta(book.bookId) or nil
  local metaSize = meta and type(fs) == "table" and type(fs.fileSize) == "function"
    and fs.fileSize(tostring(meta.source or "")) or nil
  if meta and metaSize and metaSize > 0 then
    chapter_catalog = Catalog.spec(meta.source, meta.count, meta.uid_field, meta.title_field)
    toc_source = chapter_catalog.source
    chapters = Catalog.virtual_rows(chapter_catalog, "fanqie", book.bookId)
    pcall(provider_register_current_book)
    rearm()
    network_job.toc_prefetched = true
    return true
  end
  local cached = Storage.load_toc(book.bookId)
  if type(cached) == "table" and #cached > 0 then
    rearm()
    network_job.toc_prefetched = true
    network_job.cached_toc = cached
    return true
  end
  -- Needs the network; advance_network_job drives download/connect next.
  rearm()
  return true
end

-- Network half of the TOC load. Called from a phased tick that has already
-- painted; performs at most one blocking TOC request plus local SD writes.
function finish_toc_load(book, opts)
  opts = opts or {}
  -- Switching book: drop stale native_progress so it cannot be written under another chapter.
  if not cur_book or not book or cur_book.bookId ~= book.bookId then
    native_progress = nil
  end
  cur_book = book
  chapter_uid = ""
  toc_page = 1
  status_line = font_ok and "联网读取目录…" or "fetching table of contents…"

  local tocRel = Storage.toc_rows_path and Storage.toc_rows_path(book.bookId)
    or ("cache/" .. tostring(book.bookId) .. "/toc_rows.txt")

  -- Honor begin_toc_load BEFORE resetting anything: when it armed
  -- toc_prefetched, the FileRows catalog + virtual chapters are already live
  -- and must survive into the shelf/progress/open tail below. Wipe only for a
  -- real network pass.
  local cached_toc = opts.cached_toc
  local have_cached_json = type(cached_toc) == "table" and #cached_toc > 0
  local prefetched = opts.toc_prefetched and true or false
  if prefetched and have_cached_json then
    chapters = cached_toc
    chapter_catalog = nil
  elseif not prefetched then
    chapters = {}
    chapter_catalog = nil
  end
  toc_source = tocRel
  local list, err, loaded = nil, nil, false
  local file_mode = type(dl) == "table" and type(dl.jsonToFile) == "function"

  if prefetched and (have_cached_json or chapter_catalog) then
    -- Local metadata already decided in begin_toc_load: no fetch on this path.
    loaded = true
  end

  if not loaded then
    if file_mode then
      local n, ferr = Api.fetch_toc_to_file(book.bookId, tocRel)
      if n and tonumber(n) > 0 then
        chapter_catalog = Catalog.spec(tocRel, n, 0, 1)
        chapters = Catalog.virtual_rows(chapter_catalog, "fanqie", book.bookId)
        pcall(Storage.save_catalog_meta, book.bookId, chapter_catalog)
        loaded = true
        err = nil
      else
        err = ferr
      end
    else
      list, err = Api.fetch_toc(book.bookId)  -- legacy: full chapters in Lua
      if list and #list > MAX_TOC then
        for i = #list, MAX_TOC + 1, -1 do list[i] = nil end
        if collectgarbage then collectgarbage("collect") end
      end
      chapters = list or {}
      loaded = type(list) == "table" and #list > 0
      if loaded and type(Storage.save_toc) == "function" then
        -- Legacy json.decode hosts do not persist in Api.fetch_toc itself.
        -- Save the bounded result here so the next open is offline/local-first.
        pcall(Storage.save_toc, book.bookId, chapters)
      end
    end
  end
  if not loaded then
    local title = font_ok and "目录失败" or "TOC failed"
    local body = tostring(err or "unknown")
    if err == "wifi_not_connected" then
      title, body = net_error_title(err), net_error_body(err)
    end
    set_message(title, body, font_ok and "点按回书架" or "tap to shelf", "shelf")
    return false
  end
  if collectgarbage then collectgarbage("collect") end
  shelf_add_book(cur_book)

  local totalCh = #chapters
  local prog = Storage.load_progress(book.bookId)
  if prog and prog.chapterIdx and prog.chapterIdx >= 1 and prog.chapterIdx <= totalCh then
    chapter_idx = prog.chapterIdx
  elseif book.progressChapterUid and book.progressChapterUid ~= "" then
    chapter_idx = 1
    if not chapter_catalog then
      for i = 1, totalCh do
        local uid = chapters[i] and chapters[i].chapterUid or ""
        if uid == book.progressChapterUid then chapter_idx = i; break end
      end
    end
  else
    chapter_idx = 1
  end
  toc_page = math.floor((chapter_idx - 1) / TOC_PAGE) + 1
  status_line = tostring(totalCh) .. (font_ok and " 章 · " or " ch · ") .. tostring(cur_book.title or cur_book.bookId or "")
  dirty = true
  -- FileRows books use the same native chapter picker as WeRead.  Register
  -- the catalog before the handoff so the owner task can resolve titles from
  -- SD without copying the virtual Lua rows or painting a second list UI.
  if chapter_catalog and open_native_toc() then
    return true
  end
  -- Host-owned file-backed chapter list (virtual memory; any size).
  if chapter_catalog and type(ui) == "table" and type(ui.listOpenFile) == "function" then
    ui.listOpenFile({
      title = tostring(cur_book.title),
      source = tocRel,
      page_size = 14,
      footer = status_line,
      on_row = "on_toc_row",
      on_back = "on_toc_back",
    })
    return true
  end
  -- Legacy Lua toc (host without ui.list; chapters loaded into Lua).
  if type(ui) == "table" and type(ui.listOpen) == "function" and #chapters > 0 then
    ui.listOpen({
      title = tostring(cur_book.title),
      rows = chapters,
      page_size = 14,
      footer = status_line,
      on_row = "on_toc_row",
      on_back = "on_toc_back",
    })
    return true
  end
  screen = "toc"
  return true
end

-- Native system TOC for provider-managed FileRows catalogs.  The host owns
-- the e-ink panel and returns a 0-based chapter index through onTocClosed.
function open_native_toc()
  if not cur_book or #chapters < 1 or type(reader) ~= "table"
      or type(reader.openToc) ~= "function" then
    return false
  end
  local opts = {
    bookId = tostring(cur_book.bookId or ""),
    title = tostring(cur_book.title or "目录"),
    currentIndex = math.max(0, (chapter_idx or 1) - 1),
  }
  if chapter_catalog then
    pcall(provider_register_current_book)
    opts.providerId = "fanqie"
  else
    local tocPath = "cache/" .. tostring(cur_book.bookId or "") .. "/toc.json"
    if type(fs) ~= "table" or type(fs.fileSize) ~= "function"
        or (fs.fileSize(tocPath) or 0) <= 0 then
      return false
    end
    opts.tocPath = tocPath
  end
  local ok, err = reader.openToc(opts)
  if ok then
    screen = "native_toc"
    dirty = false
    frame_changed = false
    log(string.format("[FQ] openToc_accepted t=%s ch=%d", tostring(sys.millis()), chapter_idx or 0))
    return true
  end
  log(string.format("[FQ] openToc_fail t=%s err=%s", tostring(sys.millis()), tostring(err)))
  return false
end

function onTocClosed(r)
  log(string.format("[FQ] onTocClosed t=%s", tostring(sys.millis())))
  if screen == "native_toc" then
    screen = "loading"
    dirty = true
    frame_changed = true
    last_loading_frame = nil
  end
  if type(r) == "table" and not r.cancelled and type(r.chapterIndex) == "number" then
    local idx = math.floor(r.chapterIndex) + 1
    if idx >= 1 and idx <= #chapters and open_chapter(idx) then return end
  end
  -- Back from the native picker returns to the same shelf/list entry point;
  -- never leave a host-owned list scene active behind the Lua frame.
  screen = "shelf"
  dirty = true
  open_shelf_scene()
end

-- File-mode row tap: host passes the row's fields (index0, itemId, title).
function on_toc_row(idx, itemId, title)
  ui.listClose()
  local rowIdx = (tonumber(idx) or -1) + 1
  local uid = tostring(itemId or "")
  local rowTitle = tostring(title or "")
  if uid == "" and chapter_catalog then
    local row = chapters[rowIdx]
    uid = row and tostring(row.chapterUid or "") or ""
    rowTitle = row and tostring(row.title or "") or ""
  end
  open_chapter_uid(rowIdx, uid, rowTitle)
end

function on_toc_back()
  ui.listClose()
  screen = "shelf"
  dirty = true
  open_shelf_scene()
end

function open_chapter_uid(idx, uid, title, prefer)
  if not cur_book or not uid or uid == "" then return false end
  if screen == "native_reader" then
    return false
  end
  cancel_chapter_load()
  if type(native_progress) == "table" then
    local same = tostring(native_progress.bookId or "") == tostring(cur_book.bookId or "")
      and tostring(native_progress.chapterUid or "") == tostring(uid or "")
    if not same then
      native_progress = nil
    end
  end
  chapter_idx = math.max(1, tonumber(idx) or 1)
  retry_chapter_idx = chapter_idx
  chapter_uid = tostring(uid)
  reader_title = tostring(title or uid)
  reader_page_prefer = prefer
  status_line = font_ok and "准备打开…" or "preparing…"
  screen = "loading"
  local path = Storage.chapter_path(cur_book.bookId, uid)
  chapter_job = ContentProvider.begin({
    bookId = cur_book.bookId,
    chapterUid = uid,
    title = reader_title,
    path = path,
    prefer = prefer,
    status = status_line,
  })
  log(string.format("[FQ] chapter_tap t=%s book=%s ch=%s ui_first=1",
    tostring(sys.millis()), tostring(cur_book.bookId), tostring(uid)))
  dirty = true
  return true
end

function open_chapter(idx, prefer)
  if not cur_book or idx < 1 then return false end
  if screen == "native_reader" then
    return false
  end
  local row = chapters[idx]
  local uid = row and tostring(row.chapterUid or "") or ""
  local title = row and tostring(row.title or row.chapterUid or "") or ""
  if uid == "" then return false end
  return open_chapter_uid(idx, uid, title, prefer)
end

function handle_message_action()
  local a = message_action or "exit"
  if a == "retry_category" then
    if cur_category then
      open_booklist(cur_category)
    else
      open_category_screen()
    end
  elseif a == "booklist" then
    screen = "booklist"
    dirty = true
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
  elseif a == "shelf" then
    screen = "shelf"
    dirty = true
  else
    if #chapters > 0 then
      screen = "toc"
      dirty = true
    elseif #books > 0 then
      screen = "shelf"
      dirty = true
    else
      screen = "shelf"
      dirty = true
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
  if screen == "native_reader" or screen == "native_toc" then
    dirty = false
    frame_changed = false
    return
  end

  local paint_only = false
  if chapter_job and ContentProvider and ContentProvider.should_paint_only then
    paint_only = ContentProvider.should_paint_only(chapter_job)
  elseif chapter_job and not chapter_job.ui_committed then
    paint_only = true
  end

  if chapter_job and not paint_only then
    advance_chapter_load_slice()
  end

  if screen == "native_reader" or screen == "native_toc" then
    dirty = false
    frame_changed = false
    return
  end

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
  elseif screen == "shelf" then
    UiShelf.draw(st)
  elseif screen == "category" then
    UiCategory.draw(st)
  elseif screen == "booklist" then
    UiBooklist.draw(st)
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
  dirty = keep_pumping
end

function onKey(key)
  if key == "back" or key == "left" then
    if screen == "startup" then
      startup_job = nil
      sys.exit()
      return
    end
    if screen == "native_reader" or screen == "native_toc" then
      return
    end
    if screen == "loading" then
      if network_job then cancel_network_job() end
      if chapter_job then
        cancel_chapter_load()
        screen = "toc"
      end
      dirty = true
      return
    end
    if screen == "reader" then
      save_progress_local()
      -- Reuse the system chapter picker from the legacy Lua reader too.
      -- Falling straight to screen="toc" leaves the old plugin list visible
      -- when a history/cache reopen has not entered the native reader path.
      if not open_native_toc() then
        screen = "toc"
        dirty = true
      end
    elseif screen == "toc" or screen == "booklist" or screen == "category" then
      -- Legacy Lua screens (host ui.list is preferred); always land on shelf.
      screen = "shelf"
      dirty = true
    elseif screen == "message" then
      if message_action == "retry_chapter" and #chapters > 0 then
        cancel_chapter_load()
        screen = "toc"
        dirty = true
        return
      end
      if message_action == "booklist" then
        screen = "booklist"
        dirty = true
      elseif #chapters > 0 then
        screen = "toc"
        dirty = true
      else
        screen = "shelf"
        dirty = true
      end
    else
      sys.exit()
    end
    return
  end
  if key == "confirm" or key == "right" or key == "center" then
    if screen == "native_reader" or screen == "native_toc" then
      return
    elseif screen == "loading" then
      if network_job then cancel_network_job() end
      if chapter_job then
        cancel_chapter_load()
        screen = "toc"
      end
      dirty = true
    elseif screen == "shelf" then
      if shelf_page < UiShelf.page_count(books, SHELF_PAGE) then
        shelf_page = shelf_page + 1
        dirty = true
      end
    elseif screen == "category" or screen == "booklist" or screen == "toc" then
      -- Legacy Lua paging (host ui.list preferred): no-op on new hosts.
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
  local w, h = gui.width(), gui.height()

  if screen == "message" then
    handle_message_action()
    return
  end

  if screen == "native_reader" or screen == "native_toc" then
    return
  end

  if screen == "loading" then
    if network_job then cancel_network_job() end
    if chapter_job then
      cancel_chapter_load()
      screen = "toc"
    end
    dirty = true
    return
  end

  if screen == "shelf" then
    local m = UI_METRICS
    if y < m.content_y then return end
    if Layout.home_footer_hit(y, m) then
      if x < w / 2 then
        if shelf_page > 1 then shelf_page = shelf_page - 1; dirty = true end
      else
        if shelf_page < UiShelf.page_count(books, SHELF_PAGE) then
          shelf_page = shelf_page + 1
          dirty = true
        end
      end
      return
    end
    -- Row 1 is the category entry.
    local rows = UiShelf.slice(books, shelf_page, SHELF_PAGE)
    local row = Layout.home_row_from_point(y, m, #rows + 1, m.shelf_row_h)
    if not row then return end
    if row == 1 then
      open_category_screen()
      return
    end
    local idx = row - 1
    if idx >= 1 and idx <= #rows then load_toc(rows[idx]) end
    return
  end

  -- Legacy Lua category/booklist/toc screens (old hosts without ui.list):
  -- keep minimal paging through the same data the host scenes would use.
  if screen == "category" then
    local m = UI_METRICS
    if y < m.content_y then
      screen = "shelf"
      dirty = true
      return
    end
    if Layout.home_footer_hit(y, m) then
      if x < w / 2 then
        if category_page > 1 then category_page = category_page - 1; dirty = true end
      elseif category_page < UiCategory.page_count(CATEGORIES, CATEGORY_PAGE) then
        category_page = category_page + 1; dirty = true
      end
      return
    end
    local rows, start = UiCategory.slice(CATEGORIES, category_page, CATEGORY_PAGE)
    local idx = Layout.home_row_from_point(y, m, #rows, m.toc_row_h)
    if idx then open_booklist(rows[idx]) end
    return
  end

  if screen == "booklist" then
    local m = UI_METRICS
    if y < m.content_y then
      screen = "shelf"
      dirty = true
      return
    end
    if Layout.home_footer_hit(y, m) then
      if x < w / 2 then
        if booklist_page > 1 then booklist_page = booklist_page - 1; dirty = true end
      elseif booklist_page < UiBooklist.page_count(booklist, BOOKLIST_PAGE) then
        booklist_page = booklist_page + 1; dirty = true
      end
      return
    end
    local rows, start = UiBooklist.slice(booklist, booklist_page, BOOKLIST_PAGE)
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
      elseif toc_page < UiToc.page_count(chapters, TOC_PAGE) then
        toc_page = toc_page + 1; dirty = true
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
      -- The top touch target is the chapter-list affordance. Prefer the
      -- system picker so history-opened books never fall back to the stale
      -- plugin Lua list scene; old hosts still get the legacy fallback.
      if not open_native_toc() then
        screen = "toc"
        dirty = true
      end
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
  log(string.format("[FQ] onReaderClosed t=%s", tostring(sys.millis())))
  clear_chapter_job()
  if screen == "native_reader" then
    screen = "loading"
    dirty = true
    frame_changed = true
    last_loading_frame = nil
  end
  pcall(maybe_history_bg_restore)
  if type(prog) == "table" and cur_book then
    if prog.openFailed or (type(prog.error) == "string" and prog.error ~= "") then
      status_line = font_ok and ("打开失败: " .. tostring(prog.error or "error"))
        or ("open failed: " .. tostring(prog.error or "error"))
      screen = "toc"
      dirty = true
      return
    end
    local ch = chapters[chapter_idx]
    local expectUid = chapter_uid ~= "" and chapter_uid or (ch and tostring(ch.chapterUid or "") or "")
    local expectBook = tostring(cur_book.bookId or "")
    local gotUid = tostring(prog.chapterUid or "")
    local gotBook = tostring(prog.bookId or "")
    local saveBook = expectBook
    local saveUid = expectUid
    local idsOk = (gotBook ~= "" and gotBook == expectBook)
      and (gotUid ~= "" and gotUid == expectUid)
    if (not idsOk) and gotBook ~= "" and gotUid ~= "" and gotBook == expectBook then
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
      chapter_uid = gotUid
      log(string.format("[FQ] onReaderClosed soft_match uid=%s chapter_idx=%s", gotUid, tostring(chapter_idx)))
    end
    if idsOk then
      local page1 = math.floor(tonumber(prog.page) or 1)
      if page1 < 1 then page1 = 1 end
      reader_page = page1
      local total = prog.total
      local complete = prog.complete and true or false
      local byteOffset = tonumber(prog.byteOffset) or 0
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
      log(string.format("[FQ] onReaderClosed saved off=%s page=%s pct=%s",
        tostring(byteOffset), tostring(page1), tostring(percent)))
    else
      log(string.format("[FQ] onReaderClosed drop_progress expect=%s/%s got=%s/%s",
        expectBook, expectUid, gotBook, gotUid))
    end
    -- Native reader chapter picker returns a 0-based target index. Save the
    -- outgoing chapter above, then reopen the selected row through FileRows.
    if type(prog.switchChapterIndex) == "number" then
      local nextIdx = math.floor(prog.switchChapterIndex) + 1
      if nextIdx >= 1 and nextIdx <= #chapters and open_chapter(nextIdx) then
        return
      end
    end
  end
  cancel_chapter_load()
  if #chapters > 0 then
    screen = "toc"
  elseif #books > 0 then
    screen = "shelf"
  else
    screen = "shelf"
  end
  status_line = font_ok and "已返回目录" or "back to toc"
  dirty = true
end

function provider_register_current_book()
  if type(provider) ~= "table" or type(provider.register) ~= "function" then return false end
  if not cur_book then return false end
  if chapter_catalog then
    local spec = Catalog.provider_spec(chapter_catalog)
    if not spec then return false end
    return provider.register({
      providerId = "fanqie",
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
  if #chs < 1 then return false end
  return provider.register({
    providerId = "fanqie",
    bookId = tostring(cur_book.bookId or ""),
    title = tostring(cur_book.title or ""),
    chapters = chs,
    currentIndex = math.max(0, (chapter_idx or 1) - 1),
  })
end

function provider_set_chapter_ready(bookId, chapterUid, path, index0)
  if type(provider) ~= "table" or type(provider.setChapter) ~= "function" then return end
  provider.setChapter({
    providerId = "fanqie",
    bookId = tostring(bookId or ""),
    chapterUid = tostring(chapterUid or ""),
    index = index0,
    state = "ready",
    path = path,
    pct = 100,
  })
end

function history_bg_restore_toc_step()
  local job = history_bg_restore
  if type(job) ~= "table" or not job.needToc or not job.bookId then return false end
  -- Only one heavy network operation per pump tick: a prefetch hop in flight
  -- owns the TLS/network buffers this tick (same rule as JJWXC pump).
  if type(prefetch_job) == "table" then return false end
  -- Wi-Fi bring-up inside a reader-owned pump callback can freeze input for
  -- tens of seconds; only proceed when the link is already up.
  if type(net) == "table" and type(net.isConnected) == "function"
      and not net.isConnected() then
    return false  -- retry on a later pump once Wi-Fi is connected
  end
  if not Api or (type(Api.fetch_toc) ~= "function" and type(Api.fetch_toc_to_file) ~= "function") then
    job.needToc = false
    return false
  end
  job.needToc = false
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
  local file_mode = type(dl) == "table" and type(dl.jsonToFile) == "function"
  if n and tonumber(n) > 0 then
    chapter_catalog = Catalog.spec(tocRel, n, 0, 1)
    toc_source = tocRel
    chapters = Catalog.virtual_rows(chapter_catalog, "fanqie", job.bookId)
    pcall(Storage.save_catalog_meta, job.bookId, chapter_catalog)
    -- FileRows has no UID scan API; retain the host-provided chapter index.
  elseif not file_mode then
    local list
    list, err = Api.fetch_toc(job.bookId)
    if type(list) ~= "table" or #list < 1 then
      log(string.format("[FQ] history_bg_toc fail book=%s err=%s",
        tostring(job.bookId), tostring(err or "?")))
      return false
    end
    if #list > MAX_TOC then
      for i = #list, MAX_TOC + 1, -1 do list[i] = nil end
      if collectgarbage then collectgarbage("collect") end
    end
    chapter_catalog = nil
    chapters = list
    Storage.save_toc(job.bookId, chapters)
    if keepUid and keepUid ~= "" then
      for i = 1, #chapters do
        if tostring(chapters[i].chapterUid or "") == keepUid then
          chapter_idx = i
          break
        end
      end
    end
  else
    log(string.format("[FQ] history_bg_toc file_rows_unavailable book=%s err=%s",
      tostring(job.bookId), tostring(err or "no_jsonToFile")))
    return false
  end
  pcall(provider_register_current_book)
  log(string.format("[FQ] history_bg_toc book=%s n=%d", tostring(job.bookId), #chapters))
  return true
end

-- Background prefetch while native reader owns the display (cooperative).
-- One step per pump tick so a slow TLS/JSON request can never freeze the
-- reader UI (JJWXC prefetch_job pattern). File-mode fetch_chapter_to_file is
-- one host-side hop; legacy Lua path splits into download / cache-write.
prefetch_job = nil

local function set_err(pid, bookId, chapterUid, idx, title, err)
  if type(provider) ~= "table" or type(provider.setChapter) ~= "function" then return end
  provider.setChapter({
    providerId = pid or "fanqie",
    bookId = tostring(bookId or ""),
    chapterUid = tostring(chapterUid or ""),
    index = tonumber(idx) or -1,
    title = title,
    state = "error",
    error = tostring(err or "prefetch"),
  })
end

function provider_pump_work()
  -- History cold-open: refresh full TOC to SD. Skipped while a prefetch job
  -- is in flight (one heavy op per tick).
  if type(prefetch_job) ~= "table" then
    pcall(history_bg_restore_toc_step)
  end

  -- Advance an in-flight prefetch exactly one step per pump.
  if type(prefetch_job) == "table" then
    local j = prefetch_job
    if j.phase == "fetch_file" and Api.fetch_chapter_to_file and j.path then
      local n, ferr = Api.fetch_chapter_to_file(j.bookId, { chapterUid = j.chapterUid }, j.path)
      if n and tonumber(n) > 0 then
        local path = j.path
        prefetch_job = nil
        provider_set_chapter_ready(j.bookId, j.chapterUid, path, j.idx)
        log(string.format("[FQ] prefetch_ok_file book=%s ch=%s idx=%s bytes=%s",
          j.bookId, j.chapterUid, tostring(j.idx), tostring(n)))
      elseif ferr and ferr ~= "unsupported" then
        prefetch_job = nil
        set_err(j.pid, j.bookId, j.chapterUid, j.idx, j.title, ferr)
      else
        -- unsupported / empty: fall through to the legacy text pipeline once.
        prefetch_job.phase = "fetch_text"
      end
      return
    end
    if j.phase == "fetch_text" then
      local text, err = Api.fetch_chapter_text(j.bookId, { chapterUid = j.chapterUid })
      if not text then
        prefetch_job = nil
        set_err(j.pid, j.bookId, j.chapterUid, j.idx, j.title, err or "fetch")
      else
        j.pending_text = text
        prefetch_job.phase = "write"
      end
      return
    end
    if j.phase == "write" then
      local ok_write = Storage.save_chapter_text
        and Storage.save_chapter_text(j.bookId, j.chapterUid, j.pending_text)
      j.pending_text = nil
      prefetch_job = nil
      if collectgarbage then collectgarbage("collect") end
      if ok_write then
        local path = Storage.chapter_path and Storage.chapter_path(j.bookId, j.chapterUid) or nil
        if path then provider_set_chapter_ready(j.bookId, j.chapterUid, path, j.idx) end
        log(string.format("[FQ] prefetch_ok book=%s ch=%s idx=%s",
          j.bookId, j.chapterUid, tostring(j.idx)))
      else
        set_err(j.pid, j.bookId, j.chapterUid, j.idx, j.title, "cache")
      end
      return
    end
    -- Unknown/corrupt job: surface it instead of leaving Fetching forever.
    prefetch_job = nil
    set_err(j.pid, j.bookId, j.chapterUid, j.idx, j.title, "bad_job")
    return
  end

  if type(provider) ~= "table" or type(provider.pollWork) ~= "function" then return end
  if type(Storage) ~= "table" or type(Api) ~= "table" then return end
  local w = provider.pollWork()
  if type(w) ~= "table" or w.type ~= "prefetch" then return end
  local bookId = tostring(w.bookId or "")
  local idx = tonumber(w.index) or -1
  local pid = tostring(w.providerId or "fanqie")
  if bookId == "" then return end
  local function set_chapter_err(chapterUid, title, err)
    if type(provider.setChapter) ~= "function" then return end
    provider.setChapter({
      providerId = pid, bookId = bookId, chapterUid = tostring(chapterUid or ""),
      index = idx, title = title, state = "error", error = tostring(err or "prefetch"),
    })
  end
  -- FileRows work intentionally carries an empty UID. Resolve the bounded row
  -- before touching cache, network, or provider state.
  local chapterUid, title, resolveErr = Catalog.resolve_work(w)
  if not chapterUid or chapterUid == "" then
    log(string.format("[FQ] prefetch_resolve_fail book=%s idx=%s err=%s",
      bookId, tostring(idx), tostring(resolveErr or "empty_uid")))
    -- Error (not Fetching) so idle/next can retry — leaving Fetching stuck forever.
    set_chapter_err("", title, resolveErr or "empty_uid")
    return
  end
  if type(provider.setChapter) == "function" then
    provider.setChapter({
      providerId = pid, bookId = bookId, chapterUid = chapterUid, index = idx,
      title = title, state = "fetching", pct = 5,
    })
  end
  local directPath = Storage.chapter_path and Storage.chapter_path(bookId, chapterUid) or nil
  if directPath and Storage.chapter_complete and Storage.chapter_complete(bookId, chapterUid) then
    provider_set_chapter_ready(bookId, chapterUid, directPath, idx)
    return
  end
  if Storage.chapter_file_size then
    local fsz = tonumber(Storage.chapter_file_size(bookId, chapterUid) or 0) or 0
    if fsz > 0 and Storage.clear_chapter_cache then
      pcall(Storage.clear_chapter_cache, bookId, chapterUid)
    end
  end
  if not ensure_network or not ensure_network() then
    set_chapter_err(chapterUid, title, "net")
    return
  end
  -- Arm the cooperative pipeline; steps advance on later pumps so this
  -- callback returns immediately (no blocking request inside the callback).
  if Api.fetch_chapter_to_file and directPath then
    prefetch_job = { phase = "fetch_file", pid = pid, bookId = bookId,
      chapterUid = chapterUid, idx = idx, title = title, path = directPath }
  else
    prefetch_job = { phase = "fetch_text", pid = pid, bookId = bookId,
      chapterUid = chapterUid, idx = idx, title = title }
  end
end

function open_native_reader(path, title, bookId, chapterUid)
  if type(reader) ~= "table" or type(reader.openText) ~= "function" then
    return false, "no_reader"
  end
  status_line = font_ok and "打开阅读器…" or "opening reader…"
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
    progressKey = "fanqie:" .. tostring(bookId) .. ":" .. tostring(chapterUid),
    providerId = "fanqie",
    chapterIndex = math.max(0, (chapter_idx or 1) - 1),
  }
  local tocPath = "cache/" .. tostring(bookId) .. "/toc.json"
  if chapter_catalog then
    if type(fs) ~= "table" or type(fs.fileSize) ~= "function"
        or (fs.fileSize(tocPath) or 0) <= 0 then
      tocPath = nil
    end
  end
  if tocPath then opts.tocPath = tocPath end
  local restored = false
  local function apply_saved(saved)
    if type(saved) ~= "table" then return false end
    if tostring(saved.chapterUid or "") ~= tostring(chapterUid or "") then return false end
    if type(saved.byteOffset) == "number" and saved.byteOffset >= 0 then
      opts.initialByteOffset = math.floor(saved.byteOffset)
      return true
    end
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
    if type(native_progress) == "table"
       and (tostring(native_progress.chapterUid or "") ~= tostring(chapterUid or "")
            or tostring(native_progress.bookId or "") ~= tostring(bookId or "")) then
      native_progress = nil
    end
  end
  log(string.format("[FQ] openText_call t=%s path=%s off=%s",
    tostring(sys.millis()), tostring(path),
    opts.initialByteOffset and tostring(opts.initialByteOffset) or "-"))
  local ok, err = reader.openText(opts)
  if ok then
    screen = "native_reader"
    dirty = false
    frame_changed = false
    last_loading_frame = nil
    log(string.format("[FQ] openText_accepted t=%s", tostring(sys.millis())))
    return true
  end
  log(string.format("[FQ] openText_fail t=%s err=%s", tostring(sys.millis()), tostring(err)))
  return false, err
end

function init()
  log("fanqie plugin init v0.2.18 code=29")
  begin_startup("font", 0, "starting")
end

local _orig_draw = draw
function draw()
  if screen == "startup" and startup_job then
    if startup_job.ui_committed then
      advance_startup()
    else
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
  _orig_draw()
end
