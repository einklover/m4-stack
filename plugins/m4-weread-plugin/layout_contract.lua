-- Single provider UI/layout contract. layout.lua keeps only pagination and
-- loads this file so WeRead and Fanqie cannot drift in geometry or hit testing.

function Layout.line_height(font, fallback)
  fallback = fallback or 24
  if type(gui.lineHeight) == "function" then
    local h = tonumber(gui.lineHeight(font or 12))
    if h and h > 0 then return h end
  end
  return fallback
end

function Layout.metrics()
  local w = gui.width()
  local h = gui.height()
  local line = Layout.line_height(12, 24)
  local title_y = 24
  local status_y = title_y + line + 8
  local content_y = status_y + line + 12
  local min_touch_h = 48
  local footer_top = math.max(content_y + line, h - min_touch_h)
  if footer_top + min_touch_h > h then footer_top = math.max(0, h - min_touch_h) end
  local footer_y = footer_top + math.max(0, math.floor((min_touch_h - line) / 2))
  local content_bottom = footer_top - 8
  local shelf_row_h = math.max(72, min_touch_h, line * 2 + 12)
  local toc_row_h = math.max(min_touch_h, line + 14, line * 2 + 24)
  local usable = math.max(line, footer_top - content_y)
  return {
    width = w,
    height = h,
    line = line,
    title_y = title_y,
    status_y = status_y,
    content_y = content_y,
    content_bottom = content_bottom,
    footer_top = footer_top,
    footer_y = footer_y,
    shelf_row_h = shelf_row_h,
    toc_row_h = toc_row_h,
    min_touch_h = min_touch_h,
    reader_line_h = line + 8,
    shelf_page_size = math.max(1, math.min(8, math.floor(usable / shelf_row_h))),
    toc_page_size = math.max(1, math.min(10, math.floor(usable / toc_row_h))),
  }
end

function Layout.center_text_y(y, height, font, fallback)
  local text_h = Layout.line_height(font or 10, fallback or 24)
  return math.floor(y + math.max(0, (height - text_h) / 2))
end

function Layout.home_button_rects(metrics)
  metrics = metrics or Layout.metrics()
  local w = tonumber(metrics.width) or gui.width()
  local h = tonumber(metrics.height) or gui.height()
  local min_touch = math.max(48, tonumber(metrics.min_touch_h) or 48)
  -- Keep a real center slot for the page indicator.  An 8px gap leaves the
  -- indicator drawn over both button borders on the 480px M4 screen.
  local gap, side = 56, 20
  local button_w = math.floor((w - side * 2 - gap) / 2)
  if button_w < 1 then button_w = 1 end
  local y = tonumber(metrics.footer_top) or (h - min_touch)
  if y < 0 then y = 0 end
  if y + min_touch > h then y = math.max(0, h - min_touch) end
  local button_h = math.max(1, h - y)
  return {
    { x = side, y = y, w = button_w, h = button_h },
    { x = w - side - button_w, y = y, w = button_w, h = button_h },
  }
end

function Layout.button_index_from_point(x, y, rects)
  if type(rects) ~= "table" then return nil end
  x, y = tonumber(x), tonumber(y)
  if not x or not y then return nil end
  for i = 1, #rects do
    local r = rects[i]
    if type(r) == "table" and x >= r.x and x < r.x + r.w
        and y >= r.y and y < r.y + r.h then return i end
  end
  return nil
end

function Layout.home_footer_hit(y, metrics)
  metrics = metrics or Layout.metrics()
  y = tonumber(y)
  local h = tonumber(metrics.height) or gui.height()
  local top = tonumber(metrics.footer_top) or (h - (tonumber(metrics.min_touch_h) or 48))
  return y ~= nil and y >= top and y < h
end

function Layout.home_row_from_point(y, metrics, count, row_h)
  metrics = metrics or Layout.metrics()
  y = tonumber(y)
  count = math.floor(tonumber(count) or 0)
  row_h = tonumber(row_h) or tonumber(metrics.shelf_row_h) or 48
  local top = tonumber(metrics.content_y) or 0
  local footer = tonumber(metrics.footer_top) or (tonumber(metrics.height) or gui.height())
  if not y or count < 1 or row_h < 1 or y < top or y >= footer then return nil end
  local idx = math.floor((y - top) / row_h) + 1
  if idx < 1 or idx > count then return nil end
  return idx
end

local function utf8_next(s, i)
  if i > #s then return nil end
  local c = s:byte(i)
  if not c then return nil end
  local len = 1
  if c >= 0xF0 then len = 4
  elseif c >= 0xE0 then len = 3
  elseif c >= 0xC0 then len = 2 end
  if i + len - 1 > #s then len = 1 end
  return i + len - 1
end

local function text_width_range(font, s, from, to)
  if to < from then return 0 end
  return gui.textWidth(font, s:sub(from, to))
end

function Layout.fit_line(s, from, maxW, font, hard_end)
  font = font or 12
  local n = #s
  if from > n then return from - 1 end
  if hard_end == nil or hard_end > n then hard_end = n end
  if hard_end < from then return from - 1 end
  local max_bytes = math.max(4, Layout.FIT_MAX_BYTES or 1024)
  local probe_end = math.min(from + max_bytes - 1, hard_end)
  if maxW < 1 then
    local e = utf8_next(s, from)
    return (e and e <= probe_end) and e or from
  end
  local i, best = from, from - 1
  while i <= probe_end do
    local e = utf8_next(s, i)
    if not e then break end
    if e > probe_end then
      if best < from then best = from end
      break
    end
    if text_width_range(font, s, from, e) <= maxW then
      best, i = e, e + 1
    else
      break
    end
  end
  if best < from then
    local e = utf8_next(s, from)
    best = (e and e <= probe_end) and e or from
  end
  return math.max(from, math.min(best, hard_end))
end

function Layout.ellipsize(s, maxW, font)
  s = tostring(s or "")
  font = font or 12
  maxW = tonumber(maxW) or 0
  if maxW <= 0 or s == "" then return maxW > 0 and s or "" end
  local max_bytes = Layout.FIT_MAX_BYTES or 1024
  if #s <= max_bytes and gui.textWidth(font, s) <= maxW then return s end
  local ell = "…"
  if gui.textWidth(font, ell) > maxW then return "" end
  local best_end, i, limit = 0, 1, math.min(#s, max_bytes)
  while i <= limit do
    local e = utf8_next(s, i)
    if not e or e > limit then break end
    if gui.textWidth(font, s:sub(1, e) .. ell) > maxW then break end
    best_end, i = e, e + 1
  end
  return s:sub(1, best_end) .. ell
end

return Layout
