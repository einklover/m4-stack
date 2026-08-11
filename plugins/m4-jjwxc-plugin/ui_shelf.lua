-- Shared provider template; implementation is kept in one package source.
if type(sys) == "table" and type(sys.load) == "function" then
  sys.load("ui_template_shared.lua")
else
  dofile("test/fixtures/m4x/jjwxc_src/ui_template_shared.lua")
end
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

-- 行: [1]分类浏览 [2]扫码登录/退出 [3]今日限免 [4+]书籍
-- 固定行 + 书行不能超出 footer 区域: 超出即停止绘制, 避免元素堆积/重叠。
function UiShelf.draw(state)
  local g = UiTemplate.header(state, "晋江文学", state.status_line)
  local m = g.m
  local y = m.content_y
  local row_h = math.max(m.shelf_row_h, m.line * 2 + 12)
  local function row_fits()
    return y + row_h <= m.footer_top
  end
  if row_fits() then
    y = UiTemplate.row(state, y, row_h, "分类浏览", "按分类查找书籍")
  end
  if row_fits() then
    if state.auth_has then
      y = UiTemplate.row(state, y, row_h, "扫码登录", "已登录 · 点按退出")
    else
      y = UiTemplate.row(state, y, row_h, "扫码登录", "晋江App扫一扫 · 支持VIP章")
    end
  end
  if row_fits() then
    y = UiTemplate.row(state, y, row_h, "今日限免", "每日免费书单")
  end
  local rows = UiShelf.slice(state.books, state.shelf_page, state.PAGE_SIZE)
  for i = 1, #rows do
    if not row_fits() then break end
    local b = rows[i]
    local author = tostring(b.author or "")
    local meta = (author ~= "" and (author .. " · ") or "") .. tostring(b.progress or 0) .. "%"
    y = UiTemplate.row(state, y, row_h, tostring(b.title or b.bookId or "无标题"), meta)
  end
  if #state.books == 0 then UiTemplate.empty(state, "书架为空，请从分类浏览找书") end
  UiTemplate.footer(state, "返回", "下一页", state.shelf_page,
    UiShelf.page_count(state.books, state.PAGE_SIZE))
end

return UiShelf
