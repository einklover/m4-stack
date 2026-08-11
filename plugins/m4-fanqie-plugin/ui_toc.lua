UiToc = {}

function UiToc.page_count(chapters, page_size)
  if #chapters == 0 then return 1 end
  return math.floor((#chapters + page_size - 1) / page_size)
end

function UiToc.slice(chapters, page, page_size)
  local start = (page - 1) * page_size + 1
  local t = {}
  for i = start, math.min(start + page_size - 1, #chapters) do
    t[#t + 1] = chapters[i]
  end
  return t, start
end

function UiToc.draw(state)
  local title = state.cur_book and state.cur_book.title or "目录"
  local g = UiTemplate.header(state, title, state.status_line)
  local m = g.m
  local rows, start = UiToc.slice(state.chapters, state.toc_page, state.TOC_PAGE)
  local y = m.content_y
  local row_h = math.max(m.toc_row_h, m.line + 14)
  for i = 1, #rows do
    local ch = rows[i]
    local selected = start + i - 1 == state.chapter_idx
    y = UiTemplate.row(state, y, row_h, ch.title, selected and "当前章节" or nil, selected)
  end
  UiTemplate.footer(state, "返回", "下一页", state.toc_page,
    UiToc.page_count(state.chapters, state.TOC_PAGE))
end

return UiToc
