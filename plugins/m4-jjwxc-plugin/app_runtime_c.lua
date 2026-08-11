-- Rebuild chapter_catalog from SD.
-- 完整目录 (.ok) 信任 meta.count，避免每次点章全量扫行卡死；
-- 仅 early/不完整时才 recount。
function ensure_catalog_from_disk(bookId)
  bookId = tostring(bookId or (cur_book and cur_book.bookId) or "")
  if bookId == "" then return false end
  local tocRel = Storage.toc_rows_path and Storage.toc_rows_path(bookId)
      or ("cache/" .. bookId .. "/toc_rows.txt")
  local meta = Storage.load_catalog_meta and Storage.load_catalog_meta(bookId) or nil
  if meta and tostring(meta.source or "") ~= "" then
    tocRel = tostring(meta.source)
  end
  if type(fs) ~= "table" or type(fs.fileSize) ~= "function" then return false end
  local fsz = tonumber(fs.fileSize(tocRel) or 0) or 0
  if fsz < 1 then return false end

  local metaCount = meta and tonumber(meta.count) or 0
  local complete = Storage.toc_complete and Storage.toc_complete(bookId)
  local n = metaCount
  -- 仅缺 meta / 未完成 / 单行 stub 时全量数行（大书扫行会卡 UI）
  if n < 2 or not complete then
    local real = Storage.count_file_lines and Storage.count_file_lines(tocRel) or 0
    if real > n then n = real end
    if metaCount > 0 and metaCount ~= real and real > 0 then
      log(string.format("[JJ] catalog_recount book=%s meta=%s file=%s",
        bookId, tostring(metaCount), tostring(real)))
    end
  end
  if n < 1 then return false end

  chapter_catalog = Catalog.spec(tocRel, n,
    meta and meta.uid_field or 0, meta and meta.title_field or 1)
  toc_source = tocRel
  chapters = Catalog.virtual_rows(chapter_catalog, "jjwxc", bookId)
  pcall(Storage.save_catalog_meta, bookId, chapter_catalog)
  if n >= 2 and Storage.mark_toc_complete and not complete then
    pcall(Storage.mark_toc_complete, bookId, n)
  end
  return true
end

-- 立刻回原生目录。取消/失败路径禁止 load_toc 全量网络与全量数行。
function return_to_toc()
  local t0 = (sys and sys.millis and sys.millis()) or 0
  cancel_chapter_load()
  if type(network_job) == "table" then
    pcall(cancel_network_job)
  end
  if not cur_book then
    screen = "shelf"
    dirty = true
    open_shelf_scene()
    return false
  end
  -- 已有可用 catalog 时绝不 ensure 扫盘；否则只读 meta（轻量）
  local bookId = tostring(cur_book.bookId or "")
  local count = chapter_catalog and tonumber(chapter_catalog.count) or 0
  if count < 2 then
    local meta = Storage.load_catalog_meta and Storage.load_catalog_meta(bookId) or nil
    local n = meta and tonumber(meta.count) or 0
    local src = meta and tostring(meta.source or "") or ""
    if n >= 2 and src ~= "" then
      chapter_catalog = Catalog.spec(src, n, meta.uid_field or 0, meta.title_field or 1)
      toc_source = src
      chapters = Catalog.virtual_rows(chapter_catalog, "jjwxc", bookId)
      count = n
    end
  end
  if open_native_toc({ skip_ensure = true, no_network = true }) then
    log(string.format("[JJ] return_to_toc_ok ms=%s count=%s",
      tostring(((sys and sys.millis and sys.millis()) or t0) - t0), tostring(count)))
    return true
  end
  -- 取消路径绝不 load_toc（会再等 TLS/下载）；回书架比卡死好
  log(string.format("[JJ] return_to_toc_fallback_shelf ms=%s",
    tostring(((sys and sys.millis and sys.millis()) or t0) - t0)))
  screen = "shelf"
  dirty = true
  open_shelf_scene()
  return false
end

-- opts.skip_ensure: 不扫盘 recount；opts.no_network: 目录不全时不 load_toc
function open_native_toc(opts)
  opts = opts or {}
  if not cur_book or type(reader) ~= "table"
      or type(reader.openToc) ~= "function" then
    return false
  end
  local bookId = tostring(cur_book.bookId or "")
  if not opts.skip_ensure then
    pcall(ensure_catalog_from_disk, bookId)
  end

  local count = chapter_catalog and tonumber(chapter_catalog.count) or 0
  local complete = Storage.toc_complete and Storage.toc_complete(bookId)
  -- Incomplete / single-row stub (history cold-open): refresh full TOC first.
  if count < 2 and not complete then
    if opts.no_network then
      log(string.format("[JJ] openToc_incomplete_no_net book=%s count=%s",
        bookId, tostring(count)))
      return false
    end
    log(string.format("[JJ] openToc_incomplete book=%s count=%s → load_toc",
      bookId, tostring(count)))
    load_toc(cur_book)
    return true
  end
  if not chapter_catalog and (not chapters or #chapters < 1) then
    return false
  end

  local openOpts = {
    bookId = bookId,
    title = tostring(cur_book.title or "目录"),
    currentIndex = math.max(0, (chapter_idx or 1) - 1),
  }
  if chapter_catalog and count >= 1 then
    pcall(provider_register_current_book)
    openOpts.providerId = "jjwxc"
  else
    local tocPath = "cache/" .. bookId .. "/toc.json"
    if type(fs) ~= "table" or type(fs.fileSize) ~= "function"
        or (fs.fileSize(tocPath) or 0) <= 0 then
      return false
    end
    openOpts.tocPath = tocPath
  end
  local ok, err = reader.openToc(openOpts)
  if ok then
    screen = "native_toc"
    dirty = false
    frame_changed = false
    log(string.format("[JJ] openToc_accepted t=%s ch=%d catalog=%s",
      tostring(sys.millis()), chapter_idx or 0, tostring(count)))
    return true
  end
  log(string.format("[JJ] openToc_fail t=%s err=%s", tostring(sys.millis()), tostring(err)))
  return false
end

function onTocClosed(r)
  log(string.format("[JJ] onTocClosed t=%s", tostring(sys.millis())))
  if screen == "native_toc" then
    screen = "loading"
    dirty = true
    frame_changed = true
    last_loading_frame = nil
  end
  if type(r) == "table" and not r.cancelled and type(r.chapterIndex) == "number" then
    local idx = math.floor(r.chapterIndex) + 1
    -- Native TOC only returns index; re-bind catalog so virtual_rows / isvip work.
    if cur_book and cur_book.bookId then
      pcall(ensure_catalog_from_disk, cur_book.bookId)
      pcall(provider_register_current_book)
    end
    local n = #chapters
    if idx >= 1 and idx <= n then
      if open_chapter(idx) then return end
      -- open_chapter 失败时再直接用行数据打开（带 isvip），避免直接退书架
      local row = chapters[idx]
      if type(row) == "table" then
        local uid = tostring(row.chapterUid or "")
        local title = tostring(row.title or uid)
        if uid ~= "" and open_chapter_uid(idx, uid, title, nil, row.isvip) then
          return
        end
      end
      set_message(font_ok and "打开失败" or "open failed",
        font_ok and ("无法打开第 " .. tostring(idx) .. " 章") or ("chapter " .. tostring(idx)),
        font_ok and "点按返回目录" or "tap back toc", "toc")
      return
    end
    set_message(font_ok and "打开失败" or "open failed",
      font_ok and "章节索引无效" or "bad chapter index",
      font_ok and "点按返回目录" or "tap back toc", "toc")
    return
  end
  -- 取消 / 返回: 回书架（不进自制目录）
  screen = "shelf"
  dirty = true
  open_shelf_scene()
end

function on_toc_row(idx, itemId, title)
  ui.listClose()
  local rowIdx = (tonumber(idx) or -1) + 1
  local uid = tostring(itemId or "")
  local rowTitle = tostring(title or "")
  -- 内存 rows 场景 (ui.listOpen): 回调只传 (idx, title, sub), itemId 实为标题;
  -- 必须从 chapters 取真实 chapterUid, 否则会用标题当 UID 请求接口。
  local ch = chapters[rowIdx]
  if type(ch) == "table" and tostring(ch.chapterUid or "") ~= "" then
    uid = tostring(ch.chapterUid)
    rowTitle = tostring(ch.title or uid)
  end
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

function open_chapter_uid(idx, uid, title, prefer, isvip)
  if not cur_book or not uid or uid == "" then return false end
  local bookId = tostring(cur_book.bookId or "")
  if bookId == "" then
    set_message(font_ok and "打开失败" or "open failed",
      font_ok and "书籍 ID 无效" or "missing bookId", "返回", "shelf")
    return false
  end
  uid = tostring(uid)
  -- Guard: list callbacks sometimes pass title as uid — refuse non-id for path safety.
  if uid == "" or (#uid > 32 and not uid:match("^%d+$")) then
    log(string.format("[JJ] bad chapterUid rejected %s", uid:sub(1, 40)))
    return false
  end
  if screen == "native_reader" then
    return false
  end
  cancel_chapter_load()
  if type(loader) == "table" and type(loader.cancel) == "function" then
    pcall(loader.cancel)
  end
  if type(native_progress) == "table" then
    local same = tostring(native_progress.bookId or "") == bookId
      and tostring(native_progress.chapterUid or "") == uid
    if not same then
      native_progress = nil
    end
  end
  chapter_idx = math.max(1, tonumber(idx) or 1)
  retry_chapter_idx = chapter_idx
  chapter_uid = uid
  reader_title = tostring(title or uid)
  reader_page_prefer = prefer
  local vip = isvip
  if vip == nil and type(Api) == "table" and type(Api.flag_is_vip) == "function" then
    vip = false
  end
  status_line = (vip and font_ok and "准备打开 VIP…")
      or (font_ok and "准备打开…" or "preparing…")
  screen = "loading"
  local path = Storage.chapter_path(bookId, uid)
  chapter_job = ContentProvider.begin({
    bookId = bookId,
    chapterUid = uid,
    title = reader_title,
    path = path,
    prefer = prefer,
    status = status_line,
    chapterIndex = math.max(0, chapter_idx - 1),
    isvip = vip and true or false,
  })
  log(string.format("[JJ] chapter_tap t=%s book=%s ch=%s vip=%s path=%s",
    tostring(sys.millis()), bookId, uid, tostring(vip and true or false), path))
  dirty = true
  return true
end

function open_chapter(idx, prefer)
  if not cur_book or idx < 1 then return false end
  if screen == "native_reader" then
    return false
  end
  local row = chapters[idx]
  if type(row) ~= "table" and chapter_catalog then
    -- virtual_rows 偶发未解析时再读一次
    row = Catalog and Catalog.read_row and Catalog.read_row(chapter_catalog, idx - 1) or nil
  end
  local uid = row and tostring(row.chapterUid or "") or ""
  local title = row and tostring(row.title or row.chapterUid or "") or ""
  if uid == "" then return false end
  local vip = row and row.isvip
  if vip == nil and type(Api) == "table" and type(Api.flag_is_vip) == "function" then
    vip = false
  end
  return open_chapter_uid(idx, uid, title, prefer, vip)
end

function handle_message_action()
  local a = message_action or "exit"
  if a == "retry_category" then
    if cur_category then
      open_booklist(cur_category)
    else
      open_category_screen()
    end
  elseif a == "retry_login" then
    begin_login_flow(login_return)
  elseif a == "login" then
    begin_login_flow("shelf")
  elseif a == "booklist" then
    screen = "booklist"
    dirty = true
  elseif a == "retry_chapter" then
    local idx = retry_chapter_idx or chapter_idx
    if cur_book and idx and idx >= 1 and idx <= #chapters then
      open_chapter(idx)
    elseif cur_book then
      return_to_toc()
    else
      screen = "shelf"
      dirty = true
    end
  elseif a == "toc" then
    return_to_toc()
  elseif a == "shelf" then
    screen = "shelf"
    dirty = true
  else
    screen = "shelf"
    dirty = true
  end
end

local LOAD_SPINNER = { "·", "··", "···", "····" }

function draw_loading()
  local sec = 0
  if network_job then
    sec = network_job_elapsed_s(network_job)
  elseif chapter_job and type(chapter_job.t0) == "number" and type(sys.millis) == "function" then
    local d = sys.millis() - chapter_job.t0
    if d > 0 then sec = math.floor(d / 1000) end
  end
  local spin = LOAD_SPINNER[(sec % #LOAD_SPINNER) + 1]

  local sl = status_line or ""
  if chapter_job and ContentProvider and ContentProvider.status_line then
    local s2 = ContentProvider.status_line(chapter_job, font_ok)
    if s2 and s2 ~= "" then sl = s2 end
  elseif network_job then
    sl = spin .. " " .. tostring(network_job.label or sl)
    if (network_job.count or 0) > 0 then
      sl = sl .. " · " .. tostring(network_job.count) .. (font_ok and "章" or "ch")
    end
    if (network_job.bytes or 0) > 0 then
      sl = sl .. " · " .. fmt_bytes(network_job.bytes)
    end
    if sec >= 1 then
      sl = sl .. " · " .. tostring(sec) .. "s"
    end
  else
    sl = spin .. " " .. tostring(sl)
    if sec >= 1 then sl = sl .. " · " .. tostring(sec) .. "s" end
  end
  local body
  if network_job then
    local parts = { spin .. " " .. tostring(network_job.body or "") }
    local nk = tostring(network_job.kind or "")
    if network_job.sys_loader == true then
      parts[#parts + 1] = font_ok and "模式: 流式目录 (可提前打开)" or "mode: progressive toc"
    elseif network_job.phase == "download" then
      if nk == "booklist" or nk == "bookpage" then
        parts[#parts + 1] = font_ok and "模式: 分类书单 JSON" or "mode: category list"
      else
        parts[#parts + 1] = font_ok and "模式: 整包下载" or "mode: full download"
      end
    elseif network_job.phase == "mem" or network_job.phase == "wifi"
        or network_job.phase == "open" then
      parts[#parts + 1] = (font_ok and "阶段 " or "phase ")
        .. tostring(network_job.phase)
    end
    if (network_job.count or 0) > 0 then
      if nk == "booklist" or nk == "bookpage" then
        parts[#parts + 1] = (font_ok and "本数 " or "books ") .. tostring(network_job.count)
      else
        parts[#parts + 1] = (font_ok and "章节数 " or "chapters ") .. tostring(network_job.count)
      end
    end
    if (network_job.bytes or 0) > 0 then
      parts[#parts + 1] = (font_ok and "数据 " or "data ") .. fmt_bytes(network_job.bytes)
    elseif network_job.phase == "download" then
      parts[#parts + 1] = font_ok and "等待首包 / TLS…" or "waiting first byte / TLS…"
    else
      parts[#parts + 1] = font_ok and "等待数据…" or "waiting data…"
    end
    parts[#parts + 1] = (font_ok and "已用时 " or "elapsed ") .. tostring(sec) .. "s"
    if (network_job.ms or 0) > 0 then
      parts[#parts + 1] = (font_ok and "上次耗时 " or "last hop ")
        .. tostring(math.floor(network_job.ms / 1000)) .. "s"
    end
    body = table.concat(parts, "\n")
  elseif ContentProvider and ContentProvider.loading_body then
    body = ContentProvider.loading_body(chapter_job, font_ok)
  else
    body = font_ok and "正在加载章节…" or "loading chapter…"
  end
  local title = reader_title or ""
  if network_job and network_job.kind == "toc" then
    local book = network_job.data or cur_book
    if book then title = tostring(book.title or title) end
  end
  -- 章节加载（含 VIP）：底部明确「取消 / 返回列表」，避免无按钮感
  local st = state_table()
  if chapter_job then
    local g = UiTemplate.header(st, title, sl)
    local m = g.m
    local y = m.content_y
    local step = (tonumber(m.line) or 24) + 8
    local bottom = (tonumber(m.content_bottom) or (tonumber(m.footer_top) or 760) - 8) - step
    for line in string.gmatch(tostring(body or "") .. "\n", "([^\n]*)\n") do
      if y > bottom then break end
      if line ~= "" then
        gui.drawText(g.body_font, g.left, y, line)
      end
      y = y + step
    end
    UiTemplate.footer(st,
      font_ok and "取消" or "cancel",
      font_ok and "返回章节列表" or "chapter list",
      nil, nil)
  else
    local hint = font_ok and "点按/返回 · 取消" or "tap/back · cancel"
    UiTemplate.page(st, title, sl, body, hint)
  end
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
    local loading_body = ""
    local el = 0
    if network_job then
      el = network_job_elapsed_s(network_job)
      loading_body = tostring(network_job.body or "") .. "|"
        .. tostring(network_job.bytes or 0) .. "|"
        .. tostring(network_job.count or 0) .. "|"
        .. tostring(el) .. "|"
        .. tostring(network_job.phase or "") .. "|"
        .. tostring(network_job.sys_loader or "")
    elseif ContentProvider and ContentProvider.loading_body then
      loading_body = tostring(ContentProvider.loading_body(chapter_job, font_ok) or "")
      if chapter_job and type(chapter_job.t0) == "number" and type(sys.millis) == "function" then
        local d = sys.millis() - chapter_job.t0
        if d > 0 then el = math.floor(d / 1000) end
      end
    end
    local loading_sl = status_line or ""
    if chapter_job and ContentProvider and ContentProvider.status_line then
      loading_sl = ContentProvider.status_line(chapter_job, font_ok) or loading_sl
    end
    -- Include elapsed seconds so e-ink refreshes at least once per second
    -- even when bytes stay 0 during TLS handshake.
    local loading_frame = tostring(loading_sl) .. "\n" .. tostring(loading_body)
      .. "|" .. tostring(chapter_job and chapter_job.phase or "")
      .. "|" .. tostring(chapter_job and chapter_job.bytes or 0)
      .. "|" .. tostring(chapter_job and chapter_job.loader_phase or "")
      .. "|" .. tostring(el)
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
  elseif screen == "login" then
    -- 登录屏持续轮询, 只在二维码/状态变化时触发 e-ink 刷新
    local lf = tostring(Auth.qr_data() or "-") .. "|" .. tostring(Auth.login_msg or "")
    frame_changed = lf ~= last_login_frame
    last_login_frame = lf
    draw_login()
  elseif screen == "message" then
    local title = message_title or ""
    if message_code and message_code ~= "" then
      title = message_code .. " · " .. title
    end
    local st = state_table()
    local act = message_action or "exit"
    if act == "toc" or message_code == "E_VIP" then
      -- 单主按钮：返回章节列表（点按任意处 / 底栏均有效）
      UiTemplate.page(st, title, nil, message_body or "",
        message_hint or (font_ok and "返回章节列表" or "back to list"))
    elseif act == "retry_chapter" then
      local g = UiTemplate.header(st, title, nil)
      local m = g.m
      local y = m.content_y
      local step = (tonumber(m.line) or 24) + 8
      local bottom = (tonumber(m.content_bottom) or (tonumber(m.footer_top) or 760) - 8) - step
      for line in string.gmatch(tostring(message_body or "") .. "\n", "([^\n]*)\n") do
        if y > bottom then break end
        if line ~= "" then
          gui.drawText(g.body_font, g.left, y, line)
        end
        y = y + step
      end
      UiTemplate.footer(st,
        font_ok and "返回章节列表" or "list",
        font_ok and "重试" or "retry",
        nil, nil)
    else
      UiTemplate.page(st, title, nil, message_body or "", message_hint or "返回")
    end
  else
    gui.drawText(12, 24, 80, "...")
  end

  local keep_pumping = startup_job ~= nil or network_job ~= nil or login_job ~= nil
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
    if screen == "login" then
      cancel_login_flow()
      return
    end
    if screen == "loading" then
      if network_job then cancel_network_job() end
      if chapter_job then
        return_to_toc()
      else
        dirty = true
      end
      return
    end
    if screen == "reader" then
      save_progress_local()
      return_to_toc()
    elseif screen == "toc" or screen == "booklist" or screen == "category" then
      screen = "shelf"
      dirty = true
    elseif screen == "message" then
      if message_action == "retry_chapter" or message_action == "toc" then
        return_to_toc()
        return
      end
      if message_action == "booklist" then
        screen = "booklist"
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
    elseif screen == "login" then
      -- 无动作 (轮询继续)
    elseif screen == "loading" then
      if network_job then cancel_network_job() end
      if chapter_job then
        return_to_toc()
      else
        dirty = true
      end
    elseif screen == "shelf" then
      if shelf_page < UiShelf.page_count(books, SHELF_PAGE) then
        shelf_page = shelf_page + 1
        dirty = true
      end
    elseif screen == "category" or screen == "booklist" or screen == "toc" then
      -- 新宿主用 ui.list, 无操作
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
    local act = message_action or "exit"
    if act == "retry_chapter" then
      -- 左半：返回目录；右半：重试
      if x < w / 2 then
        return_to_toc()
      else
        handle_message_action()
      end
    else
      -- VIP / toc / 其它：整屏点按 = 执行 action（VIP 为返回章节列表）
      handle_message_action()
    end
    return
  end

  if screen == "native_reader" or screen == "native_toc" then
    return
  end

  if screen == "login" then
    if y < UI_METRICS.content_y then
      cancel_login_flow()
      return
    end
    if Layout.home_footer_hit(y, UI_METRICS) then
      cancel_login_flow()
    end
    return
  end

  if screen == "loading" then
    if network_job then cancel_network_job() end
    if chapter_job then
      -- 加载中（含 VIP 探测/下载）点按 = 取消并回原生章节列表
      return_to_toc()
    else
      dirty = true
    end
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
    -- 行1=分类浏览 行2=登录 行3=限免 行4+=书籍
    local rows = UiShelf.slice(books, shelf_page, SHELF_PAGE)
    local row = Layout.home_row_from_point(y, m, #rows + 3, m.shelf_row_h)
    if not row then return end
    if row == 1 then
      open_category_screen()
      return
    elseif row == 2 then
      if Auth.has() then
        Auth.clear()
        set_message(font_ok and "已退出登录" or "Logged out",
          font_ok and "VIP 章需重新扫码" or "QR login needed for VIP",
          font_ok and "点按返回书架" or "tap back", "shelf")
      else
        begin_login_flow("shelf")
      end
      return
    elseif row == 3 then
      open_booklist({ group = "限免", title = "今日限免", channel = "novelfree" })
      return
    end
    local idx = row - 3
    if idx >= 1 and idx <= #rows then load_toc(rows[idx]) end
    return
  end

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
      return_to_toc()
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

