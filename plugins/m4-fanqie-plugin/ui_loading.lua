-- ui_loading: shared loading progress helpers for M4 plugins.
--
-- Host progressive download is `loader` (firmware):
--   loader.chapter(spec) / loader.toc(spec)  — arm job (no long block)
--   loader.pump({ms=, bytes=})               — cooperative slice
--   loader.status() → { phase, bytes, rows, early, done, error, path }
--   loader.cancel()
-- phase: idle|cache|connecting|streaming|early|done|failed
--
-- Plugins: paint-first (ui_committed) → pump/status each draw → show
-- spinner + elapsed + bytes so e-ink is never a static blank.

UiLoading = {}

local SPINNER = { "·", "··", "···", "····" }

function UiLoading.fmt_bytes(n)
  n = tonumber(n) or 0
  if n < 1 then return "0B" end
  if n < 1024 then return tostring(math.floor(n)) .. "B" end
  if n < 1024 * 1024 then
    local kb = n / 1024
    if kb < 10 then return string.format("%.1fKB", kb) end
    return string.format("%dKB", math.floor(kb + 0.5))
  end
  return string.format("%.1fMB", n / (1024 * 1024))
end

function UiLoading.elapsed_s(t0)
  t0 = tonumber(t0) or 0
  if t0 <= 0 or type(sys) ~= "table" or type(sys.millis) ~= "function" then return 0 end
  local d = sys.millis() - t0
  if d < 0 then d = 0 end
  return math.floor(d / 1000)
end

function UiLoading.spinner(sec)
  sec = math.max(0, tonumber(sec) or 0)
  return SPINNER[(sec % #SPINNER) + 1]
end

function UiLoading.loader_phase_label(phase, font_ok)
  phase = tostring(phase or "")
  if phase == "connecting" then
    return font_ok and "连接服务器 (TLS)…" or "TLS connect…"
  elseif phase == "streaming" then
    return font_ok and "流式下载…" or "streaming…"
  elseif phase == "early" then
    return font_ok and "首屏已开 · 后台续传…" or "early open · background…"
  elseif phase == "cache" then
    return font_ok and "缓存命中…" or "cache hit…"
  elseif phase == "done" then
    return font_ok and "完成" or "done"
  elseif phase == "failed" then
    return font_ok and "失败" or "failed"
  end
  return ""
end

-- Build multi-line body for a network/chapter job table.
-- job fields: label, body, t0, bytes, count, phase, sys_loader, loader_phase
function UiLoading.job_body(job, font_ok)
  if type(job) ~= "table" then
    return font_ok and "加载中…" or "loading…"
  end
  local sec = UiLoading.elapsed_s(job.t0)
  local spin = UiLoading.spinner(sec)
  local lines = {}
  local head = tostring(job.body or job.label or "")
  if head ~= "" then
    lines[#lines + 1] = spin .. " " .. head
  else
    lines[#lines + 1] = spin .. " " .. (font_ok and "加载中…" or "loading…")
  end
  local lp = UiLoading.loader_phase_label(job.loader_phase or job.phase, font_ok)
  if lp ~= "" then
    lines[#lines + 1] = lp
  end
  if job.sys_loader == true then
    lines[#lines + 1] = font_ok and "模式: 宿主流式 loader" or "mode: host loader"
  elseif job.phase == "download" or job.phase == "work" then
    lines[#lines + 1] = font_ok and "模式: 联网请求" or "mode: network"
  end
  if (job.count or 0) > 0 then
    lines[#lines + 1] = (font_ok and "条目 " or "items ") .. tostring(job.count)
  end
  if (job.bytes or 0) > 0 then
    lines[#lines + 1] = (font_ok and "已接收 " or "recv ") .. UiLoading.fmt_bytes(job.bytes)
  else
    lines[#lines + 1] = font_ok and "等待数据…" or "waiting data…"
  end
  lines[#lines + 1] = (font_ok and "已用时 " or "elapsed ") .. tostring(sec) .. "s"
  return table.concat(lines, "\n")
end

function UiLoading.job_status(job, font_ok)
  if type(job) ~= "table" then return font_ok and "加载中…" or "loading…" end
  local sec = UiLoading.elapsed_s(job.t0)
  local spin = UiLoading.spinner(sec)
  local base = tostring(job.label or (font_ok and "加载中…" or "loading…"))
  local lp = UiLoading.loader_phase_label(job.loader_phase, font_ok)
  if lp ~= "" then base = lp end
  local bits = { spin .. " " .. base }
  if (job.count or 0) > 0 then
    bits[#bits + 1] = tostring(job.count) .. (font_ok and "条" or "")
  end
  if (job.bytes or 0) > 0 then
    bits[#bits + 1] = UiLoading.fmt_bytes(job.bytes)
  end
  if sec >= 1 then
    bits[#bits + 1] = tostring(sec) .. "s"
  end
  return table.concat(bits, " · ")
end

return UiLoading
