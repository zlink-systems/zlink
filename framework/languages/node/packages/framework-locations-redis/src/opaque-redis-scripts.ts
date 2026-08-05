const PROLOGUE = `
if redis.replicate_commands then redis.replicate_commands() end
local time = redis.call('TIME')
local nowMs = tonumber(time[1]) * 1000 + math.floor(tonumber(time[2]) / 1000)
`;

export const OPAQUE_READ_SCRIPT = PROLOGUE + `
local row = redis.call('HMGET', KEYS[1], 'key', 'bytes', 'version', 'expiresAtMs')
if not row[1] or row[1] ~= ARGV[1] then
    return {0, nowMs}
end
local expiresAtMs = tonumber(row[4] or '0')
if expiresAtMs > 0 and expiresAtMs <= nowMs then
    redis.call('DEL', KEYS[1])
    redis.call('HDEL', KEYS[2], ARGV[1])
    return {0, nowMs}
end
return {1, nowMs, row[2], row[3], row[4] or ''}
`;

export const OPAQUE_WRITE_SCRIPT = PROLOGUE + `
local conditions = cjson.decode(ARGV[1])
local mutations = cjson.decode(ARGV[2])

local function liveVersion(index, logicalKey)
    local row = redis.call('HMGET', KEYS[index], 'key', 'version', 'expiresAtMs')
    if not row[1] or row[1] ~= logicalKey then return nil end
    local expiresAtMs = tonumber(row[3] or '0')
    if expiresAtMs > 0 and expiresAtMs <= nowMs then
        redis.call('DEL', KEYS[index])
        redis.call('HDEL', KEYS[1], logicalKey)
        return nil
    end
    return row[2]
end

for _, condition in ipairs(conditions) do
    local current = liveVersion(condition[2], condition[3])
    if condition[1] == 'missing' then
        if current then return {'conflict', nowMs} end
    elseif not current or current ~= condition[4] then
        return {'conflict', nowMs}
    end
end

local result = {'applied', nowMs}
for _, mutation in ipairs(mutations) do
    local rowKey = KEYS[mutation[2]]
    local logicalKey = mutation[3]
    if mutation[1] == 'delete' then
        redis.call('DEL', rowKey)
        redis.call('HDEL', KEYS[1], logicalKey)
    else
        local version = tostring(redis.call('INCR', KEYS[2]))
        local expiresAtMs = 0
        if mutation[5] ~= false then expiresAtMs = nowMs + tonumber(mutation[5]) end
        redis.call(
            'HSET', rowKey,
            'key', logicalKey,
            'bytes', mutation[4],
            'version', version,
            'expiresAtMs', tostring(expiresAtMs))
        if expiresAtMs > 0 then
            redis.call('PEXPIREAT', rowKey, expiresAtMs)
        else
            redis.call('PERSIST', rowKey)
        end
        redis.call(
            'HSET', KEYS[1], logicalKey,
            cjson.encode({mutation[4], version, expiresAtMs}))
        table.insert(result, logicalKey)
        table.insert(result, version)
    end
end
return result
`;

const SCAN_PAGE_BODY = `
local function page(snapshotKey, offset, limit)
    local length = redis.call('LLEN', snapshotKey)
    if length == 0 then return {0, nowMs} end
    local result = {1, nowMs}
    local used = 0
    local count = 0
    local position = offset
    while position < length and count < limit do
        local item = redis.call('LINDEX', snapshotKey, position)
        local decoded = cjson.decode(item)
        local record = cjson.decode(decoded[2])
        local size = string.len(decoded[1]) + string.len(record[1])
          + string.len(record[2]) + 64
        if count > 0 and used + size > 4194304 then break end
        table.insert(result, decoded[1])
        table.insert(result, decoded[2])
        used = used + size
        count = count + 1
        position = position + 1
    end
    if position < length then
        table.insert(result, 3, tostring(position))
    else
        table.insert(result, 3, '')
        redis.call('DEL', snapshotKey)
    end
    return result
end
`;

export const OPAQUE_SCAN_START_SCRIPT = PROLOGUE + SCAN_PAGE_BODY + `
local entries = redis.call('HGETALL', KEYS[1])
local snapshot = {}
for index = 1, #entries, 2 do
    local logicalKey = entries[index]
    if string.sub(logicalKey, 1, string.len(ARGV[1])) == ARGV[1] then
        local record = cjson.decode(entries[index + 1])
        local expiresAtMs = tonumber(record[3] or '0')
        if expiresAtMs == 0 or expiresAtMs > nowMs then
            table.insert(snapshot, cjson.encode({logicalKey, entries[index + 1]}))
        end
    end
end
table.sort(snapshot, function(left, right)
    return cjson.decode(left)[1] < cjson.decode(right)[1]
end)
if #snapshot == 0 then
    redis.call('RPUSH', KEYS[2], cjson.encode({'', cjson.encode({'', '', 0})}))
else
    for _, item in ipairs(snapshot) do
        redis.call('RPUSH', KEYS[2], item)
    end
end
redis.call('PEXPIRE', KEYS[2], 60000)
local result = page(KEYS[2], 0, tonumber(ARGV[2]))
if #snapshot == 0 then
    redis.call('DEL', KEYS[2])
    return {1, nowMs, ''}
end
return result
`;

export const OPAQUE_SCAN_CONTINUE_SCRIPT = PROLOGUE + SCAN_PAGE_BODY + `
if redis.call('EXISTS', KEYS[1]) ~= 1 then return {0, nowMs} end
return page(KEYS[1], tonumber(ARGV[1]), tonumber(ARGV[2]))
`;

export const BLOB_PUT_SCRIPT = PROLOGUE + `
local row = redis.call('HMGET', KEYS[1], 'reference', 'bytes', 'expiresAtMs')
if row[1] then
    if row[1] ~= ARGV[1] or row[2] ~= ARGV[2] then
        return {'conflict', nowMs}
    end
    return {'alreadyStored', nowMs, row[3]}
end
local expiresAtMs = nowMs + tonumber(ARGV[3])
redis.call(
    'HSET', KEYS[1],
    'reference', ARGV[1],
    'bytes', ARGV[2],
    'expiresAtMs', tostring(expiresAtMs))
redis.call('PEXPIREAT', KEYS[1], expiresAtMs)
return {'stored', nowMs, expiresAtMs}
`;

export const BLOB_READ_SCRIPT = PROLOGUE + `
local row = redis.call('HMGET', KEYS[1], 'reference', 'bytes', 'expiresAtMs')
if not row[1] or row[1] ~= ARGV[1] then return {0, nowMs} end
local expiresAtMs = tonumber(row[3] or '0')
if expiresAtMs <= nowMs then
    redis.call('DEL', KEYS[1])
    return {0, nowMs}
end
return {1, nowMs, row[2], expiresAtMs}
`;

export const BLOB_RENEW_SCRIPT = PROLOGUE + `
local row = redis.call('HMGET', KEYS[1], 'reference', 'expiresAtMs')
if not row[1] or row[1] ~= ARGV[1] then return {0, nowMs} end
local currentExpiry = tonumber(row[2] or '0')
if currentExpiry <= nowMs then
    redis.call('DEL', KEYS[1])
    return {0, nowMs}
end
local expiresAtMs = nowMs + tonumber(ARGV[2])
redis.call('HSET', KEYS[1], 'expiresAtMs', tostring(expiresAtMs))
redis.call('PEXPIREAT', KEYS[1], expiresAtMs)
return {1, nowMs, expiresAtMs}
`;
