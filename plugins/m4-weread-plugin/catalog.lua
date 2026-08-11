-- FileRows catalog adapter shared by the provider and the UI.
-- A catalog is a bounded row source; Lua must never materialize all UIDs.
Catalog = {}

local function as_string(v)
  if v == nil then return "" end
  return tostring(v)
end

function Catalog.spec(source, count, uid_field, title_field, word_count_field)
  return {
    source = as_string(source),
    count = math.max(0, math.floor(tonumber(count) or 0)),
    uid_field = math.max(0, math.floor(tonumber(uid_field) or 0)),
    title_field = math.max(0, math.floor(tonumber(title_field) or 1)),
    -- -1 means that the row source does not carry a word-count marker.  This
    -- distinction matters: old two-column TOC files must not make every row
    -- look like an empty parent heading.
    word_count_field = math.floor(tonumber(word_count_field) or -1),
  }
end

function Catalog.normalize(spec)
  if type(spec) ~= "table" then return nil end
  return Catalog.spec(
    spec.source or spec.fileRelPath or spec.path,
    spec.count or spec.chapterCount,
    spec.uid_field or spec.uidField or spec.uidField0,
    spec.title_field or spec.titleField or spec.titleField0,
    spec.word_count_field or spec.wordCountField or spec.wordCountField0)
end

function Catalog.provider_spec(spec)
  local c = Catalog.normalize(spec)
  if not c or c.source == "" or c.count < 1 then return nil end
  return {
    kind = "file",
    path = c.source,
    count = c.count,
    uidField = c.uid_field,
    titleField = c.title_field,
    wordCountField = c.word_count_field,
  }
end

local function split_fields(line)
  local fields = {}
  for field in string.gmatch(as_string(line) .. "\t", "([^\t]*)\t") do
    fields[#fields + 1] = field
  end
  return fields
end

local function row_value(row, index0)
  if type(row) ~= "table" then return nil end
  local fields = row.fields or row.values or row
  if type(fields) ~= "table" then return nil end
  return fields[index0 + 1] or fields[index0]
end

function Catalog.parse_row(row, spec)
  local c = Catalog.normalize(spec)
  if not c then return nil, "catalog" end
  local uid, title
  local word_count = nil
  if type(row) == "string" then
    local fields = split_fields(row)
    uid = fields[c.uid_field + 1]
    title = fields[c.title_field + 1]
    if c.word_count_field >= 0 then
      local raw = fields[c.word_count_field + 1]
      if raw ~= nil and raw ~= "" then word_count = tonumber(raw) end
    end
  elseif type(row) == "table" then
    uid = row.uid or row.chapterUid or row.id or row.itemId
    title = row.title or row.name
    if uid == nil then uid = row_value(row, c.uid_field) end
    if title == nil then title = row_value(row, c.title_field) end
    if row.wordCount ~= nil or row.word_count ~= nil then
      word_count = tonumber(row.wordCount or row.word_count)
    elseif c.word_count_field >= 0 then
      word_count = tonumber(row_value(row, c.word_count_field))
    end
  end
  uid = as_string(uid)
  title = as_string(title)
  if uid == "" then return nil, "empty_uid" end
  local out = { chapterUid = uid, title = title, uid = uid }
  if word_count ~= nil then out.wordCount = word_count end
  return out
end

function Catalog.read_row(spec, index0, original_work)
  local c = Catalog.normalize(spec)
  local i = math.floor(tonumber(index0) or -1)
  if not c or c.source == "" or i < 0 or i >= c.count then return nil, "row_range" end
  if type(provider) ~= "table" or type(provider.resolveCatalogWork) ~= "function" then
    return nil, "no_catalog_work_resolver"
  end
  local request = original_work or { type = "resolve", index = i, catalog = c }
  request.index = i
  request.catalog = c
  local row, err = provider.resolveCatalogWork(request)
  if not row then return nil, err or "row" end
  if type(row) == "table" and row.ok == false then
    return nil, tostring(row.error or err or "row")
  end
  if type(row) == "table" and type(row.row) == "table" then row = row.row end
  return Catalog.parse_row(row, c)
end

function Catalog.virtual_rows(spec, provider_id, book_id)
  local c = Catalog.normalize(spec)
  if not c or c.source == "" or c.count < 1 then return {} end
  provider_id = as_string(provider_id or c.providerId or c.provider_id)
  book_id = as_string(book_id or c.bookId or c.book_id)
  local rows = { __catalog = c }
  return setmetatable(rows, {
    __len = function() return c.count end,
    __index = function(_, key)
      if type(key) == "number" and key >= 1 and key <= c.count then
        return Catalog.read_row(c, key - 1, {
          type = "resolve",
          providerId = provider_id,
          bookId = book_id,
          index = key - 1,
          catalog = c,
        })
      end
      return nil
    end,
  })
end

function Catalog.resolve_work(work)
  if type(work) ~= "table" then return nil, nil, "work" end
  local uid = as_string(work.chapterUid)
  local title = as_string(work.title)
  local needs = work.needsResolve == true or work.needs_resolve == true
  local spec = work.catalog or work.catalogSpec or work.catalog_spec
  if needs or uid == "" then
    local row, err = Catalog.read_row(spec, tonumber(work.index) or -1, work)
    if not row then return nil, nil, err or "resolve" end
    uid, title = row.chapterUid, row.title
  end
  if uid == "" then return nil, nil, "empty_uid" end
  if title == "" then title = uid end
  return uid, title, nil
end
