UiBooklist = {}

function UiBooklist.page_count(books, page_size)
  if #books == 0 then return 1 end
  return math.floor((#books + page_size - 1) / page_size)
end

function UiBooklist.slice(books, page, page_size)
  local start = (page - 1) * page_size + 1
  local t = {}
  for i = start, math.min(start + page_size - 1, #books) do
    t[#t + 1] = books[i]
  end
  return t, start
end

function UiBooklist.draw(state)
  local g = UiTemplate.header(state,
    (state.cur_category and state.cur_category.title) or "书单", state.status_line)
  local m = g.m
  local rows, start = UiBooklist.slice(state.booklist, state.booklist_page, state.BOOKLIST_PAGE)
  local y = m.content_y
  local row_h = math.max(m.shelf_row_h, m.line * 2 + 12)
  for i = 1, #rows do
    local b = rows[i]
    local author = tostring(b.author or "")
    local meta = author ~= "" and author or "佚名"
    y = UiTemplate.row(state, y, row_h, tostring(b.title or b.bookId or "无标题"), meta)
  end
  if #state.booklist == 0 then UiTemplate.empty(state, "暂无书籍") end
  UiTemplate.footer(state, "返回", "下一页", state.booklist_page,
    UiBooklist.page_count(state.booklist, state.BOOKLIST_PAGE))
end

return UiBooklist
