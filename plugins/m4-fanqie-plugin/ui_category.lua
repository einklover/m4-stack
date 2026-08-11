UiCategory = {}

function UiCategory.page_count(cats, page_size)
  if #cats == 0 then return 1 end
  return math.floor((#cats + page_size - 1) / page_size)
end

function UiCategory.slice(cats, page, page_size)
  local start = (page - 1) * page_size + 1
  local t = {}
  for i = start, math.min(start + page_size - 1, #cats) do
    t[#t + 1] = cats[i]
  end
  return t, start
end

function UiCategory.draw(state)
  local g = UiTemplate.header(state, "分类浏览", state.status_line)
  local m = g.m
  local rows, start = UiCategory.slice(state.categories, state.category_page, state.CATEGORY_PAGE)
  local y = m.content_y
  local row_h = math.max(m.toc_row_h, m.line + 14)
  for i = 1, #rows do
    local c = rows[i]
    local group_name = tostring(c.group or "")
    local group = (group_name ~= "" and ("[" .. group_name .. "] ") or "")
    y = UiTemplate.row(state, y, row_h, tostring(c.title or "未分类"), group)
  end
  UiTemplate.footer(state, "返回", "下一页", state.category_page,
    UiCategory.page_count(state.categories, state.CATEGORY_PAGE))
end

return UiCategory
