UI_METRICS = Layout.metrics()
-- 书架 = 3 个固定行 (分类浏览/扫码登录/今日限免) + 书籍。
-- 页大小必须给固定行留出空间, 否则行会画到 footer 区域与元素重叠。
SHELF_PAGE = math.max(1, UI_METRICS.shelf_page_size - 3)
TOC_PAGE = UI_METRICS.toc_page_size
CATEGORY_PAGE = UI_METRICS.toc_page_size
BOOKLIST_PAGE = UI_METRICS.shelf_page_size
READER_MARGIN = 24
READER_LINE = UI_METRICS.reader_line_h

MAX_TOC = 900

-- 晋江 bookstore 频道 (分类浏览, 来自公开书源整理; 每项 连载/完结 双行)。
CATEGORIES = {
  { group = "古言", title = "天作之合·连载", channel = 14000019 },
  { group = "古言", title = "天作之合·完结", channel = 14000038 },
  { group = "古言", title = "宫廷侯爵·连载", channel = 14000020 },
  { group = "古言", title = "宫廷侯爵·完结", channel = 14000039 },
  { group = "古言", title = "复仇虐渣·连载", channel = 80000012 },
  { group = "古言", title = "复仇虐渣·完结", channel = 80000000 },
  { group = "古言", title = "重来一世·连载", channel = 14000021 },
  { group = "古言", title = "重来一世·完结", channel = 14000040 },
  { group = "古言", title = "古香古色·连载", channel = 14000017 },
  { group = "古言", title = "古香古色·完结", channel = 14000036 },
  { group = "古言", title = "女尊女强·连载", channel = 14000023 },
  { group = "古言", title = "女尊女强·完结", channel = 14000042 },
  { group = "现言", title = "业界精英·连载", channel = 15000018 },
  { group = "现言", title = "业界精英·完结", channel = 15000045 },
  { group = "现言", title = "豪门世家·连载", channel = 15000017 },
  { group = "现言", title = "豪门世家·完结", channel = 15000044 },
  { group = "现言", title = "都市情缘·连载", channel = 15000021 },
  { group = "现言", title = "都市情缘·完结", channel = 15000048 },
  { group = "现言", title = "破镜重圆·连载", channel = 15000022 },
  { group = "现言", title = "破镜重圆·完结", channel = 15000049 },
  { group = "现言", title = "校园青春·连载", channel = 80000016 },
  { group = "现言", title = "校园青春·完结", channel = 80000017 },
  { group = "幻言", title = "穿书攻略·连载", channel = 80000026 },
  { group = "幻言", title = "穿书攻略·完结", channel = 80000027 },
  { group = "幻言", title = "快穿系统·连载", channel = 16000021 },
  { group = "幻言", title = "快穿系统·完结", channel = 16000048 },
  { group = "幻言", title = "种田日常·连载", channel = 16000022 },
  { group = "幻言", title = "种田日常·完结", channel = 16000049 },
  { group = "幻言", title = "打脸爽文·连载", channel = 80000024 },
  { group = "幻言", title = "打脸爽文·完结", channel = 80000025 },
  { group = "古穿", title = "历史穿越·连载", channel = 17000023 },
  { group = "古穿", title = "历史穿越·完结", channel = 17000042 },
  { group = "古穿", title = "快穿穿书·连载", channel = 80000010 },
  { group = "古穿", title = "快穿穿书·完结", channel = 80000006 },
  { group = "奇幻", title = "仙侠情缘·连载", channel = 18000018 },
  { group = "奇幻", title = "仙侠情缘·完结", channel = 18000035 },
  { group = "奇幻", title = "修真升级·连载", channel = 18000021 },
  { group = "奇幻", title = "修真升级·完结", channel = 18000038 },
  { group = "奇幻", title = "灵异神怪·连载", channel = 18000022 },
  { group = "奇幻", title = "灵异神怪·完结", channel = 18000039 },
  { group = "科幻", title = "星际日常·连载", channel = 19000014 },
  { group = "科幻", title = "星际日常·完结", channel = 19000024 },
  { group = "科幻", title = "无限领域·连载", channel = 80000058 },
  { group = "科幻", title = "无限领域·完结", channel = 80000059 },
  { group = "科幻", title = "悬疑解密·连载", channel = 19000034 },
  { group = "科幻", title = "悬疑解密·完结", channel = 19000042 },
  { group = "纯爱", title = "宫廷将相·连载", channel = 21000025 },
  { group = "纯爱", title = "宫廷将相·完结", channel = 21000052 },
  { group = "纯爱", title = "仙侠修真·连载", channel = 21000024 },
  { group = "纯爱", title = "仙侠修真·完结", channel = 21000051 },
  { group = "纯爱", title = "古代幻想·连载", channel = 21000028 },
  { group = "纯爱", title = "古代幻想·完结", channel = 21000055 },
  { group = "百合", title = "都市情缘·连载", channel = 22000024 },
  { group = "百合", title = "都市情缘·完结", channel = 22000040 },
  { group = "百合", title = "穿越时空·连载", channel = 22000016 },
  { group = "百合", title = "穿越时空·完结", channel = 22000034 },
  { group = "无CP", title = "都市成长·连载", channel = 80000272 },
  { group = "无CP", title = "都市成长·完结", channel = 80000273 },
  { group = "无CP", title = "古代架空·连载", channel = 80000276 },
  { group = "无CP", title = "古代架空·完结", channel = 80000277 },
  { group = "衍纯", title = "都市轻小说·连载", channel = 80000064 },
  { group = "衍纯", title = "都市轻小说·完结", channel = 80000065 },
  { group = "衍言", title = "名著阅读·连载", channel = 24000017 },
  { group = "衍言", title = "名著阅读·完结", channel = 24000036 },
  { group = "衍言", title = "西方罗曼·连载", channel = 24000023 },
  { group = "衍言", title = "西方罗曼·完结", channel = 24000042 },
  { group = "二次元", title = "热血冒险·连载", channel = 80000180 },
  { group = "二次元", title = "热血冒险·完结", channel = 80000181 },
  { group = "二次元", title = "萌系育成·连载", channel = 80000190 },
  { group = "二次元", title = "萌系育成·完结", channel = 80000191 },
}

-- screens: startup | shelf | category | booklist | toc | reader | loading
--         | native_reader | native_toc | message | login
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
  UiTemplate.page({ UI_METRICS = UI_METRICS }, "晋江文学", nil,
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
    t0 = (sys and sys.millis and sys.millis()) or 0,
    phase = "paint",  -- paint → work (TOC: cache → download → open)
    bytes = 0,
    count = 0,
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
  if type(loader) == "table" and type(loader.cancel) == "function" then
    pcall(loader.cancel)
  end
  if type(job) ~= "table" then return false end
  screen = (job.kind == "toc" and "shelf") or job.return_screen or "shelf"
  dirty = true
  return true
end

function network_job_elapsed_s(job)
  local t0 = tonumber(job and job.t0) or 0
  if t0 <= 0 or type(sys.millis) ~= "function" then return 0 end
  local d = sys.millis() - t0
  if d < 0 then d = 0 end
  return math.floor(d / 1000)
end

function advance_network_job()
  local job = network_job
  if type(job) ~= "table" then return end

  -- TOC 分阶段: 缓存检查 / 下载 / 打开, 每阶段可刷新 UI
  if job.kind == "toc" then
    if job.phase == "paint" or job.phase == nil then
      job.phase = "cache"
      job.label = font_ok and "检查目录缓存…" or "checking toc cache…"
      job.body = font_ok and "优先使用本地目录文件" or "prefer local catalog"
      status_line = job.label
      dirty = true
      return
    end
    if job.phase == "cache" then
      local book = job.data
      if type(book) ~= "table" then
        network_job = nil
        set_message(font_ok and "目录失败" or "TOC failed", "no_book", "back", "shelf")
        return
      end
      local tocRel = Storage.toc_rows_path and Storage.toc_rows_path(book.bookId)
        or ("cache/" .. tostring(book.bookId) .. "/toc_rows.txt")
      local meta = Storage.load_catalog_meta and Storage.load_catalog_meta(book.bookId) or nil
      local metaSize = meta and type(fs) == "table" and type(fs.fileSize) == "function"
        and fs.fileSize(tostring(meta.source or "")) or nil
      if meta and metaSize and metaSize > 0 then
        job.bytes = metaSize
        job.count = tonumber(meta.count) or 0
        job.phase = "open_cached"
        job.label = font_ok
            and (string.format("缓存命中 %d 章 · %s", job.count, fmt_bytes(metaSize)))
            or ("cache hit " .. tostring(job.count))
        job.body = font_ok and "正在打开目录…" or "opening table of contents…"
        status_line = job.label
        dirty = true
        return
      end
      soft_release_mem("toc_download")
      job.phase = "download"
      job.label = font_ok and "下载目录…" or "downloading toc…"
      job.body = font_ok and "流式写入章节列表到 SD\n完成后立即打开" or "streaming catalog to SD"
      status_line = job.label
      dirty = true
      return
    end
    if job.phase == "open_cached" then
      network_job = nil
      load_toc_sync(job.data)
      return
    end
    if job.phase == "download" then
      local book = job.data
      local tocRel = Storage.toc_rows_path and Storage.toc_rows_path(book.bookId)
        or ("cache/" .. tostring(book.bookId) .. "/toc_rows.txt")
      job._toc_rel = tocRel

      -- Prefer system progressive TOC loader: early open after ~24 rows, then grow.
      if job.sys_loader ~= false and type(loader) == "table"
          and type(loader.toc) == "function" and Api and type(Api.toc_loader_spec) == "function" then
        if job.sys_loader == nil then
          local spec = Api.toc_loader_spec(book.bookId, tocRel, {
            title = book.title or "",
            currentIndex = math.max(0, (chapter_idx or 1) - 1),
          })
          if not spec then
            job.sys_loader = false
          else
            job.label = font_ok and "连接服务器 (TLS)…" or "TLS connect…"
            job.body = font_ok
                and "目录流式下载\n约 24 章即可先打开列表"
                or "stream catalog · early open ~24 rows"
            status_line = job.label
            local ok, err = loader.toc(spec)
            if not ok then
              log(string.format("[JJ] loader.toc fail %s", tostring(err)))
              job.sys_loader = false
              job.label = font_ok and "流式目录不可用 · 回退…" or "toc loader fallback…"
              status_line = job.label
              dirty = true
              return
            end
            job.sys_loader = true
            dirty = true
            return  -- paint "connecting" before first pump
          end
        end
        if job.sys_loader == true then
          if type(loader.pump) == "function" then
            local st0 = (type(loader.status) == "function" and loader.status()) or {}
            if st0.phase == "connecting" then
              loader.pump({ ms = 50, bytes = 1024 })
            else
              loader.pump({ ms = 150, bytes = 32 * 1024 })
            end
          end
          local st = (type(loader.status) == "function" and loader.status()) or {}
          job.bytes = tonumber(st.bytes) or job.bytes or 0
          job.count = tonumber(st.rows) or job.count or 0
          local phase = tostring(st.phase or "")
          local sec = network_job_elapsed_s(job)
          if phase == "connecting" then
            job.label = font_ok and "连接服务器 (TLS)…" or "TLS connect…"
            job.body = font_ok and ("握手中…\n已用时 " .. tostring(sec) .. "s")
                or ("handshake… " .. tostring(sec) .. "s")
          elseif phase == "streaming" or phase == "early" then
            job.label = font_ok
                and (string.format("目录 %d 章 · %s", job.count, fmt_bytes(job.bytes)))
                or (tostring(job.count) .. " ch · " .. fmt_bytes(job.bytes))
            job.body = font_ok
                and (string.format("流式写入 SD\n已用时 %ds%s",
                  sec, (st.early and "\n列表已可打开 · 后台续传") or ""))
                or ("streaming " .. tostring(sec) .. "s")
          end
          status_line = job.label
          if st.error and st.error ~= "" and not st.early and not st.done then
            log(string.format("[JJ] loader.toc err %s → blocking fallback", tostring(st.error)))
            job.sys_loader = false
            job.label = font_ok and ("目录回退 · " .. tostring(st.error)) or tostring(st.error)
            status_line = job.label
            dirty = true
            return
          end
          if st.early or st.done then
            -- Loader already queueToc / registered live row count.
            job.count = tonumber(st.rows) or job.count
            job.bytes = tonumber(st.bytes) or job.bytes
            if job.count and job.count > 0 then
              -- Prefer real line count when done (early count is provisional).
              local finalN = job.count
              if st.done and Storage.count_file_lines then
                local real = Storage.count_file_lines(tocRel)
                if real and real > finalN then finalN = real end
              end
              chapter_catalog = Catalog.spec(tocRel, finalN, 0, 1)
              chapters = Catalog.virtual_rows(chapter_catalog, "jjwxc", book.bookId)
              pcall(Storage.save_catalog_meta, book.bookId, chapter_catalog)
              if st.done and Storage.mark_toc_complete then
                pcall(Storage.mark_toc_complete, book.bookId, finalN)
              end
              shelf_add_book(book)
              cur_book = book
              pcall(provider_register_current_book)
              local totalCh = #chapters
              local prog = Storage.load_progress(book.bookId)
              if prog and prog.chapterIdx and prog.chapterIdx >= 1 and prog.chapterIdx <= totalCh then
                chapter_idx = prog.chapterIdx
              else
                chapter_idx = math.max(1, chapter_idx or 1)
              end
              toc_page = math.floor((chapter_idx - 1) / TOC_PAGE) + 1
              status_line = tostring(totalCh) .. (font_ok and " 章 · " or " ch · ")
                  .. tostring(book.title or book.bookId or "")
              network_job = nil
              if st.early and not st.done then
                -- Native TOC already queued by loader; stay on native_toc while residual pumps.
                -- Do NOT mark complete — keep pumping via host pumpLoader.
                screen = "native_toc"
                dirty = false
                frame_changed = false
              elseif not open_native_toc() then
                -- 不进自制 toc：保持 loading，下一 tick 再试 / 用户取消回书架
                screen = "loading"
                dirty = true
                status_line = font_ok and "打开目录…" or "open toc…"
              end
              return
            end
          end
          dirty = true
          return
        end
      end

      -- Blocking fallback (old firmware / loader failure)
      job.label = font_ok and "下载目录 (整包)…" or "downloading toc…"
      job.body = font_ok and "整包写入章节列表\n完成后打开" or "full catalog download"
      status_line = job.label
      local n, ferr, info = Api.fetch_toc_to_file(book.bookId, tocRel)
      if n and tonumber(n) > 0 then
        job.count = tonumber(n)
        job.bytes = (info and info.bytes) or 0
        if job.bytes < 1 and type(fs) == "table" and type(fs.fileSize) == "function" then
          job.bytes = tonumber(fs.fileSize(tocRel) or 0) or 0
        end
        job.phase = "open_fresh"
        job.label = font_ok
            and (string.format("已写入 %d 章 · %s", job.count, fmt_bytes(job.bytes)))
            or (tostring(job.count) .. " ch · " .. fmt_bytes(job.bytes))
        job.body = font_ok and "正在打开目录…" or "opening table of contents…"
        status_line = job.label
        dirty = true
        job._toc_n = n
        job._toc_err = nil
        return
      end
      job._toc_n = nil
      job._toc_err = ferr
      job.phase = "open_fresh"
      job.label = font_ok and "目录回退…" or "toc fallback…"
      status_line = job.label
      dirty = true
      return
    end
    if job.phase == "open_fresh" then
      network_job = nil
      load_toc_sync(job.data, {
        prefetched_n = job._toc_n,
        prefetched_rel = job._toc_rel,
        prefetched_err = job._toc_err,
        prefetched_bytes = job.bytes,
      })
      return
    end
  end

  -- booklist/bookpage: multi-phase like TOC so e-ink shows what is happening
  -- (mem / wifi / download / open) with elapsed + byte hints; not one silent freeze.
  if job.kind == "booklist" or job.kind == "bookpage" then
    local function bl_paint(label, body)
      job.label = label or job.label
      job.body = body or job.body
      status_line = job.label
      dirty = true
      frame_changed = true
      last_loading_frame = nil
    end
    -- Phases: paint → wifi → download(+open). Keep ≤3 advances after the
    -- first draw commit so host tests (pumpDraws(4)) still open the booklist.
    -- (Old paint→mem→wifi→download needed 5 draws and broke the sim gate.)
    if job.phase == "paint" or job.phase == nil or job.phase == "mem" then
      soft_release_mem(job.kind == "bookpage" and "bookpage" or "booklist_open")
      job.phase = "wifi"
      bl_paint(
        font_ok and "检查网络…" or "check wifi…",
        font_ok and "步骤 1/3 · Wi‑Fi / 释放内存" or "step 1/3 wifi")
      return
    end
    if job.phase == "wifi" then
      local online, nerr = ensure_network()
      if not online then
        network_job = nil
        booklist_loading = false
        set_message(net_error_title(nerr), net_error_body(nerr),
          font_ok and "点按重试 · 返回书架" or "tap retry / back shelf", "retry_category")
        return
      end
      job.phase = "download"
      bl_paint(
        font_ok and "下载书单…" or "download list…",
        font_ok and "步骤 2/3 · 从晋江拉取分类 JSON"
          or "step 2/3 download category JSON")
      -- Force one e-ink frame before the blocking network hop.
      if type(gui) == "table" and type(gui.refresh) == "function" then
        pcall(function()
          gui.clear()
          draw_loading()
          gui.refresh()
        end)
      end
      return
    end
    if job.phase == "download" then
      local cat = job.data
      if job.kind == "bookpage" then
        -- page fetch reuses fetch_book_page_sync internals via job.page
        local page = tonumber(type(cat) == "table" and cat.page) or (booklist_api_page + 1)
        job._page = page
        if not cur_category then
          network_job = nil
          booklist_loading = false
          return
        end
        if tostring(cur_category.channel or "") == "novelfree" then
          booklist_has_next = false
          booklist_loading = false
          network_job = nil
          booklist_api_page = 1
          open_booklist_scene()
          return
        end
        soft_release_mem("bookpage")
        if not ensure_mem(12288) then
          soft_release_mem("bookpage_mem2")
        end
        local t0 = sys.millis()
        local list, err, info = Api.fetch_category(cur_category.channel, page, BOOKLIST_FETCH_SIZE,
          function(st, inf)
            inf = inf or {}
            if st == "download" then
              bl_paint(font_ok and "下载书单…" or "download…",
                font_ok and ("步骤 3/4 · 第" .. tostring(page) .. "页 JSON")
                  or ("page " .. tostring(page)))
            elseif st == "done" then
              job.bytes = tonumber(inf.bytes) or job.bytes
              job.count = tonumber(inf.count) or job.count
            end
          end)
        job.ms = sys.millis() - t0
        booklist_loading = false
        if not list then
          network_job = nil
          -- restore previous page on failure
          open_booklist_scene()
          if tostring(err) == "empty" then
            booklist_has_next = false
            status_line = font_ok and "已到最后一页" or "last page"
          elseif err == "oom" or err == "json_too_large"
              or (type(err) == "string" and err:find("oom", 1, true)) then
            soft_release_mem("oom_guard")
            log("[JJ] bookpage oom err=" .. tostring(err), "error")
            set_message(font_ok and "内存不足" or "low memory",
              font_ok and "翻页失败，请返回书架后再试" or "page failed; back shelf",
              font_ok and "点按返回书单" or "tap back to list", "retry_category")
          else
            set_message(font_ok and "书单加载失败" or "book list failed",
              tostring(err or "unknown"),
              font_ok and "点按返回书单" or "tap back to list", "retry_category")
          end
          return
        end
        booklist_cache_put(page, list, #list >= BOOKLIST_FETCH_SIZE)
        booklist = list
        booklist_api_page = page
        booklist_has_next = #list >= BOOKLIST_FETCH_SIZE
        job.count = #list
        job.list = list
        job.phase = "open"
        -- Fall through same tick — waiting for another draw left loading stuck
        -- when frame_changed skipped the next pump.
      else
      -- booklist first page
      if type(cat) ~= "table" then
        network_job = nil
        set_message(font_ok and "书单失败" or "List failed", "no_category", "back", "shelf")
        return
      end
      cur_category = cat
      booklist = {}
      booklist_cache_clear()
      soft_release_mem("booklist_open")
      if not ensure_mem(16384) then
        soft_release_mem("booklist_retry_mem")
        if not ensure_mem(10240) then
          network_job = nil
          booklist_loading = false
          log("[JJ] booklist abort low_mem", "error")
          set_message(font_ok and "内存不足" or "low memory",
            font_ok and "请返回书架后再试分类书单" or "back to shelf and retry",
            font_ok and "点按返回书架" or "tap back shelf", "shelf")
          return
        end
      end
      local t0 = sys.millis()
      local list, err, info
      if tostring(cat.channel or "") == "novelfree" then
        list, err = Api.fetch_novelfree()
        booklist_has_next = false
      else
        list, err, info = Api.fetch_category(cat.channel, 1, BOOKLIST_FETCH_SIZE,
          function(st, inf)
            inf = inf or {}
            if st == "download" or st == "retry" then
              bl_paint(
                font_ok and "下载书单…" or "download…",
                font_ok and ("步骤 3/4 · 拉取分类 JSON"
                  .. (inf.attempt and inf.attempt > 1 and (" · 重试" .. tostring(inf.attempt)) or ""))
                  or "step 3/4 download")
            elseif st == "parse" then
              job.bytes = tonumber(inf.bytes) or job.bytes
              bl_paint(font_ok and "解析书单…" or "parse…",
                font_ok and ("步骤 3/4 · 解析 " .. fmt_bytes(job.bytes or 0))
                  or ("parse " .. fmt_bytes(job.bytes or 0)))
            elseif st == "done" then
              job.count = tonumber(inf.count) or job.count
              job.bytes = tonumber(inf.bytes) or job.bytes
            end
          end)
        booklist_has_next = list and #list >= BOOKLIST_FETCH_SIZE or false
      end
      job.ms = sys.millis() - t0
      booklist_loading = false
      if not list then
        network_job = nil
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
          log("[JJ] booklist oom err=" .. tostring(err), "error")
        end
        set_message(title, body,
          font_ok and "点按重试 · 返回书架" or "tap retry / back shelf", "retry_category")
        return
      end
      if type(info) == "table" then
        job.bytes = tonumber(info.bytes) or job.bytes
        job.count = tonumber(info.count) or #list
      else
        job.count = #list
      end
      job.list = list
      job.phase = "open"
      end  -- first-page vs bookpage
    end
    if job.phase == "open" then
      local list = job.list
      job.list = nil  -- drop job-held duplicate before building UI rows
      if type(list) ~= "table" or #list < 1 then
        network_job = nil
        set_message(font_ok and "书单失败" or "List failed", "empty", "back", "shelf")
        return
      end
      booklist = list
      if job.kind ~= "bookpage" then
        booklist_api_page = 1
        booklist_cache_put(1, list, booklist_has_next)
      end
      status_line = font_ok
          and string.format("已获 %d 本 · %ds", #list, math.floor((job.ms or 0) / 1000))
          or string.format("%d books · %ds", #list, math.floor((job.ms or 0) / 1000))
      local opened = false
      if type(ui) == "table" and type(ui.listOpen) == "function" and cur_category then
        opened = open_booklist_scene() and true or false
      end
      network_job = nil
      if opened then
        -- Host owns list paint while list_active; keep screen tag for close.
        screen = "booklist"
        dirty = false
        frame_changed = false
        log(string.format("[JJ] booklist_open_ok n=%d page=%s",
          #list, tostring(booklist_api_page)))
      else
        booklist_page = 1
        screen = "booklist"
        dirty = true
        frame_changed = true
        log(string.format("[JJ] booklist_open_fallback_lua n=%d", #list))
      end
      return
    end
    return
  end

  network_job = nil
  if job.kind == "toc" then
    load_toc_sync(job.data)
  end
end

-- 扫码登录 (UI-first: 先画一帧, 再取二维码)。
login_job = nil  -- { ui_committed, retry }  nil = 不在登录流程
login_return = "shelf"  -- 登录成功后回到哪里
last_login_frame = nil

function begin_login_flow(return_screen)
  login_return = return_screen or (screen == "toc" and "toc") or "shelf"
  login_job = { ui_committed = false }
  screen = "login"
  status_line = font_ok and "扫码登录" or "QR login"
  Auth.login_msg = "准备登录..."
  dirty = true
  frame_changed = true
  last_loading_frame = nil
end

function step_login_flow()
  if not login_job then return end
  if not login_job.ui_committed then return end
  if not Auth.key then
    -- 取二维码 (一帧一动作)
    local ok = Auth.begin_login()
    if not ok then
      login_job = nil
      set_message(font_ok and "登录失败" or "Login failed", Auth.login_msg,
        font_ok and "点按重试 · 返回书架" or "tap retry / back", "retry_login")
      return
    end
    dirty = true
    return
  end
  -- 轮询 (非阻塞)
  local st = Auth.poll_login_step()
  if st == "ok" then
    local ret = login_return
    login_job = nil
    if type(Gbk) == "table" and type(Gbk.unload) == "function" then Gbk.unload() end
    set_message(font_ok and "登录成功" or "Logged in", font_ok and "已获得晋江会话" or "session ready",
      font_ok and "点按继续" or "tap to continue",
      ret == "toc" and "toc" or "shelf")
    return
  elseif st == "timeout" then
    login_job = nil
    Auth.key = nil
    set_message(font_ok and "二维码失效" or "QR expired", Auth.login_msg,
      font_ok and "点按重试 · 返回书架" or "tap retry / back", "retry_login")
    return
  elseif st == "fail" then
    login_job = nil
    Auth.key = nil
    set_message(font_ok and "登录失败" or "Login failed", Auth.login_msg,
      font_ok and "点按重试 · 返回书架" or "tap retry / back", "retry_login")
    return
  elseif st == "cancelled" then
    login_job = nil
    Auth.key = nil
    screen = "shelf"
    dirty = true
    return
  end
  dirty = true  -- pending: 保持轮询
end

function cancel_login_flow()
  login_job = nil
  if type(Auth) == "table" and type(Auth.cancel_login) == "function" then
    Auth.cancel_login()
  end
  screen = "shelf"
  dirty = true
end

function draw_login()
  local st = state_table()
  UiLogin.draw(st)
end

function finish_startup_shelf(list, prefix)
  books = merge_local_progress_into_books(list or {})
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
    job.phase, job.pct, job.label = "session", 20, "loading session"
  elseif job.phase == "session" then
    Auth.load()
    -- Home 历史续读 (m4cp://)
    if type(provider) == "table" and type(provider.takeResume) == "function" then
      local resume = provider.takeResume()
      if type(resume) == "table" and resume.bookId and tostring(resume.bookId) ~= "" then
        pending_history_resume = {
          bookId = tostring(resume.bookId),
          title = tostring(resume.title or resume.bookId),
          providerId = tostring(resume.providerId or "jjwxc"),
          chapterUid = tostring(resume.chapterUid or ""),
          cacheRelPath = tostring(resume.cacheRelPath or ""),
          chapterIndex = tonumber(resume.chapterIndex),
          byteOffset = tonumber(resume.byteOffset),
        }
      end
    end
    if pending_history_resume and history_resume_cache_ready() then
      startup_job = nil
      if try_apply_history_resume() then return end
    end
    startup_job = nil
    finish_startup_shelf(Storage.load_shelf())
    return
  end
  dirty = true
end

cur_book = nil
chapters = {}
chapter_catalog = nil
toc_source = nil
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
chapter_uid = ""
chapter_job = nil
native_handoff_t0 = 0
PAGINATE_OPS_PER_STEP = 40
PAGINATE_SLICE_MS = 120
reader_page_prefer = nil
retry_chapter_idx = nil
native_progress = nil
pending_history_resume = nil
history_bg_restore = nil
toc_wordcount_reliable = false

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
    font_available = fi.available == nil and true or (fi.available and true or false)
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
    cur_book = cur_book,
    chapters = chapters,
    toc_page = toc_page,
    TOC_PAGE = TOC_PAGE,
    chapter_idx = chapter_idx,
    -- booklist (Lua fallback paint; host ui.listOpen uses booklist_* globals)
    booklist = booklist or {},
    booklist_page = booklist_page or 1,
    BOOKLIST_PAGE = BOOKLIST_PAGE or 12,
    cur_category = cur_category,
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
    login_qr = Auth.qr_data(),
    login_msg = Auth.login_msg,
    auth_has = Auth.has(),
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

