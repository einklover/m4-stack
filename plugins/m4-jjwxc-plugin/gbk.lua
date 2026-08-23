-- GB18030 (双字节区) → UTF-8 转换。仅 WAP VIP 章正文需要, 按需加载后释放。
-- 表: gbk_table.bin, 126 lead x 190 trail x 2B (uint16 BE), 47880 字节。
-- trail_ord = trail < 0x80 ? trail - 0x40 : trail - 0x41
--
-- 内存要点: 绝不能按「一字一 string」拼表 (万级小串会直接 lua_mem_limit)。
-- 用 rope 缓冲 (每 ≥512B 合并一段), 峰值约 输出正文 + 表 48KB。
Gbk = {}

local raw = nil
local ROPE_FLUSH = 512

function Gbk.load()
  if raw then return true end
  if type(fs) ~= "table" or type(fs.readAppFile) ~= "function" then
    return false
  end
  local ok, data = pcall(fs.readAppFile, "gbk_table.bin")
  if not ok or type(data) ~= "string" or #data < 47880 then
    return false
  end
  raw = data
  return true
end

function Gbk.unload()
  raw = nil
end

local function utf8_bytes(cp)
  if cp == 0 then
    return "?"
  elseif cp < 0x80 then
    return string.char(cp)
  elseif cp < 0x800 then
    return string.char(0xC0 + math.floor(cp / 0x40), 0x80 + (cp % 0x40))
  else
    return string.char(0xE0 + math.floor(cp / 0x1000),
      0x80 + math.floor(cp / 0x40) % 0x40, 0x80 + (cp % 0x40))
  end
end

-- 低分配转换: 小片段先塞 buf, 满 ROPE_FLUSH 再 table.concat 进 pieces。
local function convert_bytes(s, i0, i1)
  local pieces = {}
  local buf = {}
  local bn = 0
  local function flush()
    if #buf == 0 then return end
    pieces[#pieces + 1] = table.concat(buf)
    buf = {}
    bn = 0
  end
  local function emit(str)
    buf[#buf + 1] = str
    bn = bn + #str
    if bn >= ROPE_FLUSH then flush() end
  end

  local i = i0 or 1
  local n = i1 or #s
  while i <= n do
    local b = s:byte(i)
    if not b then break end
    if b < 0x80 then
      emit(s:sub(i, i))
      i = i + 1
    elseif b >= 0x81 and b <= 0xFE then
      local b2 = s:byte(i + 1)
      if b2 and b2 >= 0x30 and b2 <= 0x39 then
        -- GB18030 四字节序列: 罕见生僻字 → ?
        i = i + 4
        emit("?")
      elseif b2 and ((b2 >= 0x40 and b2 <= 0x7E) or (b2 >= 0x80 and b2 <= 0xFE)) then
        local tord = b2 < 0x80 and (b2 - 0x40) or (b2 - 0x41)
        local off = ((b - 0x81) * 190 + tord) * 2
        local cp = (raw:byte(off + 1) * 256) + raw:byte(off + 2)
        emit(utf8_bytes(cp))
        i = i + 2
      else
        emit("?")
        i = i + 1
      end
    else
      emit("?")
      i = i + 1
    end
  end
  flush()
  if #pieces == 0 then return "" end
  if #pieces == 1 then return pieces[1] end
  local out = table.concat(pieces)
  pieces = nil
  return out
end

-- 输入 GB18030 字节串, 输出 UTF-8 字符串 (nil, err 表示表缺失)。
function Gbk.convert(s)
  if type(s) ~= "string" or s == "" then return "", nil end
  if not Gbk.load() then return nil, "no_gbk_table" end
  return convert_bytes(s, 1, #s), nil
end

-- 从 SD 文件一段 GBK 转 UTF-8 写盘 (按窗读取, 输入不全进堆)。
function Gbk.convert_range_to_file(inPath, offset, length, outPath)
  if type(inPath) ~= "string" or type(outPath) ~= "string" then
    return nil, "bad_path"
  end
  offset = math.max(0, math.floor(tonumber(offset) or 0))
  length = math.max(0, math.floor(tonumber(length) or 0))
  if length < 1 then return nil, "empty_range" end
  if type(fs) ~= "table" or type(fs.readRange) ~= "function" then
    return nil, "no_readRange"
  end
  if not Gbk.load() then return nil, "no_gbk_table" end

  local WIN = 4096
  local pieces = {}
  local done = 0
  local carry = ""
  while done < length do
    local want = math.min(WIN, length - done)
    local chunk = fs.readRange(inPath, offset + done, want)
    if type(chunk) ~= "string" or chunk == "" then break end
    done = done + #chunk
    local data = carry .. chunk
    carry = ""
    chunk = nil
    -- 末尾可能是双字节 lead: 留 1 字节到下一窗
    if done < length and #data > 0 then
      local last = data:byte(#data)
      if last and last >= 0x81 and last <= 0xFE then
        carry = data:sub(#data)
        data = data:sub(1, #data - 1)
      end
    end
    if #data > 0 then
      local part = convert_bytes(data, 1, #data)
      if part and part ~= "" then pieces[#pieces + 1] = part end
    end
    data = nil
    if collectgarbage and (#pieces % 8 == 0) then collectgarbage("step", 200) end
  end
  if carry ~= "" then
    local part = convert_bytes(carry, 1, #carry)
    if part and part ~= "" then pieces[#pieces + 1] = part end
    carry = nil
  end
  local body = table.concat(pieces)
  pieces = nil
  if collectgarbage then collectgarbage("collect") end
  local okw = false
  if type(fs.replaceFile) == "function" then
    okw = fs.replaceFile(outPath, body)
  elseif type(fs.writeFile) == "function" then
    okw = fs.writeFile(outPath, body)
  end
  local n = body and #body or 0
  body = nil
  if collectgarbage then collectgarbage("collect") end
  if not okw or n < 1 then return nil, "write_failed" end
  return n, nil
end

return Gbk
