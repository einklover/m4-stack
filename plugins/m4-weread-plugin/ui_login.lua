UiLogin = {}

function UiLogin.draw(state)
  local w = gui.width()
  local g = UiTemplate.header(state, "微信读书 · 扫码登录", Auth.login_msg or "")
  local m = g.m
  local buttons = Layout.home_button_rects(m)
  local button_bottom = buttons[1].y
  if Auth.login_uid then
    local url = "https://weread.qq.com/web/confirm?uid=" .. Auth.login_uid
    local px = 5
    local qs = tonumber(gui.qrSize(px)) or (33 * px)
    local qr_y = m.content_y
    -- Use the host-reported QR size, not a hard-coded 33*px guess. Keep both
    -- prompts in the content safe area and leave the footer touch band clear.
    local prompt_gap = 12
    local prompt2_gap = m.line + 8
    local prompt1_y = qr_y + qs + prompt_gap
    local prompt2_y = prompt1_y + prompt2_gap
    if prompt2_y + m.line > button_bottom - 8 then
      qr_y = math.max(m.content_y, button_bottom - 8 - m.line - prompt2_gap - prompt_gap - qs)
      prompt1_y = qr_y + qs + prompt_gap
      prompt2_y = prompt1_y + prompt2_gap
    end
    gui.drawQR(math.floor((w - qs) / 2), qr_y, url, px)
    gui.drawText(10, 24, prompt1_y, "自动检查中 · 点按二维码立即刷新")
    gui.drawText(10, 24, prompt2_y, "返回键取消并退出")
  else
    gui.drawText(10, 24, m.content_y, "连接失败，请选择操作")
  end
  for i = 1, #buttons do
    local r = buttons[i]
    gui.drawRect(r.x, r.y, r.w, r.h)
  end
  gui.drawText(10, buttons[1].x, Layout.center_text_y(buttons[1].y, buttons[1].h, 10, m.line), "返回")
  gui.drawText(10, buttons[2].x, Layout.center_text_y(buttons[2].y, buttons[2].h, 10, m.line), "重试")
end

return UiLogin
