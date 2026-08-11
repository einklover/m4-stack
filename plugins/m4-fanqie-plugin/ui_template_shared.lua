-- Shared M4 provider UI template. Geometry is owned by Layout so legacy Lua
-- screens and host-native list screens use the same safe area and touch band.
UiTemplate = {}

local function metrics(state)
  return (state and state.UI_METRICS) or Layout.metrics()
end

local function width()
  return (gui and gui.width and gui.width()) or 480
end

local function clip(text, max_w, font)
  text = tostring(text or "")
  if max_w and max_w > 0 and Layout and Layout.ellipsize then
    return Layout.ellipsize(text, max_w, font or 12)
  end
  return text
end

function UiTemplate.geometry(state)
  local m = metrics(state)
  local w = width()
  local gutter = 20
  local line = math.max(16, tonumber(m.line) or 24)
  return {
    m = m,
    w = w,
    left = gutter,
    right = w - gutter,
    title_font = 12,
    body_font = 12,
    sub_font = 10,
    caption_font = 10,
    header_line = (tonumber(m.content_y) or line * 4) - math.max(6, math.floor(line / 3)),
    footer_line = tonumber(m.content_bottom) or ((tonumber(m.footer_top) or 760) - 8),
    row_gap = math.max(2, math.floor(line / 8)),
  }
end

function UiTemplate.header(state, title, status)
  local g = UiTemplate.geometry(state)
  local m = g.m
  gui.drawText(g.title_font, g.left, m.title_y, clip(title, g.right - g.left, g.title_font))
  if status and tostring(status) ~= "" then
    gui.drawText(g.caption_font, g.left, m.status_y,
      clip(status, g.right - g.left, g.caption_font))
  end
  gui.drawLine(g.left, g.header_line, g.right, g.header_line)
  return g
end

function UiTemplate.footer(state, left_text, right_text, page, total)
  local g = UiTemplate.geometry(state)
  local m = g.m
  gui.drawLine(g.left, g.footer_line, g.right, g.footer_line)
  local rects = Layout.home_button_rects(m)
  local left = clip(left_text or "返回", math.floor(g.w * 0.30), g.caption_font)
  local right = clip(right_text or "下一页", math.floor(g.w * 0.30), g.caption_font)
  local function label_y(r)
    return Layout.center_text_y(r.y, r.h, g.caption_font, m.line)
  end
  if left ~= "" then gui.drawText(g.caption_font, rects[1].x, label_y(rects[1]), left) end
  local page_text = ""
  if page and total then page_text = tostring(page) .. "/" .. tostring(total) end
  if page_text ~= "" then
    local pw = gui.textWidth(g.caption_font, page_text)
    gui.drawText(g.caption_font, math.floor((g.w - pw) / 2), label_y(rects[1]), page_text)
  end
  if right ~= "" then
    local rw = gui.textWidth(g.caption_font, right)
    gui.drawText(g.caption_font, rects[2].x + rects[2].w - rw, label_y(rects[2]), right)
  end
  return g
end

function UiTemplate.row_height(state, row_h, subtitle)
  local m = metrics(state)
  local h = math.max(tonumber(row_h) or 0, tonumber(m.min_touch_h) or 48)
  if subtitle and tostring(subtitle) ~= "" then
    h = math.max(h, (tonumber(m.line) or 24) * 2 + 12)
  end
  return h
end

function UiTemplate.row(state, y, row_h, title, subtitle, selected)
  local g = UiTemplate.geometry(state)
  local m = g.m
  local effective_h = UiTemplate.row_height(state, row_h, subtitle)
  local title_x = g.left + (selected and 16 or 0)
  local max_w = g.right - title_x
  local marker = selected and "> " or ""
  gui.drawText(g.body_font, title_x, y,
    clip(marker .. tostring(title or ""), max_w, g.body_font))
  if subtitle and tostring(subtitle) ~= "" then
    gui.drawText(g.sub_font, title_x, y + m.line + 4,
      clip(subtitle, max_w, g.sub_font))
  end
  gui.drawLine(g.left, y + effective_h - g.row_gap, g.right, y + effective_h - g.row_gap)
  return y + effective_h
end

function UiTemplate.empty(state, text)
  local g = UiTemplate.geometry(state)
  gui.drawText(g.body_font, g.left, g.m.content_y + 8,
    clip(text or "暂无内容", g.right - g.left, g.body_font))
end

-- Safe-area page used by loading and error states. It wraps each UTF-8 line
-- with Layout.fit_line and stops before the shared footer divider.
function UiTemplate.page(state, title, status, body, hint)
  local g = UiTemplate.header(state, title or "", status)
  local m = g.m
  local y = m.content_y
  local step = (tonumber(m.line) or 24) + 8
  local bottom = (tonumber(m.content_bottom) or (tonumber(m.footer_top) or 760) - 8) - step
  local text = tostring(body or "")
  for line in string.gmatch(text .. "\n", "([^\n]*)\n") do
    if y > bottom then break end
    if line == "" then
      y = y + step
    else
      local pos = 1
      while pos <= #line and y <= bottom do
        local last = Layout.fit_line(line, pos, g.right - g.left, g.body_font, #line)
        if last < pos then last = pos end
        gui.drawText(g.body_font, g.left, y, line:sub(pos, last))
        y = y + step
        pos = last + 1
      end
    end
  end
  UiTemplate.footer(state, hint or "返回", "", nil, nil)
  return g
end

return UiTemplate
