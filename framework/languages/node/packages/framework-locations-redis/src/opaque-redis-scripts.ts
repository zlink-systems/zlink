const PROLOGUE = `
if redis.replicate_commands then redis.replicate_commands() end
local time = redis.call('TIME')
local nowMs = tonumber(time[1]) * 1000 + math.floor(tonumber(time[2]) / 1000)
`;

// Shared decode helper. Every opaque row is a Redis ZSET append-log; the
// member with the highest score (the provider's monotonically increasing
// INCR sequence) is current. Each member is a 1-byte format tag (0x01)
// followed by a cmsgpack array {originalKey, rawBytes, version,
// expiresAtMs, tombstone}. expiresAtMs == 0 means no expiry (unsigned int
// family, never negative). tombstone is a real MessagePack bool.
// Unrecognized format tags fail explicitly rather than guessing how to
// read the value (21-location-runtime.md#2.4, 22-location-store-redis.md#7).
const DECODE_HELPERS = `
local function decodeMember(raw)
    if string.byte(raw, 1) ~= 1 then
        return redis.error_reply('zlink opaque record: unrecognized format tag')
    end
    return cmsgpack.unpack(string.sub(raw, 2))
end

local function liveRecordAt(rowKey, referenceMs)
    local members = redis.call('ZREVRANGE', rowKey, 0, 0)
    if #members == 0 then return nil end
    local record = decodeMember(members[1])
    local expiresAtMs = tonumber(record[4])
    if record[5] == true or (expiresAtMs > 0 and expiresAtMs <= referenceMs) then
        return nil
    end
    return record
end

local function encodeMember(originalKey, bytes, version, expiresAtMs, tombstone)
    return string.char(1) .. cmsgpack.pack({
        originalKey, bytes, version, expiresAtMs, tombstone
    })
end
`;

export const OPAQUE_READ_SCRIPT = PROLOGUE + DECODE_HELPERS + `
local record = liveRecordAt(KEYS[1], nowMs)
if not record then return {0, nowMs} end
return {1, nowMs, record[1], record[2], record[3], tostring(tonumber(record[4]))}
`;

// KEYS[1..6] = indexKey, mapKey, cleanupKey, sequenceKey, snapshotExpiryKey,
// snapshotBoundaryKey (private auxiliary keys, not part of the public
// contract). KEYS[7..] = the opaque record row keys referenced by
// conditions/mutations, in the order fixed by ARGV[1]/ARGV[2].
// ARGV[1] = JSON conditions: ['missing', keyIndex, originalKey]
//           | ['version', keyIndex, originalKey, expectedVersion]
// ARGV[2] = JSON mutation metadata (no raw bytes, JSON must stay text-safe):
//           ['put', keyIndex, originalKey, retentionMsOrFalse]
//           | ['delete', keyIndex, originalKey]
// ARGV[3..] = one raw-bytes argument per 'put' mutation, in mutation order.
export const OPAQUE_WRITE_SCRIPT = PROLOGUE + DECODE_HELPERS + `
local indexKey = KEYS[1]
local mapKey = KEYS[2]
local cleanupKey = KEYS[3]
local sequenceKey = KEYS[4]
local snapshotExpiryKey = KEYS[5]
local snapshotBoundaryKey = KEYS[6]

local expiredSnapshots = redis.call('ZRANGEBYSCORE', snapshotExpiryKey, '-inf', nowMs, 'LIMIT', 0, 128)
for _, snapshotId in ipairs(expiredSnapshots) do
    redis.call('ZREM', snapshotExpiryKey, snapshotId)
    redis.call('ZREM', snapshotBoundaryKey, snapshotId)
end
local minimumBoundary = nil
local boundaryEntry = redis.call('ZRANGE', snapshotBoundaryKey, 0, 0, 'WITHSCORES')
if #boundaryEntry == 2 then minimumBoundary = tonumber(boundaryEntry[2]) end

local due = redis.call('ZRANGEBYSCORE', cleanupKey, '-inf', nowMs, 'LIMIT', 0, 32)
for _, original in ipairs(due) do
    local rowKey = redis.call('HGET', mapKey, original)
    local members = {}
    if rowKey then
        members = redis.call('ZREVRANGE', rowKey, 0, 0, 'WITHSCORES')
    end
    if #members == 0 then
        redis.call('ZREM', indexKey, original)
        redis.call('HDEL', mapKey, original)
        redis.call('ZREM', cleanupKey, original)
    elseif minimumBoundary then
        local anchor = redis.call('ZREVRANGEBYSCORE', rowKey, minimumBoundary, '-inf', 'WITHSCORES', 'LIMIT', 0, 1)
        if #anchor == 2 then
            redis.call('ZREMRANGEBYSCORE', rowKey, '-inf', '(' .. anchor[2])
        end
        redis.call('ZADD', cleanupKey, nowMs + 1000, original)
    else
        local record = decodeMember(members[1])
        local expiresAtMs = tonumber(record[4])
        if record[5] == true or (expiresAtMs > 0 and expiresAtMs + 60000 <= nowMs) then
            redis.call('DEL', rowKey)
            redis.call('ZREM', indexKey, original)
            redis.call('HDEL', mapKey, original)
            redis.call('ZREM', cleanupKey, original)
        else
            redis.call('ZREMRANGEBYRANK', rowKey, 0, -2)
            if expiresAtMs > 0 then
                redis.call('ZADD', cleanupKey, math.max(nowMs + 1000, expiresAtMs + 60000), original)
            else
                redis.call('ZREM', cleanupKey, original)
            end
        end
    end
end

local conditions = cjson.decode(ARGV[1])
local mutations = cjson.decode(ARGV[2])

for _, condition in ipairs(conditions) do
    local record = liveRecordAt(KEYS[condition[2] + 6], nowMs)
    local currentVersion = record and record[3] or nil
    if condition[1] == 'missing' then
        if currentVersion ~= nil then return {'conflict', nowMs} end
    elseif currentVersion ~= condition[4] then
        return {'conflict', nowMs}
    end
end

for _, mutation in ipairs(mutations) do
    local rowKey = KEYS[mutation[2] + 6]
    if not minimumBoundary then
        redis.call('ZREMRANGEBYRANK', rowKey, 0, -2)
    end
    if redis.call('ZCARD', rowKey) >= 128 then
        return {'backlog', nowMs}
    end
end

local sequence = tostring(redis.call('INCR', sequenceKey))
local byteArg = 3
local result = {'applied', nowMs}
for _, mutation in ipairs(mutations) do
    local rowKey = KEYS[mutation[2] + 6]
    local originalKey = mutation[3]
    if mutation[1] == 'put' then
        local bytes = ARGV[byteArg]
        byteArg = byteArg + 1
        local retention = mutation[4]
        local expiresAtMs = 0
        if retention ~= false then expiresAtMs = nowMs + tonumber(retention) end
        redis.call('ZADD', rowKey, sequence, encodeMember(originalKey, bytes, sequence, expiresAtMs, false))
        redis.call('ZADD', indexKey, 0, originalKey)
        redis.call('HSET', mapKey, originalKey, rowKey)
        table.insert(result, originalKey)
        table.insert(result, sequence)
    else
        redis.call('ZADD', rowKey, sequence, encodeMember(originalKey, '', sequence, 0, true))
        redis.call('ZADD', indexKey, 0, originalKey)
        redis.call('HSET', mapKey, originalKey, rowKey)
    end
    local dueAt = nowMs + 1000
    local scheduled = redis.call('ZSCORE', cleanupKey, originalKey)
    if not scheduled or tonumber(scheduled) > dueAt then
        redis.call('ZADD', cleanupKey, dueAt, originalKey)
    end
end
return result
`;

// Shared body for point-in-time paged scanning. KEYS[1]=indexKey,
// KEYS[2]=mapKey, KEYS[3]=snapshotKey, KEYS[4]=cleanupKey,
// KEYS[5]=sequenceKey, KEYS[6]=snapshotExpiryKey, KEYS[7]=snapshotBoundaryKey.
const SCAN_CLEANUP_AND_BOUNDARY = DECODE_HELPERS + `
local expiredSnapshots = redis.call('ZRANGEBYSCORE', KEYS[6], '-inf', nowMs, 'LIMIT', 0, 128)
for _, expiredId in ipairs(expiredSnapshots) do
    redis.call('ZREM', KEYS[6], expiredId)
    redis.call('ZREM', KEYS[7], expiredId)
end
local minimumBoundary = nil
local boundaryEntry = redis.call('ZRANGE', KEYS[7], 0, 0, 'WITHSCORES')
if #boundaryEntry == 2 then minimumBoundary = tonumber(boundaryEntry[2]) end

local due = redis.call('ZRANGEBYSCORE', KEYS[4], '-inf', nowMs, 'LIMIT', 0, 32)
for _, original in ipairs(due) do
    local rowKey = redis.call('HGET', KEYS[2], original)
    local members = {}
    if rowKey then
        members = redis.call('ZREVRANGE', rowKey, 0, 0, 'WITHSCORES')
    end
    if #members == 0 then
        redis.call('ZREM', KEYS[1], original)
        redis.call('HDEL', KEYS[2], original)
        redis.call('ZREM', KEYS[4], original)
    elseif minimumBoundary then
        local anchor = redis.call('ZREVRANGEBYSCORE', rowKey, minimumBoundary, '-inf', 'WITHSCORES', 'LIMIT', 0, 1)
        if #anchor == 2 then
            redis.call('ZREMRANGEBYSCORE', rowKey, '-inf', '(' .. anchor[2])
        end
        redis.call('ZADD', KEYS[4], nowMs + 1000, original)
    else
        local record = decodeMember(members[1])
        local expiresAtMs = tonumber(record[4])
        if record[5] == true or (expiresAtMs > 0 and expiresAtMs + 60000 <= nowMs) then
            redis.call('DEL', rowKey)
            redis.call('ZREM', KEYS[1], original)
            redis.call('HDEL', KEYS[2], original)
            redis.call('ZREM', KEYS[4], original)
        else
            redis.call('ZREMRANGEBYRANK', rowKey, 0, -2)
            if expiresAtMs > 0 then
                redis.call('ZADD', KEYS[4], math.max(nowMs + 1000, expiresAtMs + 60000), original)
            else
                redis.call('ZREM', KEYS[4], original)
            end
        end
    end
end
`;

const SCAN_PAGE_READ = `
local metadata = redis.call('HMGET', KEYS[3], 'now', 'boundary', 'prefix')
if not metadata[1] or metadata[3] ~= prefix then
    redis.call('ZREM', KEYS[6], snapshotId)
    redis.call('ZREM', KEYS[7], snapshotId)
    return {'expired'}
end
local snapshotNow = tonumber(metadata[1])
local boundary = tonumber(metadata[2])
local lower = '-'
if string.len(lastKey) > 0 then lower = '(' .. lastKey end
local workLimit = math.max(limit * 4, 128)
local originals = redis.call('ZRANGEBYLEX', KEYS[1], lower, '+', 'LIMIT', 0, workLimit + 1)
local emitted = 0
local encodedBytes = 0
local examined = 0
local result = {'page', tostring(snapshotNow), ''}
while examined < #originals and examined < workLimit and emitted < limit do
    local original = originals[examined + 1]
    examined = examined + 1
    if string.sub(original, 1, string.len(prefix)) == prefix then
        local rowKey = redis.call('HGET', KEYS[2], original)
        if rowKey then
            local members = redis.call('ZREVRANGEBYSCORE', rowKey, boundary, '-inf', 'LIMIT', 0, 1)
            if #members > 0 then
                local record = decodeMember(members[1])
                local expiresAtMs = tonumber(record[4])
                if record[1] == original and record[5] ~= true
                    and (expiresAtMs == 0 or expiresAtMs > snapshotNow) then
                    local itemBytes = string.len(original) + string.len(record[2]) + string.len(record[3]) + 128
                    if emitted > 0 and encodedBytes + itemBytes > 4194304 then
                        examined = examined - 1
                        break
                    end
                    table.insert(result, original)
                    table.insert(result, record[2])
                    table.insert(result, record[3])
                    table.insert(result, tostring(expiresAtMs))
                    encodedBytes = encodedBytes + itemBytes
                    emitted = emitted + 1
                end
            end
        end
    end
end

local hasMore = examined < #originals
if not hasMore and #originals > workLimit then hasMore = true end
if hasMore then
    result[3] = originals[examined]
else
    redis.call('DEL', KEYS[3])
    redis.call('ZREM', KEYS[6], snapshotId)
    redis.call('ZREM', KEYS[7], snapshotId)
end
return result
`;

// ARGV = [prefix, limit, snapshotId]
export const OPAQUE_SCAN_START_SCRIPT = PROLOGUE + SCAN_CLEANUP_AND_BOUNDARY + `
local prefix = ARGV[1]
local limit = tonumber(ARGV[2])
local snapshotId = ARGV[3]
local lastKey = ''

if redis.call('ZCARD', KEYS[6]) >= 4096 then
    return {'capacity'}
end
redis.call('DEL', KEYS[3])
local boundary = tonumber(redis.call('GET', KEYS[5]) or '0')
redis.call('HSET', KEYS[3], 'now', tostring(nowMs), 'boundary', tostring(boundary), 'prefix', prefix)
redis.call('PEXPIRE', KEYS[3], 60000)
redis.call('ZADD', KEYS[6], nowMs + 60000, snapshotId)
redis.call('ZADD', KEYS[7], boundary, snapshotId)
` + SCAN_PAGE_READ;

// ARGV = [prefix, lastKeyHex, limit, snapshotId]
export const OPAQUE_SCAN_CONTINUE_SCRIPT = PROLOGUE + SCAN_CLEANUP_AND_BOUNDARY + `
local prefix = ARGV[1]
local lastKey = ARGV[2]
local limit = tonumber(ARGV[3])
local snapshotId = ARGV[4]

if redis.call('EXISTS', KEYS[3]) == 0 then
    redis.call('ZREM', KEYS[6], snapshotId)
    redis.call('ZREM', KEYS[7], snapshotId)
    return {'expired'}
end
` + SCAN_PAGE_READ;

// Relocation Store: raw-bytes STRING payloads at
// {prefix}:zlink-relocation-v1:blob:{reference}, retention via PSETEX/PX
// (23-relocation-store-redis.md#8). KEYS[1] is the blob key -- the reference
// itself is already the key's last segment, so identity on retry is decided
// by comparing the stored bytes against ARGV[1].
export const BLOB_PUT_SCRIPT = PROLOGUE + `
local existing = redis.call('GET', KEYS[1])
if existing then
    if existing ~= ARGV[1] then
        return {'conflict', nowMs}
    end
    local ttl = redis.call('PTTL', KEYS[1])
    local expiresAtMs = nowMs + math.max(ttl, 0)
    return {'alreadyStored', nowMs, tostring(expiresAtMs)}
end
local retentionMs = tonumber(ARGV[2])
redis.call('SET', KEYS[1], ARGV[1], 'PX', retentionMs)
return {'stored', nowMs, tostring(nowMs + retentionMs)}
`;

export const BLOB_READ_SCRIPT = PROLOGUE + `
local bytes = redis.call('GET', KEYS[1])
if not bytes then return {0, nowMs} end
local ttl = redis.call('PTTL', KEYS[1])
local expiresAtMs = nowMs + math.max(ttl, 0)
return {1, nowMs, bytes, tostring(expiresAtMs)}
`;

export const BLOB_RENEW_SCRIPT = PROLOGUE + `
if redis.call('EXISTS', KEYS[1]) == 0 then return {0, nowMs} end
local retentionMs = tonumber(ARGV[1])
redis.call('PEXPIRE', KEYS[1], retentionMs)
return {1, nowMs, tostring(nowMs + retentionMs)}
`;
