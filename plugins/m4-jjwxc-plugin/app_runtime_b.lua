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
  -- Mark abort first so a concurrent ContentProvider.step hop can exit ASAP.
  if type(chapter_job) == "table" then
    chapter_job.cancelled = true
    chapter_job.phase = "cancelled"
    if type(chapter_job.fetch) == "table" then
      chapter_job.fetch.cancelled = true
    end
  end
  if type(loader) == "table" and type(loader.cancel) == "function" then
    pcall(loader.cancel)
  end
  clear_chapter_job()
  reader_text = nil
  reader_path = nil
  reader_file_size = 0
  reader_page_starts = {}
  reader_page_chunk = nil
  reader_pages = {}
  reader_page = 1
  reader_page_prefer = nil
  -- 取消路径不做 collectgarbage：避免点按后仍卡数百 ms
end

function chapter_load_fail(code, title, body, action)
  local idx = chapter_idx
  cancel_chapter_load()
  retry_chapter_idx = idx
  action = action or "retry_chapter"
  -- VIP 未购 / 明确回目录：不提供重试文案，主按钮=返回章节列表
  if action == "toc" or code == "E_VIP" then
    action = "toc"
    body = tostring(body or "")
    if font_ok and not body:find("返回", 1, true) then
      body = body .. "\n\n点按底部「返回章节列表」"
    end
    set_message(title, body,
      font_ok and "返回章节列表" or "back to chapter list",
      "toc", code or "E_VIP")
    return
  end
  set_message(title, body,
    font_ok and "返回目录 · 重试" or "toc · retry",
    action, code)
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

function step_chapter_load()
  local job = chapter_job
  if not job then return end
  if job.cancelled or job.phase == "cancelled" then
    clear_chapter_job()
    return
  end

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
    soft_release_mem = soft_release_mem,
    log = log,
    net_error_title = net_error_title,
    net_error_body = net_error_body,
  })
  if not chapter_job or (chapter_job and chapter_job.cancelled) then
    clear_chapter_job()
    return
  end

  if r == "done" then
    -- open_native_reader already set screen=native_reader on success.
    -- Do not force native_reader if still on loading (open failed → fail path).
    if screen == "loading" or screen == "toc" then
      -- Safety: file ready but open missed — try once more.
      local path = job.path
      if (not path or path == "") and Storage.chapter_path and cur_book then
        path = Storage.chapter_path(cur_book.bookId, job.chapterUid or chapter_uid)
      end
      if path and path ~= "" then
        local ok = open_native_reader(path, job.title or reader_title, job.bookId or (cur_book and cur_book.bookId),
          job.chapterUid or chapter_uid)
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
  if r == "login" then
    local from = chapter_idx
    cancel_chapter_load()
    retry_chapter_idx = from
    begin_login_flow("toc")
    return
  end
  if r == "fail" then
    if job.cancelled or job.phase == "cancelled"
        or job.fail_code == "cancelled"
        or tostring(job.fail_body or ""):find("cancel", 1, true) then
      clear_chapter_job()
      return
    end
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

category_page = 1
cur_category = nil
booklist = {}
booklist_api_page = 1
booklist_page = 1
booklist_loading = false
booklist_has_next = true
-- Keep few pages only: each page is a Lua table of books; too many pages +
-- residual API decode peaks used to OOM on heavy categories.
BOOKLIST_CACHE_MAX = 2
booklist_cache = {}
booklist_cache_order = {}
BOOKLIST_FETCH_SIZE = 10
toc_book = nil

-- Global helpers (NOT local): advance_network_job is defined earlier in this
-- chunk and would resolve local-forward refs as _G.* → nil → crash
-- "attempt to call a nil value (global 'booklist_cache_clear')".
function booklist_cache_put(page, list, has_next)
  page = tonumber(page) or 0
  if page < 1 or type(list) ~= "table" then return end
  if not booklist_cache[page] then
    booklist_cache_order[#booklist_cache_order + 1] = page
    if #booklist_cache_order > BOOKLIST_CACHE_MAX then
      local old = table.remove(booklist_cache_order, 1)
      booklist_cache[old] = nil
    end
  end
  -- Always refresh list/has_next (end-of-catalog must update has_next=false).
  booklist_cache[page] = { list = list, has_next = has_next and true or false }
end

function booklist_cache_get(page)
  return booklist_cache[page]
end

function booklist_cache_clear()
  booklist_cache = {}
  booklist_cache_order = {}
end

-- 书架行: [0]分类浏览 [1]登录/退出 [2]今日限免 [3+]本地书籍
function shelf_rows()
  local rows = {
    { title = "分类浏览", sub = "按分类查找书籍" },
  }
  if Auth.has() then
    rows[#rows + 1] = { title = "扫码登录", sub = "已登录 · 点按退出" }
  else
    rows[#rows + 1] = { title = "扫码登录", sub = "晋江App扫一扫 · 支持VIP章" }
  end
  rows[#rows + 1] = { title = "今日限免", sub = "每日免费书单" }
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
    title = "晋江文学",
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
  local i = tonumber(idx) or -1
  if i == 0 then
    open_category_screen()
    return
  end
  if i == 1 then
    if Auth.has() then
      Auth.clear()
      set_message(font_ok and "已退出登录" or "Logged out",
        font_ok and "VIP 章需重新扫码" or "QR login needed for VIP",
        font_ok and "点按返回书架" or "tap back", "shelf")
    else
      begin_login_flow("shelf")
    end
    return
  end
  if i == 2 then
    open_booklist({ group = "限免", title = "今日限免", channel = "novelfree" })
    return
  end
  local b = books[i - 2]
  if b then load_toc(b) end
end

function on_shelf_back()
  ui.listClose()
  sys.exit()
end

function ensure_mem(min_headroom)
  if type(sys.memInfo) ~= "function" then return true end
  local mi = sys.memInfo()
  if type(mi) == "table" and tonumber(mi.lua_headroom or 0) < tonumber(min_headroom or 0) then
    log("[JJ] ensure_mem block headroom=" .. tostring(mi.lua_headroom or "?")
      .. " need=" .. tostring(min_headroom or "?"))
    return false
  end
  return true
end

function soft_release_mem(reason)
  if type(Gbk) == "table" and type(Gbk.unload) == "function" then
    pcall(Gbk.unload)
  end
  -- Drop booklist page cache when we need headroom (open category / chapter
  -- fetch / background prefetch). Prefetch used to skip this and OOM.
  reason = tostring(reason or "")
  local drop_bl = reason == "booklist_open" or reason == "booklist_retry_mem"
      or reason == "bookpage" or reason == "bookpage_mem2"
      or reason == "chapter_fetch" or reason == "toc_download"
      or reason == "prefetch_begin" or reason == "prefetch"
      or reason:find("prefetch", 1, true)
      or reason == "low_mem" or reason == "oom_guard"
  if drop_bl then
    booklist_cache = {}
    booklist_cache_order = {}
    -- Do not keep a second live copy of the previous page while fetching.
    if reason == "booklist_open" or reason == "booklist_retry_mem"
        or reason == "bookpage" or reason == "low_mem" or reason == "oom_guard" then
      -- Keep `booklist` only for current on-screen list; first open clears it.
      if reason == "booklist_open" or reason == "booklist_retry_mem"
          or reason == "low_mem" or reason == "oom_guard" then
        booklist = {}
      end
    end
  end
  if collectgarbage then
    collectgarbage("collect")
    collectgarbage("collect")
  end
  if type(sys.memInfo) == "function" then
    local mi = sys.memInfo()
    if type(mi) == "table" then
      log(string.format("[JJ] mem reason=%s used=%s head=%s limit=%s",
        reason,
        tostring(mi.lua_used or "?"),
        tostring(mi.lua_headroom or "?"),
        tostring(mi.lua_limit or "?")))
    end
  end
end

function fmt_bytes(n)
  if ContentProvider and type(ContentProvider.fmt_bytes) == "function" then
    return ContentProvider.fmt_bytes(n)
  end
  n = tonumber(n) or 0
  if n < 1024 then return tostring(math.floor(n)) .. "B" end
  return string.format("%.1fKB", n / 1024)
end

function open_category_screen()
  local rows = {}
  for i = 1, #CATEGORIES do
    rows[#rows + 1] = { title = CATEGORIES[i].title, sub = "[" .. tostring(CATEGORIES[i].group) .. "]" }
  end
  if type(ui) ~= "table" or type(ui.listOpen) ~= "function" then
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

function open_booklist(cat)
  if type(cat) ~= "table" then return false end
  cur_category = cat
  booklist = {}
  soft_release_mem("booklist_open")
  if collectgarbage then collectgarbage("collect") end
  booklist_api_page = 1
  booklist_loading = false
  begin_network_job("booklist", cat,
    font_ok and "加载书单…" or "loading book list…",
    font_ok and "分阶段加载：内存 → 网络 → 下载 → 打开\n耗时与本数会更新，非卡死"
      or "phased load: mem → wifi → download → open")
  return true
end

function open_booklist_sync(cat)
  cur_category = cat
  booklist = {}
  booklist_cache_clear()
  booklist_loading = false
  soft_release_mem("booklist_open")
  status_line = font_ok and "联网获取书单…" or "fetching book list…"
  local online, nerr = ensure_network()
  if not online then
    set_message(net_error_title(nerr), net_error_body(nerr),
      font_ok and "点按重试 · 返回书架" or "tap retry / back shelf", "retry_category")
    return
  end
  if not ensure_mem(16384) then
    soft_release_mem("booklist_retry_mem")
    if not ensure_mem(10240) then
      booklist_loading = false
      log("[JJ] booklist_sync abort low_mem", "error")
      set_message(font_ok and "内存不足" or "low memory",
        font_ok and "请返回书架后再试分类书单" or "back to shelf and retry",
        font_ok and "点按返回书架" or "tap back shelf", "shelf")
      return
    end
  end
  local list, err
  if tostring(cat.channel or "") == "novelfree" then
    list, err = Api.fetch_novelfree()
    booklist_has_next = false
  else
    list, err = Api.fetch_category(cat.channel, 1, BOOKLIST_FETCH_SIZE)
    booklist_has_next = list and #list >= BOOKLIST_FETCH_SIZE or false
  end
  booklist_loading = false
  if not list then
    local title = font_ok and "书单失败" or "List failed"
    local body = tostring(err or "unknown")
    if err == "wifi_not_connected" then
      title, body = net_error_title(err), net_error_body(err)
    elseif err == "oom" or err == "out_of_memory" or err == "json_too_large"
        or (type(err) == "string" and err:find("oom", 1, true)) then
      title = font_ok and "内存不足" or "low memory"
      body = font_ok and "书单过大，已取消加载。请返回书架后再试"
        or "list too large; back to shelf and retry"
      soft_release_mem("oom_guard")
      log("[JJ] booklist_sync oom err=" .. tostring(err), "error")
    end
    set_message(title, body, font_ok and "点按重试 · 返回书架" or "tap retry / back shelf", "retry_category")
    return
  end
  booklist = list
  booklist_api_page = 1
  booklist_cache_put(1, list, booklist_has_next)
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
    local b = booklist[i]
    local sub = tostring(b.author or "")
    local st = tostring(b.status or "")
    if st == "2" then
      sub = (sub ~= "" and (sub .. " · ") or "") .. "完结"
    elseif st == "1" then
      sub = (sub ~= "" and (sub .. " · ") or "") .. "连载"
    end
    rows[#rows + 1] = { title = b.title or "", sub = sub }
  end
  return rows
end

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
  local page = tonumber(type(job) == "table" and job.page) or (booklist_api_page + 1)
  page = math.max(1, math.floor(page))
  -- Preserve current page so a failed fetch can resync the host list.
  local prev_list = booklist
  local prev_page = booklist_api_page
  local prev_has = booklist_has_next

  local function restore_and_reopen(has_next_override)
    booklist = prev_list or {}
    booklist_api_page = prev_page or 1
    if has_next_override ~= nil then
      booklist_has_next = has_next_override and true or false
    else
      booklist_has_next = prev_has and true or false
    end
    booklist_loading = false
    open_booklist_scene()
  end

  if type(cur_category) ~= "table" then
    booklist_loading = false
    return
  end
  if tostring(cur_category.channel or "") == "novelfree" then
    -- 限免单页: 无翻页 — reopen page 1 so host page counter resyncs
    booklist_has_next = false
    booklist_loading = false
    booklist_api_page = 1
    open_booklist_scene()
    return
  end

  soft_release_mem("bookpage")
  local online, nerr = ensure_network()
  if not online then
    restore_and_reopen()
    set_message(net_error_title(nerr), net_error_body(nerr),
      font_ok and "点按返回书单" or "tap back to list", "retry_category")
    return
  end
  if not ensure_mem(10240) then
    soft_release_mem("bookpage_mem2")
  end

  local ok_call, list, err = pcall(function()
    return Api.fetch_category(cur_category.channel, page, BOOKLIST_FETCH_SIZE)
  end)
  if not ok_call then
    err = tostring(list or "pcall")
    list = nil
    log(string.format("[JJ] bookpage pcall fail page=%s err=%s", tostring(page), err))
  end
  booklist_loading = false

  if not list then
    if tostring(err) == "empty" then
      -- True end of category: stay on previous page, drop next affordance.
      restore_and_reopen(false)
      status_line = font_ok and "已到最后一页" or "last page"
      return
    end
    restore_and_reopen()
    set_message(font_ok and "书单加载失败" or "book list failed",
      tostring(err or "unknown"),
      font_ok and "点按返回书单" or "tap back to list", "retry_category")
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
  if type(cur_category) ~= "table" then return end
  if booklist_loading then
    log("[JJ] on_book_page ignored: loading")
    return
  end
  page = math.max(1, math.floor(tonumber(page) or 1))
  total = math.max(1, math.floor(tonumber(total) or 1))

  -- Host already advanced its page counter before this callback. If we cannot
  -- fetch, we must reopen with booklist_api_page so footer/page stay in sync.
  if page == booklist_api_page then
    return
  end

  if page > booklist_api_page and not booklist_has_next then
    log(string.format("[JJ] on_book_page clamp host=%s api=%s no_next",
      tostring(page), tostring(booklist_api_page)))
    open_booklist_scene()  -- resync host to current api page
    return
  end
  if page < 1 then
    open_booklist_scene()
    return
  end

  local cached = booklist_cache_get(page)
  if cached and type(cached.list) == "table" then
    booklist = cached.list
    booklist_api_page = page
    booklist_has_next = cached.has_next and true or false
    if type(ui) == "table" and type(ui.listClose) == "function" then ui.listClose() end
    open_booklist_scene()
    return
  end

  -- Snapshot page we leave for back-navigation cache.
  if type(booklist) == "table" and #booklist > 0 then
    booklist_cache_put(booklist_api_page, booklist, booklist_has_next)
  end

  booklist_loading = true
  if type(ui) == "table" and type(ui.listClose) == "function" then ui.listClose() end
  local going_next = page > booklist_api_page
  begin_network_job("bookpage", { page = page },
    going_next
      and (font_ok and ("加载第" .. tostring(page) .. "页…") or ("page " .. tostring(page)))
      or (font_ok and ("返回第" .. tostring(page) .. "页…") or ("page " .. tostring(page))),
    font_ok and "正在获取分类书籍，请稍候…" or "fetching category page…")
end

function on_book_back()
  ui.listClose()
  screen = "shelf"
  dirty = true
  open_shelf_scene()
end

-- 打开一本书的目录。FileRows 把完整 UID/标题 留在 SD。
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

function load_toc_sync(book, pref)
  pref = pref or {}
  status_line = font_ok and "打开目录…" or "opening toc…"

  local tocRel = pref.prefetched_rel
    or (Storage.toc_rows_path and Storage.toc_rows_path(book.bookId))
    or ("cache/" .. tostring(book.bookId) .. "/toc_rows.txt")
  toc_source = tocRel
  local err, loaded = nil, false

  -- 目录内容只落 SD (toc_rows.txt), Lua 只持有 FileRows 虚拟行 (磁盘地址)。
  -- 本地缓存优先: catalog meta + toc_rows.txt
  local meta = Storage.load_catalog_meta and Storage.load_catalog_meta(book.bookId) or nil
  local metaSize = meta and type(fs) == "table" and type(fs.fileSize) == "function"
    and fs.fileSize(tostring(meta.source or "")) or nil
  if meta and metaSize and metaSize > 0 then
    chapter_catalog = Catalog.spec(meta.source, meta.count, meta.uid_field, meta.title_field)
    toc_source = chapter_catalog.source
    chapters = Catalog.virtual_rows(chapter_catalog, "jjwxc", book.bookId)
    loaded = true
  end

  -- 分阶段下载已完成: 直接用预取结果, 避免再打一遍网络
  if not loaded and pref.prefetched_n and tonumber(pref.prefetched_n) > 0 then
    local n = tonumber(pref.prefetched_n)
    chapter_catalog = Catalog.spec(tocRel, n, 0, 1)
    chapters = Catalog.virtual_rows(chapter_catalog, "jjwxc", book.bookId)
    pcall(Storage.save_catalog_meta, book.bookId, chapter_catalog)
    loaded = true
    err = nil
  end

  if not loaded then
    local n, ferr, info = nil, nil, nil
    -- network_job 分阶段已尝试过则不再重复打接口
    if pref.prefetched_n ~= nil or pref.prefetched_err ~= nil then
      n = pref.prefetched_n
      ferr = pref.prefetched_err
    else
      n, ferr, info = Api.fetch_toc_to_file(book.bookId, tocRel)
    end
    if n and tonumber(n) > 0 then
      local finalN = tonumber(n)
      if Storage.count_file_lines then
        local real = Storage.count_file_lines(tocRel)
        if real and real > finalN then finalN = real end
      end
      chapter_catalog = Catalog.spec(tocRel, finalN, 0, 1)
      chapters = Catalog.virtual_rows(chapter_catalog, "jjwxc", book.bookId)
      pcall(Storage.save_catalog_meta, book.bookId, chapter_catalog)
      if Storage.mark_toc_complete then
        pcall(Storage.mark_toc_complete, book.bookId, finalN)
      end
      loaded = true
      err = nil
      if info and info.bytes then
        status_line = tostring(finalN) .. (font_ok and " 章 · " or " ch · ") .. fmt_bytes(info.bytes)
      end
    else
      err = ferr
      -- 最后兜底 (旧宿主无 dl): jsonGet 投影小表 (大书 900 章上限, 内存仅 5 字段)
      if type(dl) ~= "table" or type(dl.jsonGet) ~= "function" then
        local list
        list, err = Api.fetch_toc(book.bookId)
        if list and #list > MAX_TOC then
          for i = #list, MAX_TOC + 1, -1 do list[i] = nil end
          if collectgarbage then collectgarbage("collect") end
        end
        chapters = list or {}
        loaded = type(list) == "table" and #list > 0
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
  -- 一律原生阅读器目录；不再 listOpenFile / 自制 toc
  if chapter_catalog and open_native_toc() then
    return true
  end
  if open_native_toc() then return true end
  -- 最后兜底仍走 load_toc 重试原生，不进入 screen=toc
  log("[JJ] load_toc_sync no_native_toc book=" .. tostring(book.bookId))
  screen = "loading"
  dirty = true
  return true
end

