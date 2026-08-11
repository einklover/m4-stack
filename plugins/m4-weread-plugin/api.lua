-- WeRead HTTP API: shelf / toc / chapter / progress upload.
Api = {}
-- Stable body-provider sentinel. Only this exact value means that a chapter
-- has no readable body and may be skipped; transport/decode failures retain
-- their diagnostic error strings.
Api.EMPTY_BODY = "empty"

local HOST = "https://weread.qq.com"

function Api.jstr(v)
  return Auth.jstr(v)
end

-- Normalize transport failures before parsing the body.  Recent WeRead
-- deployments return a short HTML/empty 404 for malformed sync requests;
-- exposing that as `http_404` lets the UI choose cache/retry instead of
-- reporting a misleading JSON/empty-data error.
local function response_error(resp, fallback)
  if not resp then return fallback or "no_response" end
  local status = tonumber(resp.status) or 0
  local e = tostring(resp.error or "")
  if e == "" or e == "nil" or e == "0" or e == "0.0" then
    if status >= 400 then return "http_" .. tostring(status) end
    return fallback or "http_error"
  end
  return e
end

local function with_auth_retry(do_request)
  local r = do_request()
  if not r then return nil, "no_response" end
  Auth.absorb_set_cookie(r)
  local doc = nil
  if r.body and r.body ~= "" and r.body:sub(1, 1) == "{" then
    doc = json.decode(r.body)
  end
  if doc and Auth.is_login_timeout(doc) then
    if Auth.try_renew() then
      r = do_request()
      if r then Auth.absorb_set_cookie(r) end
      if r and r.body and r.body ~= "" and r.body:sub(1, 1) == "{" then
        doc = json.decode(r.body)
        if doc and Auth.is_login_timeout(doc) then
          return r, "login_timeout"
        end
      end
    else
      return r, "login_timeout"
    end
  end
  return r, nil, doc
end

function Api.http_get(path, timeout)
  return with_auth_retry(function()
    return net.request("GET", HOST .. path, {
      headers = { Cookie = Auth.cookie_header(), Referer = "https://weread.qq.com/" },
      timeout_ms = timeout or 20000,
    })
  end)
end

function Api.http_post(path, body, timeout, referer)
  return with_auth_retry(function()
    return net.request("POST", HOST .. path, {
      headers = {
        Cookie = Auth.cookie_header(),
        Referer = referer or "https://weread.qq.com/",
        ["Content-Type"] = "application/json",
      },
      body = body or "",
      timeout_ms = timeout or 25000,
    })
  end)
end

-- Prefer host-side projection for the shelf.  Shelf responses may include
-- cover/metadata blobs that are irrelevant to the plugin; decoding them as one
-- Lua JSON document creates a large temporary peak.  If the bridge is absent,
-- retain the compatibility path below.
local function fetch_shelf_projected()
  if type(dl) ~= "table" or type(dl.jsonGet) ~= "function" then
    return nil, "unsupported"
  end
  local rows, err = dl.jsonGet({
    url = HOST .. "/web/shelf/sync",
    headers = { Cookie = Auth.cookie_header(), Referer = "https://weread.qq.com/" },
    path = { "books" },
    fields = { "bookId", "title", "author", "progress" },
    max_bytes = 2 * 1024 * 1024,
    timeout_ms = 25000,
  })
  if not rows then return nil, err or "shelf_projection" end
  local books = {}
  for i = 1, #rows do
    local b = rows[i]
    local id = Api.jstr(b and b.bookId or "")
    if id ~= "" then
      books[#books + 1] = {
        bookId = id,
        title = Api.jstr(b.title or "无标题"),
        author = Api.jstr(b.author or ""),
        progress = tonumber(b.progress) or 0,
        updateTime = 0,
        progressChapterUid = "",
      }
    end
  end
  if #books == 0 then return nil, "empty" end
  table.sort(books, function(a, b) return (a.updateTime or 0) > (b.updateTime or 0) end)
  Storage.save_shelf_cache(books)
  return books, nil
end

function Api.fetch_shelf()
  local projected, projection_err = fetch_shelf_projected()
  if projected then return projected, nil end
  if projection_err ~= "unsupported" then return nil, projection_err end
  local r, err, doc = Api.http_get("/web/shelf/sync", 25000)
  if err == "login_timeout" then return nil, "2012" end
  if not r or not r.body or r.body == "" then
    return nil, (r and r.error) or "空响应"
  end
  doc = doc or json.decode(r.body)
  r.body = nil
  if not doc then return nil, "json" end
  if Auth.is_login_timeout(doc) then return nil, "2012" end

  local list = doc.books or {}
  local progress = doc.bookProgress or {}
  -- A malformed/very old shelf can contain thousands of entries.  Keep the
  -- newest bounded window; the UI and provider only need a practical shelf,
  -- and trimming in place avoids a second table allocation.
  for i = #list, 257, -1 do list[i] = nil end
  for i = #progress, 513, -1 do progress[i] = nil end
  local prog_map = {}
  for i = 1, #progress do
    local p = progress[i]
    if p and p.bookId then prog_map[Api.jstr(p.bookId)] = p end
  end
  local books = {}
  for i = 1, #list do
    local b = list[i]
    if b then
      local book = b.book or b
      local id = Api.jstr(book.bookId or b.bookId)
      if id ~= "" then
        local pr = prog_map[id] or {}
        books[#books + 1] = {
          bookId = id,
          title = Api.jstr(book.title or b.title or "无标题"),
          author = Api.jstr(book.author or b.author or ""),
          progress = tonumber(pr.progress) or tonumber(book.progress) or 0,
          updateTime = tonumber(pr.readUpdateTime or pr.updateTime) or 0,
          progressChapterUid = Api.jstr(pr.chapterUid or ""),
        }
      end
    end
  end
  table.sort(books, function(a, b) return (a.updateTime or 0) > (b.updateTime or 0) end)
  Storage.save_shelf_cache(books)
  return books, nil
end

function Api.fetch_toc(bookId)
  -- `synckeys`/`teenmode` are optional on older web builds but required by
  -- current chapterInfos routing.  Supplying them is backward compatible and
  -- avoids the otherwise opaque HTTP 404 seen on real devices.
  local body = '{"bookIds":["' .. bookId .. '"],"synckeys":[0],"teenmode":0}'
  local r, err, doc = Api.http_post("/web/book/chapterInfos", body, 30000)
  if err == "login_timeout" then return nil, "2012" end
  if not r then
    return nil, response_error(r, err or "no_response")
  end
  if tonumber(r.status) and tonumber(r.status) >= 400 then
    return nil, response_error(r, "http_error")
  end
  if not r.body or r.body == "" then
    return nil, response_error(r, "empty_response")
  end
  doc = doc or json.decode(r.body)
  r.body = nil
  if not doc then return nil, "json" end
  if Auth.is_login_timeout(doc) then return nil, "2012" end
  local data = doc.data
  if type(data) ~= "table" then return nil, "无 data" end

  local chosen = nil
  for i = 1, #data do
    local item = data[i]
    if item then
      local bid = Api.jstr(item.bookId or (item.book and item.book.bookId))
      if bid == bookId then
        chosen = item
        break
      end
      if not chosen then chosen = item end
    end
  end
  local chapters = {}
  if chosen then
    local updated = chosen.updated or chosen.chapterInfos or {}
    for i = #updated, 4097, -1 do updated[i] = nil end
    for j = 1, #updated do
      local ch = updated[j]
      if ch then
        local uid = Api.jstr(ch.chapterUid)
        if uid ~= "" then
          local item = {
            chapterUid = uid,
            title = Api.jstr(ch.title or ("第" .. tostring(j) .. "章")),
            chapterIdx = tonumber(ch.chapterIdx) or j,
          }
          -- A zero word count is the service's representation of a volume /
          -- section heading.  Preserve nil when the field is absent so an
          -- older response cannot accidentally classify every row as empty.
          if ch.wordCount ~= nil then item.wordCount = tonumber(ch.wordCount) or 0 end
          if ch.isParent ~= nil then item.isParent = ch.isParent end
          if ch.isEmpty ~= nil then item.isEmpty = ch.isEmpty end
          if ch.chapterType ~= nil then item.chapterType = ch.chapterType end
          chapters[#chapters + 1] = item
        end
      end
    end
  end
  if #chapters == 0 then return nil, "empty" end
  Storage.save_toc(bookId, chapters)
  return chapters, nil
end

-- FileRows TOC path. The current bridge may not support POST/body in
-- jsonToFile yet; callers fall back to fetch_toc without changing behavior.
function Api.fetch_toc_to_file(bookId, relPath)
  local body = '{"bookIds":["' .. tostring(bookId) .. '"],"synckeys":[0],"teenmode":0}'
  if type(dl) ~= "table" or type(dl.jsonToFile) ~= "function" then
    return nil, "no_jsonToFile"
  end
  local res, err = dl.jsonToFile({
    method = "POST",
    url = HOST .. "/web/book/chapterInfos",
    headers = {
      Cookie = Auth.cookie_header(),
      Referer = "https://weread.qq.com/",
      ["Content-Type"] = "application/json",
    },
    body = body,
    out = relPath,
    path = { "data", "updated" },
    -- Keep wordCount in the row source so native TOC clicks can skip empty
    -- volume headings without fetching their body first.
    fields = { "chapterUid", "title", "wordCount" },
    max_bytes = 8 * 1024 * 1024,
    timeout_ms = 30000,
  })
  if not res or not res.ok then return nil, response_error(res, err or "jsonToFile") end
  if tonumber(res.count or 0) < 1 then return nil, "empty" end
  return tonumber(res.count), nil
end

-- psvts cache: bookId -> psvts
Api._psvts = { bookId = nil, psvts = nil }

function Api.fetch_psvts(bookId, chapterUid, force)
  if not force and Api._psvts.bookId == bookId and Api._psvts.psvts then
    return Api._psvts.psvts, nil
  end
  local rurl = HOST .. "/web/reader/" .. weread.e(bookId)
  if chapterUid and chapterUid ~= "" then rurl = rurl .. "k" .. weread.e(chapterUid) end
  -- Stream-scan only: reader HTML can exceed generic body / Lua heap caps (~768KB / 512KB).
  local r, err = with_auth_retry(function()
    return net.extractPsvts(rurl, {
      headers = { Cookie = Auth.cookie_header(), Referer = "https://weread.qq.com/" },
      timeout_ms = 30000,
    })
  end)
  if err == "login_timeout" then return nil, "2012" end
  if not r then return nil, "reader fail" end
  if r.ok and r.value and r.value ~= "" then
    Api._psvts.bookId = bookId
    Api._psvts.psvts = r.value
    return r.value, nil
  end
  -- Stable host errors: psvts_not_found | scan_too_large | timeout | ...
  local e = response_error(r, "psvts_not_found")
  if e == "psvts_not_found" or e == "psvts_unclosed" or e == "psvts_value_too_large"
      or e == "scan_too_large" or e == "timeout" or e == "idle_timeout"
      or e == "cancelled" or e == "oom" or e == "https_required"
      or e == "wifi_not_connected" then
    return nil, e
  end
  return nil, "reader " .. tostring(r.status or "fail") .. " " .. e
end

-- Incremental chapter fetch so UI can paint between network hops.
-- job.step: psvts → e0 → (psvts_refresh?) → t0/t1 or e1/e3 → done
function Api.chapter_fetch_begin(bookId, ch)
  return {
    bookId = bookId,
    chapterUid = (ch and ch.chapterUid) or "",
    step = "psvts",
    psvts = nil,
    b0 = nil,
    is_txt = false,
    refreshed = false,
    e1_body = nil,
    e3_body = nil,
    t0_body = nil,
  }
end

-- Release all large per-chapter buffers as soon as a fetch leaves the network
-- state machine.  A chapter can briefly contain several encrypted shards plus
-- the decoded/plain copy; retaining the psvts token and old shard strings until
-- the next chapter makes the Lua heap fragment and eventually trips OOM.
function Api.release_chapter_buffers(job)
  if type(job) == "table" then
    job.psvts = nil
    job.b0 = nil
    job.e1_body = nil
    job.e3_body = nil
    job.t0_body = nil
  end
  if type(Api._psvts) == "table" and (not job or not job.bookId
      or Api._psvts.bookId == job.bookId) then
    Api._psvts.bookId = nil
    Api._psvts.psvts = nil
  end
end

local function chapter_post_shard(bookId, chapterUid, psvts, path)
  local rurl = HOST .. "/web/reader/" .. weread.e(bookId)
  if chapterUid ~= "" then rurl = rurl .. "k" .. weread.e(chapterUid) end
  local body = weread.makeContentParams(bookId, chapterUid, psvts, false, 1)
  return with_auth_retry(function()
    return net.request("POST", HOST .. path, {
      headers = {
        Cookie = Auth.cookie_header(),
        Referer = rurl,
        ["Content-Type"] = "application/json",
      },
      body = body,
      timeout_ms = 30000,
    })
  end)
end

local function e0_invalid(body)
  if not body or body == "" then return true end
  if body == "{}" then return true end
  return false
end

-- One network hop. Returns:
--   "busy", status_hint  — more hops needed
--   "done", plain_text
--   "error", err_string
function Api.chapter_fetch_step(job)
  if not job then return "error", "no_job" end
  local bookId = job.bookId
  local chapterUid = job.chapterUid or ""

  if job.step == "psvts" then
    local psvts, err = Api.fetch_psvts(bookId, chapterUid, job.force_psvts and true or false)
    job.force_psvts = false
    if not psvts then return "error", err or "psvts" end
    job.psvts = psvts
    job.step = "e0"
    return "busy", "下载章节…"
  end

  if job.step == "e0" then
    local e0, e0err = chapter_post_shard(bookId, chapterUid, job.psvts, "/web/book/chapter/e_0")
    if e0err == "login_timeout" then return "error", "2012" end
    local b0 = (e0 and e0.body) or ""
    if (not e0 or not e0.ok) or e0_invalid(b0) then
      if not job.refreshed then
        job.refreshed = true
        job.force_psvts = true
        Api.release_chapter_buffers(job)
        job.step = "psvts"
        return "busy", "刷新凭证…"
      end
      if not e0 or not e0.ok then
        Api.release_chapter_buffers(job)
        return "error", "e_0 " .. response_error(e0, "failed")
      end
      Api.release_chapter_buffers(job)
      return "error", Api.EMPTY_BODY
    end
    if #b0 >= 2 and b0:sub(1, 2) == "PK" then
      return "error", "整本 EPUB ZIP 暂不支持"
    end
    if b0 == "{}" then return "error", Api.EMPTY_BODY end
    job.b0 = b0
    job.is_txt = (#b0 > 0 and b0:sub(1, 1) == "{" and string.find(b0, '"bookId"', 1, true)) and true or false
    if job.is_txt then
      job.step = "t0"
    else
      job.step = "e1"
    end
    return "busy", "下载正文…"
  end

  if job.step == "t0" then
    local t0, terr = chapter_post_shard(bookId, chapterUid, job.psvts, "/web/book/chapter/t_0")
    if terr == "login_timeout" then return "error", "2012" end
    if not t0 or not t0.ok then return "error", "t_0 " .. response_error(t0, "failed") end
    job.t0_body = t0.body or ""
    job.step = "t1"
    return "busy", "下载正文…"
  end

  if job.step == "t1" then
    local t1 = chapter_post_shard(bookId, chapterUid, job.psvts, "/web/book/chapter/t_1")
    -- The token is no longer needed once t_1 has arrived.  Drop it before the
    -- decoder allocates the plaintext copy.
    Api.release_chapter_buffers({ bookId = bookId, psvts = job.psvts })
    job.psvts = nil
    local t1_body = (t1 and t1.body) or ""
    local plain = weread.decodeShards(job.t0_body or "", t1_body, "")
    t1_body = nil
    t1 = nil
    job.b0 = nil
    job.t0_body = nil
    if collectgarbage then collectgarbage("collect") end
    if not plain or plain == "" then
      Api.release_chapter_buffers(job)
      return "error", Api.EMPTY_BODY
    end
    job.step = "done"
    return "done", plain
  end

  if job.step == "e1" then
    local e1, e1err = chapter_post_shard(bookId, chapterUid, job.psvts, "/web/book/chapter/e_1")
    if e1err == "login_timeout" then return "error", "2012" end
    if not e1 or not e1.ok then
      if not job.refreshed then
        job.refreshed = true
        job.force_psvts = true
        Api.release_chapter_buffers(job)
        job.step = "psvts"
        return "busy", "刷新凭证…"
      end
      Api.release_chapter_buffers(job)
      return "error", "e_1 " .. response_error(e1, "failed")
    end
    job.e1_body = e1.body or ""
    job.step = "e3"
    return "busy", "下载正文…"
  end

  if job.step == "e3" then
    local e3 = chapter_post_shard(bookId, chapterUid, job.psvts, "/web/book/chapter/e_3")
    Api.release_chapter_buffers({ bookId = bookId, psvts = job.psvts })
    job.psvts = nil
    local e3_body = (e3 and e3.body) or ""
    local decoded = weread.decodeShards(job.b0 or "", job.e1_body or "", e3_body)
    e3_body = nil
    e3 = nil
    job.b0 = nil
    job.e1_body = nil
    if collectgarbage then collectgarbage("collect") end
    if not decoded or decoded == "" then
      Api.release_chapter_buffers(job)
      return "error", Api.EMPTY_BODY
    end
    local plain = weread.stripXhtml(decoded)
    decoded = nil
    if collectgarbage then collectgarbage("collect") end
    job.step = "done"
    return "done", plain
  end

  Api.release_chapter_buffers(job)
  return "error", "bad_step"
end

-- Blocking whole-chapter fetch (compat for tests / one-shot callers).
function Api.fetch_chapter_text(bookId, ch)
  local job = Api.chapter_fetch_begin(bookId, ch)
  for _ = 1, 24 do
    local st, payload = Api.chapter_fetch_step(job)
    if st == "done" then
      Api.release_chapter_buffers(job)
      return payload, nil
    end
    if st == "error" then
      Api.release_chapter_buffers(job)
      return nil, payload
    end
    -- busy: continue
  end
  Api.release_chapter_buffers(job)
  return nil, "fetch_steps_exhausted"
end

-- Progress upload is DISABLED by default until a verified signed protocol test exists.
-- Set Api.ENABLE_PROGRESS_UPLOAD = true only with fixture evidence (never block draw).
Api.ENABLE_PROGRESS_UPLOAD = false

function Api.upload_progress(bookId, chapterUid, page, percent)
  if not Api.ENABLE_PROGRESS_UPLOAD then
    return false, "upload_disabled"
  end
  local body = string.format(
    '{"bookId":"%s","chapterUid":"%s","chapterIdx":%d,"appId":"weread_wxmini","progress":%d}',
    bookId, chapterUid, page or 1, percent or 0
  )
  local r, err = Api.http_post("/web/book/read", body, 15000)
  if err == "login_timeout" then return false, "2012" end
  if not r or not r.ok then return false, (r and r.error) or "upload_fail" end
  return true, nil
end

return Api
