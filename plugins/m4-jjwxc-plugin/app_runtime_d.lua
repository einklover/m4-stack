function onReaderClosed(prog)
  log(string.format("[JJ] onReaderClosed t=%s", tostring(sys.millis())))
  clear_chapter_job()
  if screen == "native_reader" then
    screen = "loading"
    dirty = true
    frame_changed = true
    last_loading_frame = nil
  end
  if type(prog) == "table" and cur_book then
    if prog.openFailed or (type(prog.error) == "string" and prog.error ~= "") then
      local openErr = tostring(prog.error or "error")
      -- Chapter-end cross-chapter open: the next chapter's body wasn't ready
      -- (prefetch TLS failed / file empty). Hand it to the same flow as
      -- picking the chapter from the list: open_chapter shows the loading page
      -- ("准备打开… / 下载章节…"), downloads if needed, then opens.
      if openErr == "load_failed" or openErr == "encoding_unsupported" then
        local ch = chapters[chapter_idx]
        local expectUid = chapter_uid ~= "" and chapter_uid or (ch and tostring(ch.chapterUid or "") or "")
        local expectBook = tostring(cur_book.bookId or "")
        if expectUid ~= "" and type(open_chapter) == "function" then
          log(string.format("[JJ] reader_open_not_ready err=%s → redownload book=%s ch=%s",
            openErr, expectBook, expectUid))
          pcall(open_chapter, chapter_idx, reader_page_prefer)
          return  -- loading page now; do not return_to_toc
        end
      end
      status_line = font_ok and ("打开失败: " .. openErr)
        or ("open failed: " .. openErr)
      return_to_toc()
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
    end
    if type(prog.switchChapterIndex) == "number" then
      local nextIdx = math.floor(prog.switchChapterIndex) + 1
      if nextIdx >= 1 and nextIdx <= #chapters and open_chapter(nextIdx) then
        return
      end
    end
  end
  cancel_chapter_load()
  -- After history open, chapters may be a 1-row stub — rebuild catalog first.
  if cur_book then
    return_to_toc()
    status_line = font_ok and "已返回目录" or "back to toc"
    return
  end
  screen = "shelf"
  dirty = true
  status_line = font_ok and "已返回目录" or "back to toc"
end

function provider_register_current_book()
  if type(provider) ~= "table" or type(provider.register) ~= "function" then return false end
  if not cur_book then return false end
  -- Prefer FileRows catalog; never register a 1-chapter inline stub that
  -- would make native TOC show only "第一节" after history reopen.
  pcall(ensure_catalog_from_disk, cur_book.bookId)
  if chapter_catalog and tonumber(chapter_catalog.count) and tonumber(chapter_catalog.count) >= 1 then
    local spec = Catalog.provider_spec(chapter_catalog)
    if not spec then return false end
    return provider.register({
      providerId = "jjwxc",
      bookId = tostring(cur_book.bookId or ""),
      title = tostring(cur_book.title or ""),
      catalog = spec,
      currentIndex = math.max(0, (chapter_idx or 1) - 1),
    })
  end
  -- Inline only when we truly have multiple in-memory rows (legacy toc.json).
  local chs = {}
  local maxInline = math.min(#chapters, 50)
  for i = 1, maxInline do
    local ch = chapters[i]
    if type(ch) == "table" then
      chs[#chs + 1] = {
        uid = tostring(ch.chapterUid or ""),
        title = tostring(ch.title or ""),
      }
    end
  end
  if #chs < 2 then return false end  -- refuse single-stub register
  return provider.register({
    providerId = "jjwxc",
    bookId = tostring(cur_book.bookId or ""),
    title = tostring(cur_book.title or ""),
    chapters = chs,
    currentIndex = math.max(0, (chapter_idx or 1) - 1),
  })
end

function provider_set_chapter_ready(bookId, chapterUid, path, index0)
  if type(provider) ~= "table" or type(provider.setChapter) ~= "function" then return end
  provider.setChapter({
    providerId = "jjwxc",
    bookId = tostring(bookId or ""),
    chapterUid = tostring(chapterUid or ""),
    index = index0,
    state = "ready",
    path = path,
    pct = 100,
  })
end

-- 历史续读的目录后台恢复 (androidapi 目录无需登录; 勿用 Auth.has 挡住)
function history_bg_restore_toc_step()
  local job = history_bg_restore
  if type(job) ~= "table" or not job.needToc or not job.bookId then return false end
  if not Api or (type(Api.fetch_toc) ~= "function" and type(Api.fetch_toc_to_file) ~= "function") then
    job.needToc = false
    return false
  end
  -- Prefer local disk rebuild before network.
  if ensure_catalog_from_disk(job.bookId) then
    job.needToc = false
    pcall(provider_register_current_book)
    log(string.format("[JJ] history_bg_toc disk_ok book=%s n=%s",
      tostring(job.bookId), tostring(chapter_catalog and chapter_catalog.count or #chapters)))
    return true
  end
  if ensure_network and not ensure_network() then
    return false  -- keep needToc for next pump
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
    local finalN = tonumber(n)
    if Storage.count_file_lines then
      local real = Storage.count_file_lines(tocRel)
      if real and real > finalN then finalN = real end
    end
    chapter_catalog = Catalog.spec(tocRel, finalN, 0, 1)
    toc_source = tocRel
    chapters = Catalog.virtual_rows(chapter_catalog, "jjwxc", job.bookId)
    pcall(Storage.save_catalog_meta, job.bookId, chapter_catalog)
    if Storage.mark_toc_complete then
      pcall(Storage.mark_toc_complete, job.bookId, finalN)
    end
    do
      local p = Storage.load_progress and Storage.load_progress(job.bookId) or nil
      if type(p) == "table" and tonumber(p.chapterIdx) then
        local i = math.floor(tonumber(p.chapterIdx))
        if i >= 1 and i <= #chapters then chapter_idx = i end
      end
      if keepUid and keepUid ~= "" and type(p) == "table"
          and tostring(p.chapterUid or "") == keepUid
          and tonumber(p.chapterIdx) then
        local i = math.floor(tonumber(p.chapterIdx))
        if i >= 1 and i <= #chapters then chapter_idx = i end
      end
    end
    log(string.format("[JJ] history_bg_toc net_ok book=%s n=%s",
      tostring(job.bookId), tostring(finalN)))
  elseif not file_mode then
    local list
    list, err = Api.fetch_toc(job.bookId)
    if type(list) ~= "table" or #list < 1 then
      log(string.format("[JJ] history_bg_toc fail book=%s err=%s",
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
    log(string.format("[JJ] history_bg_toc file_rows_unavailable book=%s err=%s",
      tostring(job.bookId), tostring(err or "no_jsonToFile")))
    return false
  end
  pcall(provider_register_current_book)
  return true
end

-- 阅读中后台预取: 每 pump 一步 (下载/转码分离), 不阻塞触摸触摸 30s。
prefetch_job = nil

local function prefetch_set_error(pid, bookId, chapterUid, idx, title, err)
  if type(provider) ~= "table" or type(provider.setChapter) ~= "function" then return end
  provider.setChapter({
    providerId = pid or "jjwxc",
    bookId = tostring(bookId or ""),
    chapterUid = tostring(chapterUid or ""),
    index = tonumber(idx) or -1,
    title = title,
    state = "error",
    error = tostring(err or "prefetch"),
  })
end

function provider_pump_work()
  -- Do not run heavy TOC network restore while a chapter prefetch is in flight
  -- (both can allocate / hold network buffers and tip Lua 512KB over).
  if type(prefetch_job) ~= "table" then
    pcall(history_bg_restore_toc_step)
  end

  -- 推进进行中的预取 hop
  if type(prefetch_job) == "table" then
    local j = prefetch_job
    if j.phase == "fetch" and Api and type(Api.chapter_fetch_step) == "function" then
      local path = j.path
      -- VIP convert / GBK is the heavy hop: bail early rather than OOM the host.
      local step = j.fetch and tostring(j.fetch.step or "") or ""
      if step == "vip_cvt" or step == "vip_dl" then
        soft_release_mem("prefetch")
        if not ensure_mem(96000) then
          prefetch_set_error(j.pid, j.bookId, j.chapterUid, j.idx, j.title, "low_mem")
          log(string.format("[JJ] prefetch_low_mem book=%s ch=%s step=%s",
            j.bookId, j.chapterUid, step))
          prefetch_job = nil
          return
        end
      end
      local st, payload, info = Api.chapter_fetch_step(j.fetch, path)
      if st == "busy" then
        if type(provider) == "table" and type(provider.setChapter) == "function" then
          local pct = 20
          if j.fetch and j.fetch.step == "vip_cvt" then pct = 70 end
          if j.fetch and j.fetch.downloaded and j.fetch.downloaded > 0 then pct = 55 end
          provider.setChapter({
            providerId = j.pid, bookId = j.bookId, chapterUid = j.chapterUid, index = j.idx,
            title = j.title, state = "fetching", pct = pct,
          })
        end
        return
      end
      if st == "done" then
        local n = tonumber(payload) or 0
        if n > 0 then
          provider_set_chapter_ready(j.bookId, j.chapterUid, path, j.idx)
          log(string.format("[JJ] prefetch_ok book=%s ch=%s idx=%s bytes=%s",
            j.bookId, j.chapterUid, tostring(j.idx), tostring(n)))
        else
          -- Must not leave Fetching forever (idle will not re-queue).
          prefetch_set_error(j.pid, j.bookId, j.chapterUid, j.idx, j.title, "empty_body")
          log(string.format("[JJ] prefetch_empty book=%s ch=%s idx=%s",
            j.bookId, j.chapterUid, tostring(j.idx)))
        end
        prefetch_job = nil
        if collectgarbage then collectgarbage("collect") end
        return
      end
      -- error
      local es = tostring(payload or "fetch")
      local transient = es:find("http_request", 1, true) or es:find("oom", 1, true)
        or es:find("memory", 1, true) or es:find("low_heap", 1, true)
        or es:find("tls", 1, true) or es:find("network", 1, true)
        or es:find("timeout", 1, true)
      if transient then
        -- Low internal RAM / TLS handshake under reader+prefetch concurrency:
        -- keep the job and retry on a later pump tick (once the display task
        -- is idle and internal RAM recovers) instead of failing permanently.
        local tries = tonumber(j.retry) or 0
        j.retry = tries + 1
        if tries < 8 then
          log(string.format("[JJ] prefetch_retry book=%s ch=%s idx=%s err=%s try=%s",
            j.bookId, j.chapterUid, tostring(j.idx), es, tostring(tries + 1)))
          return  -- keep prefetch_job; next pump tick retries the hop
        end
      end
      prefetch_set_error(j.pid, j.bookId, j.chapterUid, j.idx, j.title, es)
      log(string.format("[JJ] prefetch_err book=%s ch=%s idx=%s err=%s",
        j.bookId, j.chapterUid, tostring(j.idx), es))
      prefetch_job = nil
      if collectgarbage then collectgarbage("collect") end
      return
    end
    -- legacy single-shot
    if j.phase == "fetch_legacy" and Api and type(Api.fetch_chapter_to_file) == "function" then
      if not ensure_mem(80000) then
        prefetch_set_error(j.pid, j.bookId, j.chapterUid, j.idx, j.title, "low_mem")
        prefetch_job = nil
        return
      end
      local n, err = Api.fetch_chapter_to_file(j.bookId, {
        chapterUid = j.chapterUid, isvip = j.isvip,
      }, j.path)
      if n and tonumber(n) > 0 then
        provider_set_chapter_ready(j.bookId, j.chapterUid, j.path, j.idx)
        log(string.format("[JJ] prefetch_ok book=%s ch=%s idx=%s bytes=%s",
          j.bookId, j.chapterUid, tostring(j.idx), tostring(n)))
      else
        prefetch_set_error(j.pid, j.bookId, j.chapterUid, j.idx, j.title, err or "fetch")
      end
      prefetch_job = nil
      if collectgarbage then collectgarbage("collect") end
      return
    end
    -- Unknown/corrupt job: clear Fetching so user/idle can retry.
    prefetch_set_error(j.pid, j.bookId, j.chapterUid, j.idx, j.title, "bad_job")
    prefetch_job = nil
  end

  if type(provider) ~= "table" or type(provider.pollWork) ~= "function" then return end
  if type(Storage) ~= "table" or type(Api) ~= "table" then return end
  local w = provider.pollWork()
  if type(w) ~= "table" or w.type ~= "prefetch" then return end
  local bookId = tostring(w.bookId or "")
  local idx = tonumber(w.index) or -1
  local pid = tostring(w.providerId or "jjwxc")
  if bookId == "" then return end
  -- Ensure FileRows catalog is bound before resolve (history cold-open race).
  -- Only when catalog missing — avoid re-register churn every poll.
  if cur_book and tostring(cur_book.bookId or "") == bookId then
    local cnt = chapter_catalog and tonumber(chapter_catalog.count) or 0
    if cnt < 2 then
      pcall(ensure_catalog_from_disk, bookId)
      pcall(provider_register_current_book)
    end
  end
  local chapterUid, title, resolveErr, isvip = Catalog.resolve_work(w)
  if not chapterUid or chapterUid == "" then
    log(string.format("[JJ] prefetch_resolve_fail book=%s idx=%s err=%s",
      bookId, tostring(idx), tostring(resolveErr or "empty_uid")))
    -- Leave Error (not Fetching) so decideNextChapter/idle can retry.
    prefetch_set_error(pid, bookId, "", idx, title, resolveErr or "empty_uid")
    return
  end
  isvip = isvip and true or false
  if type(provider.setChapter) == "function" then
    provider.setChapter({
      providerId = pid, bookId = bookId, chapterUid = chapterUid, index = idx,
      title = title, state = "fetching", pct = 5,
    })
  end
  -- Only complete cache (.ok) is a hit; partial early-open bodies must redownload.
  local path = Storage.chapter_path and Storage.chapter_path(bookId, chapterUid) or nil
  if path and Storage.chapter_complete and Storage.chapter_complete(bookId, chapterUid) then
    provider_set_chapter_ready(bookId, chapterUid, path, idx)
    log(string.format("[JJ] prefetch_cache_hit book=%s ch=%s idx=%s",
      bookId, chapterUid, tostring(idx)))
    return
  end
  if path and Storage.chapter_file_size then
    local fsz = tonumber(Storage.chapter_file_size(bookId, chapterUid) or 0) or 0
    if fsz > 0 and Storage.clear_chapter_cache then
      pcall(Storage.clear_chapter_cache, bookId, chapterUid)
    end
  end
  if not ensure_network or not ensure_network() then
    prefetch_set_error(pid, bookId, chapterUid, idx, title, "net")
    return
  end
  if not path or path == "" then
    prefetch_set_error(pid, bookId, chapterUid, idx, title, "no_path")
    return
  end
  soft_release_mem("prefetch_begin")
  -- Free chapter file_out is cheap; VIP hops need headroom. Also require
  -- system heap (TLS) — mid-read free_heap ~50KB yields http_request_failed.
  local need = isvip and 96000 or 48000
  if not ensure_mem(need) then
    -- Not enough headroom right now: keep the job, retry next pump tick (the
    -- reader may be rendering; once it idles, internal RAM frees up).
    log(string.format("[JJ] prefetch_skip_low_mem book=%s ch=%s vip=%s (retry)",
      bookId, chapterUid, tostring(isvip)))
    return
  end
  if type(sys) == "table" and type(sys.memInfo) == "function" then
    local mi = sys.memInfo()
    local heap = type(mi) == "table" and tonumber(mi.heap_free or 0) or 0
    if heap > 0 and heap < 90000 then
      -- System heap too low for a TLS handshake right now (reader rendering +
      -- prefetch overlap): keep the job and retry on a later pump tick.
      log(string.format("[JJ] prefetch_skip_low_heap book=%s heap=%s (retry)",
        bookId, tostring(heap)))
      return
    end
  end
  if type(Api.chapter_fetch_begin) == "function" and type(Api.chapter_fetch_step) == "function" then
    prefetch_job = {
      phase = "fetch",
      bookId = bookId,
      chapterUid = chapterUid,
      title = title,
      idx = idx,
      pid = pid,
      path = path,
      isvip = isvip,
      fetch = Api.chapter_fetch_begin(bookId, { chapterUid = chapterUid, isvip = isvip }),
    }
    log(string.format("[JJ] prefetch_begin book=%s ch=%s idx=%s vip=%s",
      bookId, chapterUid, tostring(idx), tostring(isvip)))
  else
    prefetch_job = {
      phase = "fetch_legacy",
      bookId = bookId,
      chapterUid = chapterUid,
      title = title,
      idx = idx,
      pid = pid,
      path = path,
      isvip = isvip,
    }
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
    progressKey = "jjwxc:" .. tostring(bookId) .. ":" .. tostring(chapterUid),
    providerId = "jjwxc",
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
  log(string.format("[JJ] openText_call t=%s path=%s off=%s",
    tostring(sys.millis()), tostring(path),
    opts.initialByteOffset and tostring(opts.initialByteOffset) or "-"))
  local ok, err = reader.openText(opts)
  if ok then
    screen = "native_reader"
    dirty = false
    frame_changed = false
    last_loading_frame = nil
    log(string.format("[JJ] openText_accepted t=%s", tostring(sys.millis())))
    return true
  end
  log(string.format("[JJ] openText_fail t=%s err=%s", tostring(sys.millis()), tostring(err)))
  return false, err
end

-- 历史续读 (m4cp://) — 与 fanqie 相同的最小实现
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

function history_resume_cache_ready()
  local r = pending_history_resume
  if type(r) ~= "table" or not r.bookId or r.bookId == "" then return false end
  if not Storage.chapter_file_size then return false end
  local bookId = tostring(r.bookId)
  if r.chapterUid and tostring(r.chapterUid) ~= "" then
    local fsz = Storage.chapter_file_size(bookId, tostring(r.chapterUid))
    if fsz and fsz > 0 then return true end
  end
  -- Do NOT run safe_seg on full relative paths (would strip '/').
  if r.cacheRelPath and tostring(r.cacheRelPath) ~= "" and type(fs.fileSize) == "function" then
    local n = fs.fileSize(tostring(r.cacheRelPath))
    if n and n > 0 then return true end
  end
  -- Catalog meta alone is enough to open TOC from history (body may re-fetch).
  local meta = Storage.load_catalog_meta and Storage.load_catalog_meta(bookId) or nil
  if meta and type(fs) == "table" and type(fs.fileSize) == "function" then
    local n = tonumber(fs.fileSize(tostring(meta.source or "")) or 0) or 0
    if n > 0 and tonumber(meta.count or 0) > 0 then return true end
  end
  local toc = Storage.load_toc and Storage.load_toc(bookId) or nil
  if type(toc) == "table" and #toc > 0 then
    local idx = resolve_history_chapter_idx(bookId, toc, r)
    local ch = toc[idx]
    if ch and ch.chapterUid then
      local fsz = Storage.chapter_file_size(bookId, ch.chapterUid)
      if fsz and fsz > 0 then return true end
    end
    return true  -- have catalog/toc for list even without body
  end
  return false
end

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
  log(string.format("[JJ] apply_history_resume book=%s ch=%s cache=%s",
    book.bookId, tostring(r.chapterUid or ""), tostring(r.cacheRelPath or "")))

  local toc = Storage.load_toc and Storage.load_toc(book.bookId) or nil
  local hadFullToc = type(toc) == "table" and #toc > 0

  -- Prefer FileRows catalog (toc_rows + meta) — this is the production path.
  local meta = Storage.load_catalog_meta and Storage.load_catalog_meta(book.bookId) or nil
  local metaSize = meta and type(fs) == "table" and type(fs.fileSize) == "function"
    and tonumber(fs.fileSize(tostring(meta.source or "")) or 0) or 0
  if not hadFullToc and meta and metaSize and metaSize > 0 then
    chapter_catalog = Catalog.spec(meta.source, meta.count, meta.uid_field, meta.title_field)
    toc_source = chapter_catalog.source
    chapters = Catalog.virtual_rows(chapter_catalog, "jjwxc", book.bookId)
    cur_book = book
    local p = Storage.load_progress and Storage.load_progress(book.bookId) or nil
    if type(r.chapterIndex) == "number" and r.chapterIndex >= 0 then
      chapter_idx = math.floor(r.chapterIndex) + 1
    elseif type(p) == "table" and tonumber(p.chapterIdx) then
      chapter_idx = math.floor(tonumber(p.chapterIdx))
    else
      chapter_idx = 1
    end
    if chapter_idx < 1 or chapter_idx > #chapters then chapter_idx = 1 end
    -- Prefer uid match when available (index alone can drift).
    local wantUid = tostring(r.chapterUid or (p and p.chapterUid) or "")
    if wantUid ~= "" then
      -- Virtual rows: cannot scan all; trust index if set, else leave chapter_idx.
      if type(p) == "table" and tostring(p.chapterUid or "") == wantUid
          and tonumber(p.chapterIdx) then
        local i = math.floor(tonumber(p.chapterIdx))
        if i >= 1 and i <= #chapters then chapter_idx = i end
      end
    end
    pcall(provider_register_current_book)
    local uid = wantUid
    if uid == "" and chapters[chapter_idx] then
      uid = tostring(chapters[chapter_idx].chapterUid or "")
    end
    if uid ~= "" and Storage.chapter_file_size then
      local fsz = Storage.chapter_file_size(book.bookId, uid)
      if fsz and fsz > 0 then
        seed_history_native_progress(book.bookId, uid, r)
        history_bg_restore = { bookId = book.bookId, needToc = false }
        local title = tostring(r.title or (chapters[chapter_idx] and chapters[chapter_idx].title) or uid)
        open_chapter_uid(chapter_idx, uid, title)
        return true
      end
    end
    -- Catalog ready but body missing: open full TOC list (not empty stub).
    if open_native_toc() then return true end
    load_toc(book)
    return true
  end

  if hadFullToc then
    chapter_catalog = nil
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
    load_toc(book)
    return true
  end

  -- Body cache only: open chapter, then bg-restore full FileRows catalog.
  if r.chapterUid and tostring(r.chapterUid) ~= "" and Storage.chapter_file_size then
    local uid = tostring(r.chapterUid)
    local title = tostring(r.title or uid)
    local fsz = Storage.chapter_file_size(book.bookId, uid)
    if fsz and fsz > 0 then
      -- Try disk catalog first so TOC is never empty after reopen.
      cur_book = book
      if ensure_catalog_from_disk(book.bookId) then
        chapter_idx = 1
        if type(r.chapterIndex) == "number" and r.chapterIndex >= 0 then
          local i = math.floor(r.chapterIndex) + 1
          if i >= 1 and i <= #chapters then chapter_idx = i end
        end
        pcall(provider_register_current_book)
        seed_history_native_progress(book.bookId, uid, r)
        history_bg_restore = { bookId = book.bookId, needToc = false }
        open_chapter_uid(chapter_idx, uid, title)
        return true
      end
      chapters = {{ chapterUid = uid, title = title }}
      chapter_idx = 1
      pcall(provider_register_current_book)
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
        cur_book = book
        if ensure_catalog_from_disk(book.bookId) then
          chapter_idx = math.max(1, (tonumber(r.chapterIndex) or 0) + 1)
          if chapter_idx > #chapters then chapter_idx = 1 end
          pcall(provider_register_current_book)
          seed_history_native_progress(book.bookId, uid, r)
          history_bg_restore = { bookId = book.bookId, needToc = false }
          open_chapter_uid(chapter_idx, uid, tostring(r.title or uid))
          return true
        end
        chapters = {{ chapterUid = uid, title = tostring(r.title or uid) }}
        chapter_idx = 1
        pcall(provider_register_current_book)
        seed_history_native_progress(book.bookId, uid, r)
        history_bg_restore = { bookId = book.bookId, needToc = true }
        open_chapter(1)
        return true
      end
    end
  end

  -- No local body: still try catalog for TOC-first, else network load_toc.
  cur_book = book
  if ensure_catalog_from_disk(book.bookId) then
    chapter_idx = math.max(1, (tonumber(r.chapterIndex) or 0) + 1)
    if chapter_idx > #chapters then chapter_idx = 1 end
    pcall(provider_register_current_book)
    if open_native_toc() then return true end
  end
  load_toc(book)
  return true
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

function maybe_history_bg_restore()
  local job = history_bg_restore
  if type(job) ~= "table" or not job.bookId then return end
  -- TOC restore may run while reading (provider_pump also calls the step).
  if not cur_book or tostring(cur_book.bookId) ~= tostring(job.bookId) then
    return
  end
  if job.needToc then
    pcall(history_bg_restore_toc_step)
    if not job.needToc then
      history_bg_restore = nil
    end
  else
    history_bg_restore = nil
  end
end

function init()
  log("jjwxc plugin init v0.1.9 code=19")
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
  if login_job then
    if login_job.ui_committed then
      step_login_flow()
    else
      login_job.ui_committed = true
      dirty = true
    end
  end
  -- History cold-open: restore full FileRows TOC in background.
  if history_bg_restore and history_bg_restore.needToc then
    pcall(maybe_history_bg_restore)
  end
  _orig_draw()
end
