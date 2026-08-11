UiReader = {}

function UiReader.draw(state)
  local g = UiTemplate.header(state, state.reader_title or "阅读", state.status_line)
  local m = g.m
  local w = g.w
  local margin = state.READER_MARGIN or 24
  local line_h = math.max(state.READER_LINE or 36, m.reader_line_h)
  local maxW = w - margin * 2

  local y = m.content_y
  local page_i = state.reader_page or 1
  local starts = state.reader_page_starts
  local path = state.reader_path
  local fsz = state.reader_file_size or 0

  if path and type(starts) == "table" and #starts > 0 then
    -- SD window mode: only this page's byte span (never whole-chapter readFile).
    local a = starts[page_i] or 0
    local b = starts[page_i + 1] or fsz
    if b < a then b = a end
    local chunk, perr = Layout.read_file_span(path, a, b)
    if not chunk then
      -- Oversized/corrupt page span: do not invent truncated text.
      gui.drawText(12, margin, y, perr == "page_too_large" and "[page rebuild needed]" or "[read error]")
      chunk = ""
    end
    local lines = Layout.wrap_page_chunk(chunk, margin, line_h, 12, nil)
    for i = 1, #lines do
      local line = Layout.line_text(chunk, lines[i])
      gui.drawText(12, margin, y, line)
      y = y + line_h
      if y + line_h > m.footer_y then break end
    end
  else
    -- Legacy in-memory page tables (tests / tiny chapters).
    local page = state.reader_pages and state.reader_pages[page_i]
    local lines = Layout.page_lines(page)
    if lines then
      for i = 1, #lines do
        local line = Layout.line_text(state.reader_text or "", lines[i])
        gui.drawText(12, margin, y, line)
        y = y + line_h
        if y + line_h > m.footer_y then break end
      end
    else
      local body = type(page) == "string" and page or ""
      for line in string.gmatch(body .. "\n", "([^\n]*)\n") do
        gui.drawText(12, margin, y, line)
        y = y + line_h
        if y + line_h > m.footer_y then break end
      end
    end
  end

  UiTemplate.footer(state, "上一页", "下一页", state.reader_page, state.reader_page_count)
  state._last_maxW = maxW
end

return UiReader
