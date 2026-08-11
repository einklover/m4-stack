-- UTF-8 safe pagination with SD-windowed chapter bodies.
--
-- Memory model (Phase 5A worst case, approximate):
--   * Sliding window string: WINDOW_BYTES (8 KiB)
--   * textWidth probe: FIT_MAX_BYTES (1 KiB)
--   * Page index: up to MAX_PAGES numbers (~page starts only, no per-line tables)
--   * Draw buffer: one page range via readRange (<= 16 KiB host cap, typically << that)
-- Peak Lua string windows stay under ~16 KiB; page_starts table is the main
-- growth term and is hard-capped at MAX_PAGES.
Layout = {}

Layout.FIT_MAX_BYTES = 1024
Layout.WINDOW_BYTES = 8192
Layout.MAX_PAGES = 4096
-- Host fs.readRange hard cap (must match M4xPathSafe::kMaxReadRangeBytes).
Layout.READ_RANGE_MAX = 16384
-- Geometry, hit testing, UTF-8 fitting and truncation live in the shared
-- package contract. Direct host tests provide sys.load just like firmware.
if type(sys) == "table" and type(sys.load) == "function" then
  sys.load("layout_contract.lua")
else
  dofile("test/fixtures/m4x/weread_src/layout_contract.lua")
end

-- ---- In-memory paginate (tests / tiny strings only) ----

local function job_flush_page_mem(job)
  if #job.cur_lines == 0 then return end
  job.pages[#job.pages + 1] = { lines = job.cur_lines }
  job.cur_lines = {}
end

local function job_push_line_mem(job, a, b)
  job.cur_lines[#job.cur_lines + 1] = { start = a, finish = b }
  if #job.cur_lines >= job.maxLines then job_flush_page_mem(job) end
end

function Layout.paginate_begin(text, margin, line_h, font)
  text = text or ""
  margin = margin or 24
  local metrics = Layout.metrics()
  line_h = math.max(line_h or 36, metrics.reader_line_h)
  font = font or 12
  local w = gui.width()
  local maxW = w - margin * 2
  if maxW < 1 then maxW = 1 end
  local top = metrics.content_y
  local bottom = metrics.content_bottom
  local maxLines = math.max(1, math.floor((bottom - top) / line_h))
  return {
    mode = "mem",
    text = text,
    n = #text,
    margin = margin,
    line_h = line_h,
    font = font,
    maxW = maxW,
    maxLines = maxLines,
    i = 1,
    cur_lines = {},
    pages = {},
    done = false,
    ops = 0,
  }
end

local function scan_chunk_end(text, from, n, max_bytes)
  local limit = from + max_bytes - 1
  if limit > n then limit = n end
  local j = from
  while j <= limit do
    local c = text:byte(j)
    if c == 10 or c == 13 then
      return j - 1, j, (j - from + 1)
    end
    j = j + 1
  end
  return limit, 0, (limit - from + 1)
end

function Layout.paginate_step_mem(job, max_ops)
  if not job or job.done then return "done", 100 end
  max_ops = tonumber(max_ops) or 32
  if max_ops < 1 then max_ops = 1 end
  local text = job.text
  local n = job.n
  local ops = 0
  local max_bytes = Layout.FIT_MAX_BYTES or 1024
  if max_bytes < 4 then max_bytes = 4 end

  while ops < max_ops and not job.done do
    local i = job.i
    if i > n then
      job_flush_page_mem(job)
      if #job.pages == 0 then
        job.pages[1] = { lines = { { start = 1, finish = 0 } } }
      end
      job.done = true
      break
    end
    local b = text:byte(i)
    if b == 13 then
      job.i = i + 1
      ops = ops + 1
    elseif b == 10 then
      job_push_line_mem(job, i, i - 1)
      job.i = i + 1
      ops = ops + 1
    else
      local hard_end, term_at = scan_chunk_end(text, i, n, max_bytes)
      if hard_end < i then
        job.i = i + 1
        ops = ops + 1
      else
        local cut = Layout.fit_line(text, i, job.maxW, job.font, hard_end)
        if cut < i then cut = i end
        if cut > hard_end then cut = hard_end end
        job_push_line_mem(job, i, cut)
        local ni = cut + 1
        if term_at > 0 and cut >= hard_end then
          ni = term_at
          if ni <= n and text:byte(ni) == 13 then ni = ni + 1 end
          if ni <= n and text:byte(ni) == 10 then ni = ni + 1 end
        end
        if ni <= i then ni = i + 1 end
        job.i = ni
        ops = ops + 1
      end
    end
  end
  job.ops = (job.ops or 0) + ops
  if job.done then return "done", 100 end
  if n <= 0 then
    job_flush_page_mem(job)
    if #job.pages == 0 then job.pages[1] = { lines = { { start = 1, finish = 0 } } } end
    job.done = true
    return "done", 100
  end
  local cursor = job.i
  if cursor < 1 then cursor = 1 end
  local pct = math.floor((cursor - 1) * 100 / n)
  if pct > 99 then pct = 99 end
  if pct < 0 then pct = 0 end
  return "running", pct
end

-- ---- SD file window pagination ----
-- job.page_starts: compact array of 0-based absolute byte offsets for page starts.
-- No per-line Lua tables during pagination.
--
-- Stability rule (P0): one pagination op captures a single contiguous window
-- and never refills mid-scan. Probe span is FIT_MAX_BYTES + UTF-8 overlap.

local function file_load_window(job, abs_pos)
  local n = job.n
  if abs_pos >= n then
    job.win = ""
    job.win_base = abs_pos
    return true
  end
  local win_bytes = Layout.WINDOW_BYTES or 8192
  local len = n - abs_pos
  if len > win_bytes then len = win_bytes end
  if len > (Layout.READ_RANGE_MAX or 16384) then len = Layout.READ_RANGE_MAX end
  if len < 1 then
    job.win = ""
    job.win_base = abs_pos
    return true
  end
  local chunk, err = fs.readRange(job.path, abs_pos, len)
  if chunk == nil then
    job.io_error = err or "read_failed"
    return false
  end
  job.win = chunk
  job.win_base = abs_pos
  return true
end

-- Ensure [abs_pos, abs_pos + probe_need) is covered by one stable window.
-- Reloads from abs_pos when remaining coverage is insufficient.
local function file_ensure_probe_window(job, abs_pos, probe_need)
  local n = job.n
  if abs_pos >= n then
    job.win = ""
    job.win_base = abs_pos
    return true
  end
  if probe_need < 4 then probe_need = 4 end
  local need_end = abs_pos + probe_need  -- exclusive
  if need_end > n then need_end = n end
  if job.win and job.win_base ~= nil then
    local covered_end = job.win_base + #job.win
    if abs_pos >= job.win_base and need_end <= covered_end then
      return true
    end
  end
  return file_load_window(job, abs_pos)
end

local function file_push_line(job)
  job.cur_line_count = (job.cur_line_count or 0) + 1
  if job.cur_line_count >= job.maxLines then
    job.cur_line_count = 0
    job._need_page_break = true
  end
end

local function file_record_page_break(job, at_pos, max_pages)
  if at_pos >= job.n then return true end
  if #job.page_starts >= max_pages then
    job.io_error = "too_many_pages"
    return false
  end
  job.page_starts[#job.page_starts + 1] = at_pos
  job.page_start_abs = at_pos
  job.cur_line_count = 0
  job._need_page_break = false
  return true
end

-- Every finished page byte span must be <= READ_RANGE_MAX (no silent draw truncate).
local function file_force_span_if_needed(job, max_pages)
  local cap = Layout.READ_RANGE_MAX or 16384
  local span = job.pos - (job.page_start_abs or 0)
  if span >= cap and job.pos < job.n then
    return file_record_page_break(job, job.pos, max_pages)
  end
  return true
end

function Layout.file_paginate_begin(path, file_size, margin, line_h, font)
  margin = margin or 24
  local metrics = Layout.metrics()
  line_h = math.max(line_h or 36, metrics.reader_line_h)
  font = font or 12
  local w = gui.width()
  local maxW = w - margin * 2
  if maxW < 1 then maxW = 1 end
  local top = metrics.content_y
  local bottom = metrics.content_bottom
  local maxLines = math.max(1, math.floor((bottom - top) / line_h))
  file_size = tonumber(file_size) or 0
  if file_size < 0 then file_size = 0 end
  return {
    mode = "file",
    path = path,
    n = file_size,
    margin = margin,
    line_h = line_h,
    font = font,
    maxW = maxW,
    maxLines = maxLines,
    pos = 0,           -- 0-based absolute
    win = "",
    win_base = 0,
    page_starts = { 0 }, -- compact: only page-start offsets
    page_start_abs = 0,  -- absolute start of the page currently being filled
    cur_line_count = 0,
    _need_page_break = false,
    done = false,
    ops = 0,
    io_error = nil,
  }
end

function Layout.file_paginate_step(job, max_ops)
  if not job or job.done then return "done", 100 end
  if job.io_error then return "error", 0 end
  max_ops = tonumber(max_ops) or 32
  if max_ops < 1 then max_ops = 1 end
  local n = job.n
  local ops = 0
  local max_bytes = Layout.FIT_MAX_BYTES or 1024
  if max_bytes < 4 then max_bytes = 4 end
  local max_pages = Layout.MAX_PAGES or 4096
  -- Contiguous probe: FIT_MAX + 4-byte UTF-8 overlap (never refill mid-op).
  local probe_need = max_bytes + 4

  while ops < max_ops and not job.done do
    local pos = job.pos
    if pos >= n then
      job.done = true
      break
    end

    if not file_ensure_probe_window(job, pos, probe_need) then
      return "error", 0
    end
    -- Capture window for this entire op — no mid-scan refill.
    local win = job.win
    local win_base = job.win_base
    local win_len = #win
    local from_rel = pos - win_base + 1
    if from_rel < 1 or from_rel > win_len then
      -- Window must cover pos after ensure; hard failure if not.
      job.io_error = "window_unstable"
      return "error", 0
    end
    local b = win:byte(from_rel)
    if b == nil then
      job.io_error = "read_failed"
      return "error", 0
    end

    if b == 13 then
      job.pos = pos + 1
      if not file_force_span_if_needed(job, max_pages) then return "error", 0 end
      ops = ops + 1
    elseif b == 10 then
      file_push_line(job)
      job.pos = pos + 1
      if job._need_page_break then
        if not file_record_page_break(job, job.pos, max_pages) then return "error", 0 end
      elseif not file_force_span_if_needed(job, max_pages) then
        return "error", 0
      end
      ops = ops + 1
    else
      -- Scan terminator and fit_line only inside the captured window.
      local probe_rel_end = from_rel + max_bytes - 1
      if probe_rel_end > win_len then probe_rel_end = win_len end
      local hard_rel = probe_rel_end
      local term_rel = 0
      local j = from_rel
      while j <= probe_rel_end do
        local c = win:byte(j)
        if c == 10 or c == 13 then
          hard_rel = j - 1
          term_rel = j
          break
        end
        j = j + 1
      end

      if hard_rel < from_rel then
        -- Immediate terminator at from_rel (should be rare in content branch).
        job.pos = pos + 1
        if not file_force_span_if_needed(job, max_pages) then return "error", 0 end
        ops = ops + 1
      else
        -- Cap line so current page span cannot exceed READ_RANGE_MAX.
        local cap = Layout.READ_RANGE_MAX or 16384
        local page_start = job.page_start_abs or 0
        local max_abs_end = page_start + cap - 1  -- inclusive last byte of page
        local hard_end_abs = win_base + hard_rel - 1
        if hard_end_abs > max_abs_end then
          -- If this line alone would start a page already full, break first.
          if pos > page_start and pos > max_abs_end then
            if not file_record_page_break(job, pos, max_pages) then return "error", 0 end
            page_start = job.page_start_abs
            max_abs_end = page_start + cap - 1
          end
          if hard_end_abs > max_abs_end then
            hard_end_abs = max_abs_end
            hard_rel = hard_end_abs - win_base + 1
            if hard_rel < from_rel then hard_rel = from_rel end
            term_rel = 0  -- soft split, not a real terminator
          end
        end

        local cut_rel = Layout.fit_line(win, from_rel, job.maxW, job.font, hard_rel)
        if cut_rel < from_rel then cut_rel = from_rel end
        if cut_rel > hard_rel then cut_rel = hard_rel end
        local cut_abs = win_base + cut_rel - 1
        if cut_abs < pos then cut_abs = pos end

        file_push_line(job)
        local ni = cut_abs + 1
        if term_rel > 0 and cut_rel >= hard_rel then
          -- Consume CR/LF still inside the captured window (no refill).
          local abs_term = win_base + term_rel - 1
          ni = abs_term + 1
          if win:byte(term_rel) == 13 and term_rel + 1 <= win_len and win:byte(term_rel + 1) == 10 then
            ni = abs_term + 2
          end
        end
        if ni <= pos then ni = pos + 1 end
        job.pos = ni
        if job._need_page_break then
          if not file_record_page_break(job, job.pos, max_pages) then return "error", 0 end
        elseif not file_force_span_if_needed(job, max_pages) then
          return "error", 0
        end
        ops = ops + 1
      end
    end
  end

  job.ops = (job.ops or 0) + ops
  if job.done then return "done", 100 end
  if n <= 0 then
    job.done = true
    return "done", 100
  end
  local pct = math.floor(job.pos * 100 / n)
  if pct > 99 then pct = 99 end
  if pct < 0 then pct = 0 end
  return "running", pct
end

function Layout.paginate_step(job, max_ops)
  if not job then return "done", 100 end
  if job.mode == "file" then
    return Layout.file_paginate_step(job, max_ops)
  end
  return Layout.paginate_step_mem(job, max_ops)
end

function Layout.paginate(text, margin, line_h, font)
  local job = Layout.paginate_begin(text, margin, line_h, font)
  local guard = 0
  local max_guard = math.max(16, math.floor((#text) / 8) + 64)
  while true do
    local st = Layout.paginate_step_mem(job, 64)
    if st == "done" then break end
    guard = guard + 1
    if guard > max_guard then break end
  end
  if not job.done then
    job_flush_page_mem(job)
    if #job.pages == 0 then
      job.pages[1] = { lines = { { start = 1, finish = 0 } } }
    end
    job.done = true
  end
  return job.pages
end

function Layout.paginate_offsets(text, margin, line_h, font)
  return Layout.paginate(text, margin, line_h, font)
end

-- Read a page body from SD: [start, finish) 0-based.
-- Returns chunk, err. Never silently truncates: page span must be <= READ_RANGE_MAX
-- (pagination enforces this; oversized span is a recoverable error for rebuild).
function Layout.read_file_span(path, start_off, finish_off)
  start_off = tonumber(start_off) or 0
  finish_off = tonumber(finish_off) or start_off
  if finish_off < start_off then finish_off = start_off end
  local len = finish_off - start_off
  if len <= 0 then return "", nil end
  local cap = Layout.READ_RANGE_MAX or 16384
  if len > cap then
    return nil, "page_too_large"
  end
  local chunk, err = fs.readRange(path, start_off, len)
  if chunk == nil then return nil, err or "read_failed" end
  return chunk, nil
end

-- Wrap a page-sized chunk into display lines (same fit rules as pagination).
function Layout.wrap_page_chunk(chunk, margin, line_h, font, max_lines)
  chunk = chunk or ""
  margin = margin or 24
  local metrics = Layout.metrics()
  line_h = math.max(line_h or 36, metrics.reader_line_h)
  font = font or 12
  local w = gui.width()
  local maxW = w - margin * 2
  if maxW < 1 then maxW = 1 end
  if not max_lines then
    local top = metrics.content_y
    local bottom = metrics.content_bottom
    max_lines = math.max(1, math.floor((bottom - top) / line_h))
  end
  local lines = {}
  local i = 1
  local n = #chunk
  local max_bytes = Layout.FIT_MAX_BYTES or 1024
  while i <= n and #lines < max_lines do
    local b = chunk:byte(i)
    if b == 13 then
      i = i + 1
    elseif b == 10 then
      lines[#lines + 1] = { start = i, finish = i - 1 }
      i = i + 1
    else
      local hard_end, term_at = scan_chunk_end(chunk, i, n, max_bytes)
      if hard_end < i then
        i = i + 1
      else
        local cut = Layout.fit_line(chunk, i, maxW, font, hard_end)
        if cut < i then cut = i end
        lines[#lines + 1] = { start = i, finish = cut }
        local ni = cut + 1
        if term_at > 0 and cut >= hard_end then
          ni = term_at
          if ni <= n and chunk:byte(ni) == 13 then ni = ni + 1 end
          if ni <= n and chunk:byte(ni) == 10 then ni = ni + 1 end
        end
        if ni <= i then ni = i + 1 end
        i = ni
      end
    end
  end
  if #lines == 0 then
    lines[1] = { start = 1, finish = 0 }
  end
  return lines
end

function Layout.line_text(text, line)
  if not line or not text then return "" end
  if line.finish < line.start then return "" end
  return text:sub(line.start, line.finish)
end

function Layout.page_lines(page)
  if type(page) == "table" and page.lines then return page.lines end
  return nil
end

function Layout.page_text(text, page)
  if type(page) == "table" and page.lines and #page.lines > 0 then
    local parts = {}
    for i = 1, #page.lines do
      parts[#parts + 1] = Layout.line_text(text, page.lines[i])
    end
    return table.concat(parts, "\n")
  end
  if type(page) == "table" and page.start then
    if page.finish < page.start then return "" end
    return text:sub(page.start, page.finish)
  end
  return ""
end


return Layout
