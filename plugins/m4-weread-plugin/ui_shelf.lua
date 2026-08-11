UiShelf = {}

function UiShelf.page_count(books, page_size)
  if #books == 0 then return 1 end
  return math.floor((#books + page_size - 1) / page_size)
end

function UiShelf.slice(books, page, page_size)
  local start = (page - 1) * page_size + 1
  local t = {}
  for i = start, math.min(start + page_size - 1, #books) do
    t[#t + 1] = books[i]
  end
  return t, start
end

function UiShelf.draw(state)
  local g = UiTemplate.header(state, "微信读书", state.status_line)
  local m = g.m
  local rows = UiShelf.slice(state.books, state.shelf_page, state.PAGE_SIZE)
  local y = m.content_y
  local row_h = math.max(m.shelf_row_h, m.line * 2 + 12)
  for i = 1, #rows do
    local b = rows[i]
    local meta = (b.author ~= "" and (b.author .. " · ") or "") .. tostring(b.progress or 0) .. "%"
    y = UiTemplate.row(state, y, row_h, b.title, meta)
  end
  if #state.books == 0 then UiTemplate.empty(state, "书架为空") end
  UiTemplate.footer(state, "返回", "下一页", state.shelf_page,
    UiShelf.page_count(state.books, state.PAGE_SIZE))
end

return UiShelf
