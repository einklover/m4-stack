-- Major Arcana data and deterministic draw helpers. No host APIs required.
Tarot = Tarot or {}

Tarot.CARDS = {
  { id=0,  key="fool",       name="愚者",   upright="自由 · 开始 · 冒险",       reversed="冲动 · 逃避 · 失序" },
  { id=1,  key="magician",   name="魔术师", upright="行动 · 创造 · 掌控",       reversed="分心 · 炫技 · 迟疑" },
  { id=2,  key="priestess",  name="女祭司", upright="直觉 · 静观 · 秘密",       reversed="封闭 · 忽视直觉 · 疑惑" },
  { id=3,  key="empress",    name="皇后",   upright="丰盛 · 滋养 · 创造",       reversed="过度付出 · 停滞 · 依赖" },
  { id=4,  key="emperor",    name="皇帝",   upright="秩序 · 责任 · 边界",       reversed="僵化 · 控制 · 固执" },
  { id=5,  key="hierophant", name="教皇",   upright="传统 · 学习 · 指引",       reversed="质疑规则 · 独立 · 改革" },
  { id=6,  key="lovers",     name="恋人",   upright="选择 · 联结 · 真诚",       reversed="失衡 · 犹豫 · 价值冲突" },
  { id=7,  key="chariot",    name="战车",   upright="推进 · 意志 · 胜利",       reversed="失控 · 急躁 · 方向混乱" },
  { id=8,  key="strength",   name="力量",   upright="勇气 · 温柔 · 自制",       reversed="自我怀疑 · 消耗 · 逞强" },
  { id=9,  key="hermit",     name="隐者",   upright="独处 · 寻找 · 智慧",       reversed="孤立 · 退缩 · 迷失" },
  { id=10, key="wheel",      name="命运之轮", upright="转机 · 周期 · 机会",     reversed="延迟 · 反复 · 抗拒变化" },
  { id=11, key="justice",    name="正义",   upright="平衡 · 事实 · 责任",       reversed="偏见 · 逃避 · 不公平" },
  { id=12, key="hanged",     name="倒吊人", upright="暂停 · 换角度 · 放下",     reversed="拖延 · 卡住 · 白白牺牲" },
  { id=13, key="death",      name="死神",   upright="结束 · 转化 · 新生",       reversed="抗拒结束 · 停滞 · 执着" },
  { id=14, key="temperance", name="节制",   upright="调和 · 耐心 · 节奏",       reversed="过量 · 失衡 · 操之过急" },
  { id=15, key="devil",      name="恶魔",   upright="欲望 · 束缚 · 执念",       reversed="看见束缚 · 松绑 · 戒断" },
  { id=16, key="tower",      name="高塔",   upright="突变 · 真相 · 打破",       reversed="延迟爆发 · 抗拒改变 · 不安" },
  { id=17, key="star",       name="星星",   upright="希望 · 疗愈 · 灵感",       reversed="失望 · 缺乏信心 · 枯竭" },
  { id=18, key="moon",       name="月亮",   upright="潜意识 · 模糊 · 感受",     reversed="迷雾消散 · 焦虑 · 误判" },
  { id=19, key="sun",        name="太阳",   upright="清晰 · 活力 · 喜悦",       reversed="暂时低落 · 过度乐观 · 延迟" },
  { id=20, key="judgement",  name="审判",   upright="觉醒 · 回应 · 复盘",       reversed="自我怀疑 · 逃避召唤 · 后悔" },
  { id=21, key="world",      name="世界",   upright="完成 · 整合 · 抵达",       reversed="未完成 · 缺口 · 收尾延迟" },
}

function Tarot.count()
  return #Tarot.CARDS
end

function Tarot.card(index)
  return Tarot.CARDS[index]
end

local function default_rng()
  return math.random()
end

function Tarot.draw(count, rng)
  rng = rng or default_rng
  count = math.max(1, math.min(count or 1, #Tarot.CARDS))
  local pool = {}
  for i = 1, #Tarot.CARDS do pool[i] = i end
  local out = {}
  for i = 1, count do
    local r = rng()
    if type(r) ~= "number" then r = 0.5 end
    if r < 0 then r = 0 elseif r >= 1 then r = 0.999999 end
    local slot = i + math.floor(r * (#pool - i + 1))
    pool[i], pool[slot] = pool[slot], pool[i]
    local orient = rng()
    out[i] = { index = pool[i], reversed = type(orient) == "number" and orient < 0.35 or false }
  end
  return out
end

function Tarot.daily(day)
  day = math.floor(tonumber(day) or 0)
  if day < 0 then day = -day end
  local mix = (day * 1103515245 + 12345) % 2147483647
  local index = (mix % #Tarot.CARDS) + 1
  local reversed = (math.floor(mix / #Tarot.CARDS) % 5) == 0
  return { index = index, reversed = reversed }
end

function Tarot.meaning(reading)
  if not reading then return "" end
  local c = Tarot.CARDS[reading.index]
  if not c then return "" end
  return reading.reversed and c.reversed or c.upright
end

function Tarot.serialize(kind, readings, selected)
  local parts = { tostring(kind or ""), tostring(selected or 1) }
  for i = 1, #(readings or {}) do
    local r = readings[i]
    parts[#parts+1] = tostring(r.index) .. ":" .. (r.reversed and "1" or "0")
  end
  return table.concat(parts, ",")
end

function Tarot.deserialize(s)
  if type(s) ~= "string" or s == "" then return nil end
  local fields = {}
  for token in string.gmatch(s, "[^,]+") do fields[#fields+1] = token end
  if #fields < 3 then return nil end
  local out = { kind = fields[1], selected = tonumber(fields[2]) or 1, readings = {} }
  for i = 3, #fields do
    local idx, rev = fields[i]:match("^(%d+):([01])$")
    idx = tonumber(idx)
    if not idx or not Tarot.CARDS[idx] then return nil end
    out.readings[#out.readings+1] = { index = idx, reversed = rev == "1" }
  end
  if #out.readings == 0 then return nil end
  return out
end
