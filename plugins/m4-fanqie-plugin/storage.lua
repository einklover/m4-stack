-- Atomic-ish config/cache/progress helpers for com.weread.client
-- Phase 5A: chapter bodies stay on SD; Lua uses fileSize/readRange windows.
Storage = {}

-- History/provider records can be partial (for example a cold-start record
-- may only contain bookId and chapterUid).  Never let a missing identifier
-- reach Lua's `..` operator and terminate the plugin.
local function path_part(v)
  return tostring(v or "")
end

-- Page index format version (ASCII pidx header).
Storage.PIDX_VERSION = 1
-- Must match Layout.MAX_PAGES — reject corrupt/huge pidx before allocating.
Storage.MAX_PAGES = 4096
-- Must match Layout.READ_RANGE_MAX / host fs.readRange hard cap.
Storage.MAX_PAGE_SPAN = 16384
-- Legacy Lua fallback cap; native provider reads larger files by range.
Storage.MAX_LEGACY_CHAPTER = 400000

function Storage.read_json(rel)
  local raw = fs.readFile(rel)
  if not raw or raw == "" then return nil end
  local doc = json.decode(raw)
  raw = nil
  return doc
end

function Storage.write_json(rel, doc)
  local s = json.encode(doc)
  if not s then return false end
  if fs.replaceFile then
    return fs.replaceFile(rel, s)
  end
  return fs.writeFile(rel, s)
end

local function progress_rel(bookId)
  return "progress/" .. tostring(bookId or "") .. ".json"
end

-- Per-book progress file.  Never rewrites one shared progress.json: with 200+
-- shelf books the shared file grows past the host readFile cap and every
-- load_progress re-decodes it, which surfaces as "too large" in the UI.
function Storage.load_progress(bookId)
  if not bookId or bookId == "" then return nil end
  local doc = Storage.read_json(progress_rel(bookId))
  if doc then return doc end
  -- Legacy: single shared progress.json from older plugin versions.
  local legacy = Storage.read_json("progress.json")
  if legacy and legacy[bookId] then return legacy[bookId] end
  return nil
end

function Storage.save_progress_entry(bookId, entry)
  if not bookId or bookId == "" then return false end
  return Storage.write_json(progress_rel(bookId), entry)
end

-- True when chapter body for bookId/chapterUid is a non-empty SD file.
function Storage.chapter_body_ready(bookId, chapterUid)
  if not bookId or not chapterUid or chapterUid == "" then return false end
  local n = Storage.chapter_file_size(bookId, chapterUid)
  return n ~= nil and n > 0
end

-- Pick best chapter uid for history reopen (resume hint → progress → nil).
function Storage.resolve_resume_chapter_uid(bookId, resume)
  if type(resume) == "table" and resume.chapterUid and tostring(resume.chapterUid) ~= "" then
    local uid = tostring(resume.chapterUid)
    if Storage.chapter_body_ready(bookId, uid) then return uid end
    -- Prefer host identity even if body missing (caller may fetch).
    return uid
  end
  local prog = Storage.load_progress(bookId)
  if type(prog) == "table" and prog.chapterUid and tostring(prog.chapterUid) ~= "" then
    return tostring(prog.chapterUid)
  end
  return nil
end

function Storage.save_shelf_cache(books)
  return Storage.write_json("shelf_cache.json", { books = books })
end

function Storage.load_shelf_cache()
  local doc = Storage.read_json("shelf_cache.json")
  if doc and doc.books then return doc.books end
  return nil
end

-- Local shelf (books the user opened; no cloud account for fanqie).
function Storage.save_shelf(books)
  return Storage.write_json("shelf.json", { books = books or {} })
end

function Storage.load_shelf()
  local doc = Storage.read_json("shelf.json")
  if doc and type(doc.books) == "table" then
    for i = #doc.books, 257, -1 do doc.books[i] = nil end
    return doc.books
  end
  return nil
end

function Storage.save_toc(bookId, chapters)
  return Storage.write_json("cache/" .. path_part(bookId) .. "/toc.json", { chapters = chapters })
end

function Storage.load_toc(bookId)
  local doc = Storage.read_json("cache/" .. path_part(bookId) .. "/toc.json")
  if doc and type(doc.chapters) == "table" then
    for i = #doc.chapters, 901, -1 do doc.chapters[i] = nil end
    return doc.chapters
  end
  return nil
end

function Storage.toc_rows_path(bookId)
  return "cache/" .. tostring(bookId) .. "/toc_rows.txt"
end

function Storage.save_catalog_meta(bookId, spec)
  if type(spec) ~= "table" then return false end
  return Storage.write_json("cache/" .. tostring(bookId) .. "/toc_catalog.json", {
    source = tostring(spec.source or ""),
    count = tonumber(spec.count) or 0,
    uid_field = tonumber(spec.uid_field) or 0,
    title_field = tonumber(spec.title_field) or 1,
  })
end

function Storage.load_catalog_meta(bookId)
  local doc = Storage.read_json("cache/" .. tostring(bookId) .. "/toc_catalog.json")
  if type(doc) == "table" and tostring(doc.source or "") ~= "" and tonumber(doc.count or 0) > 0 then
    return doc
  end
  return nil
end

function Storage.chapter_path(bookId, chapterUid)
  return "cache/" .. path_part(bookId) .. "/ch_" .. path_part(chapterUid) .. ".txt"
end

function Storage.pidx_path(bookId, chapterUid)
  return "cache/" .. path_part(bookId) .. "/ch_" .. path_part(chapterUid) .. ".pidx"
end

function Storage.save_chapter_text(bookId, chapterUid, text)
  -- Whole chapter body only. Requires atomic replaceFile (tmp+rename).
  -- Non-atomic writeFile(live) can expose a truncated formal file — refuse it.
  if type(text) ~= "string" or #text > (Storage.MAX_LEGACY_CHAPTER or 400000) then
    return false
  end
  local path = Storage.chapter_path(bookId, chapterUid)
  if type(fs.replaceFile) ~= "function" then
    return false
  end
  return fs.replaceFile(path, text)
end

-- Legacy full-file read (avoid for large chapters). Prefer fileSize/readRange.
function Storage.load_chapter_text(bookId, chapterUid)
  local path = Storage.chapter_path(bookId, chapterUid)
  if type(fs.fileSize) == "function" then
    local n = fs.fileSize(path)
    if n and n > (Storage.MAX_LEGACY_CHAPTER or 400000) then return nil end
  end
  return fs.readFile(path)
end

function Storage.chapter_file_size(bookId, chapterUid)
  if type(fs.fileSize) ~= "function" then return nil end
  return fs.fileSize(Storage.chapter_path(bookId, chapterUid))
end

function Storage.chapter_ready(bookId, chapterUid)
  local n = Storage.chapter_file_size(bookId, chapterUid)
  return n ~= nil and n > 0
end

-- layout fingerprint for pidx invalidation
function Storage.layout_fingerprint(margin, line_h, font, maxW, maxLines)
  return string.format("m%d_l%d_f%d_w%d_n%d",
    tonumber(margin) or 0,
    tonumber(line_h) or 0,
    tonumber(font) or 0,
    tonumber(maxW) or 0,
    tonumber(maxLines) or 0)
end

-- Save compact page-start index. Returns true on success.
function Storage.save_pidx(bookId, chapterUid, meta, page_starts)
  if type(fs.writeFile) ~= "function" then return false end
  if type(page_starts) ~= "table" or #page_starts < 1 then return false end
  local max_pages = Storage.MAX_PAGES or 4096
  if #page_starts > max_pages then return false end
  meta = meta or {}
  local lines = {
    "WRPI",
    "ver=" .. tostring(Storage.PIDX_VERSION),
    "size=" .. tostring(meta.size or 0),
    "layout=" .. tostring(meta.layout or ""),
    "count=" .. tostring(#page_starts),
  }
  for i = 1, #page_starts do
    lines[#lines + 1] = tostring(page_starts[i])
  end
  local body = table.concat(lines, "\n")
  -- Keep pidx well under full-file write cap.
  if #body > 400000 then return false end
  local path = Storage.pidx_path(bookId, chapterUid)
  if fs.replaceFile then
    return fs.replaceFile(path, body)
  end
  return fs.writeFile(path, body)
end

-- Load pidx if version/size/layout match. Returns page_starts or nil.
-- Hard-caps count at MAX_PAGES before appending body lines (OOM-safe).
function Storage.load_pidx(bookId, chapterUid, expect_size, expect_layout)
  local path = Storage.pidx_path(bookId, chapterUid)
  local raw = fs.readFile(path)
  if not raw or raw == "" then return nil end
  local max_pages = Storage.MAX_PAGES or 4096
  expect_size = tonumber(expect_size) or 0
  local ver, size, layout, count
  local starts = {}
  local mode = "hdr"
  for line in string.gmatch(raw .. "\n", "([^\n]*)\n") do
    if mode == "hdr" then
      if line == "WRPI" then
        -- ok
      elseif line:sub(1, 4) == "ver=" then
        ver = tonumber(line:sub(5))
      elseif line:sub(1, 5) == "size=" then
        size = tonumber(line:sub(6))
      elseif line:sub(1, 7) == "layout=" then
        layout = line:sub(8)
      elseif line:sub(1, 6) == "count=" then
        count = tonumber(line:sub(7))
        -- Reject immediately before allocating body entries.
        if not count or count < 1 or count > max_pages or count ~= math.floor(count) then
          return nil
        end
        mode = "body"
      else
        -- unknown header line; ignore
      end
    else
      -- Ignore blank lines (trailing newline after last offset).
      if line == "" then
        -- ok
      else
        -- Extra non-empty body lines beyond declared count → corrupt.
        if #starts >= count then return nil end
        local v = tonumber(line)
        if v == nil or v ~= math.floor(v) then return nil end
        starts[#starts + 1] = v
      end
    end
  end
  if ver ~= Storage.PIDX_VERSION then return nil end
  if size ~= expect_size then return nil end
  if layout ~= expect_layout then return nil end
  if not count or count ~= #starts then return nil end
  if starts[1] ~= 0 then return nil end

  if expect_size == 0 then
    -- Empty body: only a single zero start is valid.
    if #starts ~= 1 then return nil end
    return starts
  end

  -- Nonempty: starts[1]==0, strictly increasing, each offset < file size.
  -- Every page span, including the final page, must fit one bounded readRange.
  if starts[1] >= expect_size then return nil end
  local max_span = Storage.MAX_PAGE_SPAN or 16384
  for i = 2, #starts do
    local o = starts[i]
    if o <= starts[i - 1] then return nil end
    if o < 0 or o >= expect_size then return nil end
    if o - starts[i - 1] > max_span then return nil end
  end
  if expect_size - starts[#starts] > max_span then return nil end
  return starts
end

function Storage.clear_pidx(bookId, chapterUid)
  -- Best-effort: overwrite with empty via writeFile if present.
  local path = Storage.pidx_path(bookId, chapterUid)
  if fs.writeFile then
    pcall(fs.writeFile, path, "")
  end
end

return Storage
