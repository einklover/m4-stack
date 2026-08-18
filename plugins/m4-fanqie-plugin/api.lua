-- 番茄小说 book source: https://fq-book.netsite.cc (legado source format).
-- Plain JSON APIs; no login / no shard crypto (unlike weread).
Api = {}

local HOST = "https://fq-book.netsite.cc"
-- Chapter bodies 302 from HOST to this NAT mirror on :8043. Device TLS
-- cannot auto-follow that hop (ESP_ERR_HTTP_CONNECT); hit the mirror
-- directly with today's UTC date (required `nt=` query).
local CHAPTER_HOST = "https://fq-book.nat.netsite.cc:8043"
local UA = "Mozilla/5.0 (Linux; Android 10.0; wv) AppleWebKit/603.1.30 (KHTML, like Gecko) Version/4.0 Chrome/58.0.3029.110 Mobile Safari/537.36 T7/10.3 SearchCraft/2.6.2 (Baidu; P1 7.0)"

local function headers()
  return { ["User-Agent"] = UA, Referer = "https://fanqienovel.com/" }
end

local function chapter_url(itemId)
  local nt = os.date("!%Y-%m-%d")
  -- Device clock is often unset; 1970-01-01 makes the mirror return empty.
  if type(nt) ~= "string" or nt < "2024-01-01" then
    nt = os.date("!%Y-%m-%d") -- keep if host clock is sane
    if type(nt) ~= "string" or nt < "2024-01-01" then nt = "2026-08-18" end
  end
  return CHAPTER_HOST .. "/content?item_id=" .. tostring(itemId) .. "&nt=" .. nt
end

local function http_get(path, timeout)
  local r = net.request("GET", path, {
    headers = headers(),
    timeout_ms = timeout or 20000,
    -- M4 host follows redirects itself; newer/alternate hosts may consume
    -- this hint as well.  Headers are public (UA/Referer only), so redirect
    -- following cannot forward credentials.
    follow_redirects = true,
  })
  if not r then return nil, "no_response" end
  if not r.ok then return nil, r.error or ("http_" .. tostring(r.status)) end
  if not r.body or r.body == "" then return nil, "empty_response" end
  return r.body, nil
end

local function decode_or_err(body)
  local ok, doc = pcall(json.decode, body)
  if not ok or type(doc) ~= "table" then return nil, "json" end
  return doc, nil
end

-- Category books. gender: 1=男生 0=女生; page is 1-based.
-- limit: keep each decoded page small. The host shows six rows and replaces
-- pages instead of retaining an ever-growing Lua list.
function Api.fetch_category(category_id, gender, page, genre_type, limit)
  page = math.max(1, tonumber(page) or 1)
  limit = math.max(5, math.min(50, tonumber(limit) or 6))
  local offset = (page - 1) * limit
  local url = "https://novel.snssdk.com/api/novel/channel/homepage/new_category/book_list/v1/"
    .. "?parent_enterfrom=novel_channel_category.tab.&aid=1967&offset=" .. tostring(offset)
    .. "&limit=" .. tostring(limit) .. "&category_id=" .. tostring(category_id)
    .. "&gender=" .. tostring(gender or 1)
  if genre_type then
    url = url .. "&genre_type=" .. tostring(genre_type)
  end

  -- Prefer host-side JSON projection when available.  The category response
  -- contains long abstracts and metadata that the plugin never uses; decoding
  -- the complete body in Lua on every page leaves a large temporary peak and
  -- is the reason later pages can still hit low memory.  jsonGet keeps the
  -- response/parse pool outside the Lua heap and returns only six tiny rows.
  if type(dl) == "table" and type(dl.jsonGet) == "function" then
    local list, jerr = dl.jsonGet({
      url = url,
      headers = headers(),
      path = { "data", "data" },
      fields = { "book_id", "book_name", "title", "author" },
      max_bytes = 2 * 1024 * 1024,
      timeout_ms = 25000,
    })
    if not list then return nil, jerr end
    local books = {}
    for i = 1, #list do
      local it = list[i]
      if type(it) == "table" and it.book_id and tostring(it.book_id) ~= "" then
        local title = tostring(it.book_name or "")
        if title == "" then title = tostring(it.title or "无标题") end
        books[#books + 1] = {
          bookId = tostring(it.book_id),
          title = title,
          author = tostring(it.author or ""),
        }
      end
    end
    if #books == 0 then return nil, "empty" end
    return books, nil
  end

  local body, err = http_get(url, 25000)
  if not body then return nil, err end
  local doc, jerr = decode_or_err(body)
  body = nil
  if collectgarbage then collectgarbage("collect") end
  if not doc then return nil, jerr end
  local data = doc.data and doc.data.data
  if type(data) ~= "table" then return nil, "no_data" end
  local books = {}
  for i = 1, #data do
    local it = data[i]
    if type(it) == "table" and it.book_id then
      books[#books + 1] = {
        bookId = tostring(it.book_id),
        title = tostring(it.book_name or it.title or "无标题"),
        author = tostring(it.author or ""),
      }
    end
  end
  if #books == 0 then return nil, "empty" end
  return books, nil
end

-- TOC via fanqie public reader API. Large books return hundreds of KB of JSON
-- (all chapters at once), which cannot fit the 512KB Lua heap. Use dl.jsonToFile:
-- the host streams + parses the body in PSRAM and writes one tab-separated
-- record per chapter to `relPath` ("virtual memory": rows stay on SD).
-- Returns count or nil, err.
function Api.fetch_toc_to_file(bookId, relPath)
  local url = "https://fanqienovel.com/api/reader/directory/detail?bookId=" .. tostring(bookId)
  if type(dl) == "table" and type(dl.jsonToFile) == "function" then
    -- Older hosts did not create parent directories for transactional
    -- downloads.  Touch the destination through the sandbox FS first so a
    -- fresh install can still fetch cache/<bookId>/toc_rows.txt; newer hosts
    -- make this a harmless no-op because the file already exists.
    if type(fs) == "table" and type(fs.fileSize) == "function"
        and type(fs.writeFile) == "function" and fs.fileSize(relPath) == nil then
      fs.writeFile(relPath, "")
    end
    local res, err = dl.jsonToFile({
      url = url,
      headers = headers(),
      out = relPath,
      path = { "data", "chapterListWithVolume" },
      fields = { "itemId", "title" },
      max_bytes = 4 * 1024 * 1024,
      timeout_ms = 30000,
    })
    if not res or not res.ok then return nil, err or "jsonToFile" end
    if tonumber(res.count or 0) == 0 then return nil, "empty" end
    return tonumber(res.count), nil
  end
  return nil, "no_jsonToFile"
end

-- Legacy host: full TOC via net.request + json.decode (small books only).
-- Returns chapters list or nil, err.
function Api.fetch_toc(bookId)
  local url = "https://fanqienovel.com/api/reader/directory/detail?bookId=" .. tostring(bookId)
  if type(dl) == "table" and type(dl.jsonGet) == "function" then
    local list, err = dl.jsonGet({
      url = url,
      headers = headers(),
      path = { "data", "chapterListWithVolume" },
      fields = { "itemId", "title" },
      max_bytes = 4 * 1024 * 1024,
      timeout_ms = 30000,
    })
    if not list then return nil, err end
    -- Convert in place (itemId -> chapterUid) to avoid a second ~100KB table
    -- for a 1000+ chapter book; the 512KB Lua heap cannot hold two copies.
    for i = 1, #list do
      local it = list[i]
      local uid = tostring(it.itemId or "")
      it.chapterUid = uid
      it.itemId = nil
    end
    if #list == 0 then return nil, "empty" end
    return list, nil
  end
  -- Legacy host fallback: net.request + json.decode (small books only).
  local body, err = http_get(url, 30000)
  if not body then return nil, err end
  local doc, jerr = decode_or_err(body)
  body = nil
  if collectgarbage then collectgarbage("collect") end
  if not doc then return nil, jerr end
  local data = doc.data
  if type(data) ~= "table" then return nil, "no_data" end
  local chapters = {}
  local vols = data.chapterListWithVolume
  if type(vols) == "table" then
    for vi = 1, #vols do
      local v = vols[vi]
      if type(v) == "table" then
        for ci = 1, #v do
          local c = v[ci]
          if type(c) == "table" and c.itemId then
            chapters[#chapters + 1] = {
              chapterUid = tostring(c.itemId),
              title = tostring(c.title or ("第" .. tostring(#chapters + 1) .. "章")),
            }
            if #chapters >= 900 then break end
          end
        end
      end
      if #chapters >= 900 then break end
    end
  end
  if #chapters == 0 then return nil, "empty" end
  return chapters, nil
end

-- Declarative chapter load for system progressive loader (firmware ≥ loader API).
function Api.chapter_loader_spec(bookId, ch, relPath, openMeta)
  local itemId = tostring((ch and ch.chapterUid) or "")
  if itemId == "" or type(relPath) ~= "string" or relPath == "" then return nil end
  openMeta = openMeta or {}
  return {
    url = chapter_url(itemId),
    headers = headers(),
    out = relPath,
    extract = { kind = "json_field", path = { "data", "data" }, field = "content" },
    -- No early open: the native TxtReader paginates once from file size at
    -- open, so an early 2KB open would freeze the chapter at ~4 pages while
    -- the host keeps streaming the rest into the same file.  Open only when
    -- the body is complete (host finishOk triggers the open).
    early_bytes = 0,
    max_bytes = 4 * 1024 * 1024,
    timeout_ms = 30000,
    follow_redirects = true,
    title = openMeta.title or "",
    bookId = tostring(bookId or ""),
    chapterUid = itemId,
    progressKey = openMeta.progressKey or ("fanqie:" .. tostring(bookId) .. ":" .. itemId),
    providerId = openMeta.providerId or "fanqie",
    chapterIndex = openMeta.chapterIndex or 0,
  }
end

function Api.toc_loader_spec(bookId, relPath, openMeta)
  openMeta = openMeta or {}
  return {
    url = "https://fanqienovel.com/api/reader/directory/detail?bookId=" .. tostring(bookId),
    headers = headers(),
    out = relPath,
    path = { "data", "chapterListWithVolume" },
    fields = { "itemId", "title" },
    early_rows = 40,
    max_bytes = 4 * 1024 * 1024,
    timeout_ms = 30000,
    follow_redirects = true,
    bookId = tostring(bookId or ""),
    title = openMeta.title or "",
    providerId = openMeta.providerId or "fanqie",
    currentIndex = openMeta.currentIndex or 0,
    uidField = 0,
    titleField = 1,
  }
end

-- One-shot full chapter text (single JSON GET; no shards).
-- Prefer writing the scalar body directly to SD on a host that supports the
-- file_out extension.  The response never crosses the Lua boundary, so a
-- large chapter cannot double the Lua heap before it is cached.
function Api.fetch_chapter_to_file(bookId, ch, relPath)
  local itemId = tostring((ch and ch.chapterUid) or "")
  if itemId == "" or type(relPath) ~= "string" or relPath == "" then
    return nil, "no_item_id"
  end
  if type(dl) ~= "table" or type(dl.jsonGet) ~= "function" then
    return nil, "unsupported"
  end
  local url = chapter_url(itemId)
  local res, jerr = dl.jsonGet({
    url = url,
    headers = headers(),
    -- fq-book.netsite.cc returns a short-lived public mirror redirect for
    -- chapter bodies.  The host follows it only when explicitly requested
    -- and rejects credential-bearing headers, so this remains safe here.
    follow_redirects = true,
    path = { "data", "data" },
    fields = { "content" },
    file_out = relPath,
    max_bytes = 4 * 1024 * 1024,
    timeout_ms = 30000,
  })
  if type(res) == "table" and res.ok and tonumber(res.size or 0) > 0 then
    return tonumber(res.size), nil
  end
  -- An old host ignores the optional file_out key and returns the normal
  -- projected rows.  Keep the legacy Lua path for that case.
  if type(res) == "table" and res.ok == nil then return nil, "unsupported" end
  return nil, jerr or "chapter_file_failed"
end

-- Legacy one-shot full chapter text (single JSON GET; no shards).
function Api.fetch_chapter_text(bookId, ch)
  local itemId = tostring((ch and ch.chapterUid) or "")
  if itemId == "" then return nil, "no_item_id" end
  local url = chapter_url(itemId)

  -- The public mirror returns a short-lived redirect to a non-default TLS
  -- port.  The generic net.request path follows redirects in the host's
  -- bounded/manual redirect loop; use it first for chapter bodies.  The
  -- projected dl.jsonGet path uses Arduino HTTPClient and some firmware
  -- builds reject that redirect as `http_request_failed` even though the
  -- same request is valid.  Chapters are bounded by the host response cap,
  -- so decoding this one body is safe and remains a reliable fallback.
  local body, net_err = http_get(url, 30000)
  if body then
    local doc, jerr = decode_or_err(body)
    body = nil
    if collectgarbage then collectgarbage("collect") end
    if doc then
      local content = doc.data and doc.data.data and doc.data.data.content
      if type(content) == "string" and content ~= "" then
        return Api.clean_content(content), nil
      end
      net_err = "empty_content"
    else
      net_err = jerr or "json"
    end
  end

  -- Keep the large response out of the Lua heap when the host supports the
  -- projected JSON reader.  Only the scalar content field crosses the bridge;
  -- the HTTP body and JSON document remain host-side.
  if type(dl) == "table" and type(dl.jsonGet) == "function" then
    local rows, jerr = dl.jsonGet({
      url = url,
      headers = headers(),
      -- The public content endpoint returns a short-lived mirror redirect.
      -- Follow it for both the SD file path and the Lua fallback; otherwise
      -- older hosts surface the opaque "http request failed" error.
      follow_redirects = true,
      path = { "data", "data" },
      fields = { "content" },
      max_bytes = 4 * 1024 * 1024,
      timeout_ms = 30000,
    })
    local content = rows and rows[1] and rows[1].content
    if type(content) ~= "string" or content == "" then
      return nil, jerr or net_err or "empty_content"
    end
    return Api.clean_content(content), nil
  end
  return nil, net_err or "http_request_failed"
end

-- Clean chapter body: drop ad substrings, keep paragraphs as-is (UTF-8 text).
function Api.clean_content(content)
  -- Most chapters contain no removable ad marker. Avoid allocating a line
  -- table and a second full-size concatenated string in that common case.
  if type(content) == "string" and not string.find(content, "收听有声版", 1, true) then
    return content
  end
  -- On old hosts the complete chapter already occupies a large Lua string.
  -- Do not build a second line table/concat copy near the heap limit; the
  -- streamed file_out path is the normal path for these chapters anyway.
  if type(content) == "string" and #content > 300000 then return content end
  local out = {}
  for line in string.gmatch(content .. "\n", "([^\n]*)\n") do
    -- Legado source rule: strip "收听有声版" wherever it appears.
    local t = line:gsub("收听有声版", "")
    -- Drop pure-cruft lines.
    local trimmed = t:gsub("^%s+", ""):gsub("%s+$", "")
    if trimmed ~= "" then
      out[#out + 1] = t
    end
  end
  return table.concat(out, "\n")
end

-- Search (kept for future/keyboard hosts; not reachable from current UI).
function Api.search(query, page)
  local url = HOST .. "/search?query=" .. crypto.urlEncode(query) .. "&page=" .. tostring(page or 1)
  local body, err = http_get(url, 25000)
  if not body then return nil, err end
  local doc, jerr = decode_or_err(body)
  body = nil
  if collectgarbage then collectgarbage("collect") end
  if not doc then return nil, jerr end
  local books = {}
  local tabs = doc.data and doc.data.search_tabs
  if type(tabs) == "table" then
    for ti = 1, #tabs do
      local data = tabs[ti] and tabs[ti].data
      if type(data) == "table" then
        for di = 1, #data do
          local bd = data[di] and data[di].book_data
          if type(bd) == "table" then
            for bi = 1, #bd do
              local b = bd[bi]
              if type(b) == "table" and b.book_id then
                books[#books + 1] = {
                  bookId = tostring(b.book_id),
                  title = tostring(b.book_name or "无标题"),
                  author = tostring(b.author or ""),
                  intro = tostring(b.abstract or ""),
                }
                if #books >= 50 then return books, nil end
              end
            end
          end
        end
      end
    end
  end
  if #books == 0 then return nil, "empty" end
  return books, nil
end

return Api
