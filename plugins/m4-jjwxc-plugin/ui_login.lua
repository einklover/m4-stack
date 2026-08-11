-- 扫码登录界面: 二维码 + 状态 + 提示。
UiLogin = {}

function UiLogin.draw(state)
  local g = UiTemplate.header(state, "晋江文学 · 扫码登录", state.status_line)
  local m = g.m
  local w = g.w
  local msg = tostring(state.login_msg or "获取登录码...")
  local data = state.login_qr

  if data then
    -- 晋江扫码 URL 固定 93 字符 → 宿主自动选版恒为 V5 (37 模块, ECC_LOW)。
    local px = 8
    local modules = 37
    local size = px * modules
    local x = math.floor((w - size) / 2)
    local y = m.content_y + 20
    if type(gui.drawQR) == "function" then
      gui.drawQR(x, y, data, px)
    end
    gui.drawText(g.body_font, g.left, y + size + 24,
      "请用「晋江小说阅读」App 扫一扫")
    local status_y = y + size + 64
    if status_y + g.m.line > m.footer_top then
      status_y = math.max(m.content_y + 8, m.footer_top - g.m.line * 2)
    end
    gui.drawText(g.caption_font, g.left, status_y, msg)
  else
    gui.drawText(g.body_font, g.left, m.content_y + 30, "正在准备二维码…")
    gui.drawText(g.caption_font, g.left, m.content_y + 60, msg)
  end
  UiTemplate.footer(state, "返回", "", nil, nil)
  return g
end

return UiLogin
