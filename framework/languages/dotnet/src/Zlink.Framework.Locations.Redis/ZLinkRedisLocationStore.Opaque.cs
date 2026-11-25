using System.Globalization;
using System.Text;
using StackExchange.Redis;
using Zlink.Framework.Internal;
using static Zlink.Framework.Internal.ZLinkLocationStoreLimits;

namespace Zlink.Framework.Locations.Redis;

public sealed partial class ZLinkRedisLocationStore
{
    private const int OpaqueFormatTag = 1;
    private const int OpaqueDataOffset = 2;
    private const int CleanupRetryMilliseconds = 1000;
    private const int ExpiredRecordGraceMilliseconds = 60000;
    private const int SnapshotLifetimeMilliseconds = 60000;
    private const int SnapshotCleanupBatch = 128;
    private const int RecordCleanupBatch = 32;
    private const int MaximumVersionBacklog = 128;
    private const int MaximumSnapshots = 4096;
    private const int ScanWorkMultiplier = 4;
    private const int MinimumScanWork = 128;
    private const int ScanItemOverheadBytes = 128;
    private const int Utf8PrefixUpperBoundByte = 0xff;
    private const int RecordKeyPosition = 1;
    private const int RecordValuePosition = 2;
    private const int RecordVersionPosition = 3;
    private const int RecordExpiryPosition = 4;
    private const int RecordTombstonePosition = 5;
    private const int WriteMutationStride = 6;
    private const int ResultOutcomeIndex = 0;
    private const int ResultStoreNowIndex = 1;
    private const int ReadKeyIndex = 2;
    private const int ReadValueIndex = 3;
    private const int ReadVersionIndex = 4;
    private const int ReadExpiryIndex = 5;
    private const int WriteVersionsIndex = 2;
    private const int WriteVersionStride = 2;
    private const int ScanItemsIndex = 3;
    private const int ScanItemStride = 4;
    private const int ScanNextKeyIndex = 2;
    private const int ScanValueOffset = 1;
    private const int ScanVersionOffset = 2;
    private const int ScanExpiryOffset = 3;
    private const int WriteVersionOffset = 1;
    private const int CursorIdLength = 32;
    private const string MissingToken = "missing";
    private const string FoundToken = "found";
    private const string FormatErrorToken = "format-error";
    private const string VersionToken = "version";
    private const string ValueToken = "value";
    private const string PutToken = "put";
    private const string AddToken = "add";
    private const string SumAtMostToken = "sum-at-most";
    private const string CorruptToken = "corrupt";
    private const string DeleteToken = "delete";
    private const string ConflictToken = "conflict";
    private const string BacklogToken = "backlog";
    private const string AppliedToken = "applied";
    private const string ExpiredToken = "expired";
    private const string CapacityToken = "capacity";
    private const string PageToken = "page";
    private const string SnapshotNowField = "now";
    private const string SnapshotBoundaryField = "boundary";
    private const string SnapshotPrefixField = "prefix";
    private const string UuidFormat = "N";

    private static readonly string OpaqueReadScript = $$"""
        if redis.replicate_commands then redis.replicate_commands() end
        local function unpackTagged(raw)
            if string.byte(raw, 1) ~= {{OpaqueFormatTag}} then
                return nil
            end
            return cmsgpack.unpack(string.sub(raw, {{OpaqueDataOffset}}))
        end
        local time = redis.call('TIME')
        local nowMs = tonumber(time[1]) * 1000 + math.floor(tonumber(time[2]) / 1000)
        local members = redis.call('ZREVRANGE', KEYS[1], 0, 0)
        if #members == 0 then
            return { '{{MissingToken}}', nowMs }
        end
        local record = unpackTagged(members[1])
        if not record then
            return { '{{FormatErrorToken}}', nowMs }
        end
        local expiresAt = tonumber(record[{{RecordExpiryPosition}}])
        if record[{{RecordTombstonePosition}}] == true or (expiresAt > 0 and expiresAt <= nowMs) then
            return { '{{MissingToken}}', nowMs }
        end
        return {
            '{{FoundToken}}',
            nowMs,
            record[{{RecordKeyPosition}}],
            record[{{RecordValuePosition}}],
            record[{{RecordVersionPosition}}],
            expiresAt
        }
        """;

    private const string PruneHistoryHelper = """
        local function pruneHistory(recordKey, minimumBoundary)
            if minimumBoundary then
                local anchor = redis.call(
                    'ZREVRANGEBYSCORE', recordKey,
                    minimumBoundary, '-inf', 'WITHSCORES', 'LIMIT', 0, 1)
                if #anchor == 2 then
                    redis.call('ZREMRANGEBYSCORE', recordKey, '-inf', '(' .. anchor[2])
                end
            else
                redis.call('ZREMRANGEBYRANK', recordKey, 0, -2)
            end
        end

        """;

    private static readonly string OpaqueWriteScript =
        PruneHistoryHelper
        + $$"""
            if redis.replicate_commands then redis.replicate_commands() end
            local function unpackTagged(raw)
                if string.byte(raw, 1) ~= {{OpaqueFormatTag}} then
                    return nil
                end
                return cmsgpack.unpack(string.sub(raw, {{OpaqueDataOffset}}))
            end
            local conditionCount = tonumber(ARGV[1])
            local mutationCount = tonumber(ARGV[2])
            local indexKey = KEYS[#KEYS - 5]
            local mapKey = KEYS[#KEYS - 4]
            local cleanupKey = KEYS[#KEYS - 3]
            local sequenceKey = KEYS[#KEYS - 2]
            local snapshotExpiryKey = KEYS[#KEYS - 1]
            local snapshotBoundaryKey = KEYS[#KEYS]
            local time = redis.call('TIME')
            local nowMs = tonumber(time[1]) * 1000
                + math.floor(tonumber(time[2]) / 1000)

            local expiredSnapshots = redis.call(
                'ZRANGEBYSCORE',
                snapshotExpiryKey,
                '-inf',
                nowMs,
                'LIMIT',
                0,
                {{SnapshotCleanupBatch}})
            for _, snapshotId in ipairs(expiredSnapshots) do
                redis.call('ZREM', snapshotExpiryKey, snapshotId)
                redis.call('ZREM', snapshotBoundaryKey, snapshotId)
            end
            local minimumBoundary = nil
            local boundaryEntry = redis.call(
                'ZRANGE', snapshotBoundaryKey, 0, 0, 'WITHSCORES')
            if #boundaryEntry == 2 then
                minimumBoundary = tonumber(boundaryEntry[2])
            end

            local due = redis.call(
                'ZRANGEBYSCORE', cleanupKey, '-inf', nowMs, 'LIMIT', 0, {{RecordCleanupBatch}})
            for _, original in ipairs(due) do
                local recordKey = redis.call('HGET', mapKey, original)
                local members = {}
                if recordKey then
                    members = redis.call(
                        'ZREVRANGE', recordKey, 0, 0, 'WITHSCORES')
                end
                if #members == 0 then
                    redis.call('ZREM', indexKey, original)
                    redis.call('HDEL', mapKey, original)
                    redis.call('ZREM', cleanupKey, original)
                elseif minimumBoundary then
                    pruneHistory(recordKey, minimumBoundary)
                    redis.call(
                        'ZADD', cleanupKey, nowMs + {{CleanupRetryMilliseconds}}, original)
                else
                    local record = unpackTagged(members[1])
                    if not record then
                        return { '{{FormatErrorToken}}', nowMs }
                    end
                    local expiresAt = tonumber(record[{{RecordExpiryPosition}}])
                    if record[{{RecordTombstonePosition}}] == true
                        or (expiresAt > 0 and expiresAt + {{ExpiredRecordGraceMilliseconds}} <= nowMs) then
                        redis.call('DEL', recordKey)
                        redis.call('ZREM', indexKey, original)
                        redis.call('HDEL', mapKey, original)
                        redis.call('ZREM', cleanupKey, original)
                    else
                        pruneHistory(recordKey, minimumBoundary)
                        if expiresAt > 0 then
                            redis.call(
                                'ZADD',
                                cleanupKey,
                                math.max(nowMs + {{CleanupRetryMilliseconds}}, expiresAt + {{ExpiredRecordGraceMilliseconds}}),
                                original)
                        else
                            redis.call('ZREM', cleanupKey, original)
                        end
                    end
                end
            end

            local function readCurrent(keyIndex)
                local members = redis.call('ZREVRANGE', KEYS[keyIndex], 0, 0)
                if #members == 0 then return nil, nil, true end
                local record = unpackTagged(members[1])
                if not record then return nil, nil, false end
                local expiresAt = tonumber(record[{{RecordExpiryPosition}}])
                if record[{{RecordTombstonePosition}}] == true
                    or (expiresAt > 0 and expiresAt <= nowMs) then
                    return nil, nil, true
                end
                return record[{{RecordVersionPosition}}], record[{{RecordValuePosition}}], true
            end

            local maxCounter = '9223372036854775807'
            local function validCounter(value)
                return string.match(value, '^%d+$') and (#value == 1 or value:sub(1, 1) ~= '0')
                    and (#value < 19 or (#value == 19 and value <= maxCounter))
            end

            -- Redis Lua numbers cannot represent an arbitrary int64 exactly.
            -- Three base-10^7 limbs keep even the sum of a full batch exact.
            local counterBase = 10000000
            local function accumulate(total, digits, sign)
                if digits:sub(1, 1) == '-' then
                    digits = digits:sub(2)
                    sign = -sign
                end
                total[1] = total[1] + sign * (tonumber(digits:sub(-7)) or 0)
                total[2] = total[2] + sign * (tonumber(digits:sub(-14, -8)) or 0)
                total[3] = total[3] + sign * (tonumber(digits:sub(1, -15)) or 0)
            end
            local function normalize(total)
                for i = 1, 2 do
                    local carry = math.floor(total[i] / counterBase)
                    total[i] = total[i] - carry * counterBase
                    total[i + 1] = total[i + 1] + carry
                end
            end
            local function readCounter(keyIndex)
                local _, value, valid = readCurrent(keyIndex)
                if not valid then return nil, '{{FormatErrorToken}}' end
                if value == nil then return '0' end
                if validCounter(value) then return value end
                return nil, '{{CorruptToken}}'
            end

            local arg = 3
            for i = 1, conditionCount do
                local kind = ARGV[arg]
                if kind == '{{SumAtMostToken}}' then
                    local total = {0, 0, 0}
                    accumulate(total, ARGV[arg + 1], 1)
                    accumulate(total, ARGV[arg + 2], -1)
                    local count = tonumber(ARGV[arg + 3])
                    for offset = 0, count - 1 do
                        local value, failure = readCounter(tonumber(ARGV[arg + 4 + offset]))
                        if not value then return { failure, nowMs } end
                        accumulate(total, value, 1)
                    end
                    normalize(total)
                    if total[3] > 0 or (total[3] == 0 and (total[2] > 0 or total[1] > 0)) then
                        return { '{{ConflictToken}}', nowMs }
                    end
                    arg = arg + 4 + count
                else
                    local keyIndex = tonumber(ARGV[arg + 1])
                    local expected = ARGV[arg + 2]
                    local current, currentValue, valid = readCurrent(keyIndex)
                    if not valid then return { '{{FormatErrorToken}}', nowMs } end
                    if (kind == '{{MissingToken}}' and current ~= nil)
                        or (kind == '{{VersionToken}}' and current ~= expected)
                        or (kind == '{{ValueToken}}' and (current == nil or currentValue ~= expected)) then
                        return { '{{ConflictToken}}', nowMs }
                    end
                    arg = arg + 3
                end
            end

            if mutationCount == 0 then return { '{{AppliedToken}}', nowMs } end
            local additions = nil
            local counterArg = arg
            for i = 1, mutationCount do
                if ARGV[counterArg + 1] == '{{AddToken}}' then
                    local current, failure = readCounter(tonumber(ARGV[counterArg]))
                    if not current then return { failure, nowMs } end
                    local total = {0, 0, 0}
                    accumulate(total, current, 1)
                    accumulate(total, ARGV[counterArg + 3], 1)
                    normalize(total)
                    if total[3] < 0 then return { '{{CorruptToken}}', nowMs } end
                    local value = string.format('%d%07d%07d', total[3], total[2], total[1])
                    value = value:gsub('^0+', '')
                    if value == '' then value = '0' end
                    if not validCounter(value) then return { '{{CorruptToken}}', nowMs } end
                    additions = additions or {}
                    additions[i] = value
                end
                counterArg = counterArg + {{WriteMutationStride}}
            end
            local checkArg = arg
            for i = 1, mutationCount do
                local keyIndex = tonumber(ARGV[checkArg])
                pruneHistory(KEYS[keyIndex], minimumBoundary)
                if redis.call('ZCARD', KEYS[keyIndex]) >= {{MaximumVersionBacklog}} then
                    return { '{{BacklogToken}}', nowMs }
                end
                checkArg = checkArg + {{WriteMutationStride}}
            end
            local sequence = redis.call('INCR', sequenceKey)
            local putVersions = {}
            for i = 1, mutationCount do
                local keyIndex = tonumber(ARGV[arg])
                local kind = ARGV[arg + 1]
                local originalKey = ARGV[arg + 2]
                local value = ARGV[arg + 3]
                local version = ARGV[arg + 4]
                local retention = tonumber(ARGV[arg + 5])
                local redisKey = KEYS[keyIndex]
                if kind == '{{PutToken}}' or kind == '{{AddToken}}' then
                    if kind == '{{AddToken}}' then value = additions[i] end
                    local expiresAt = 0
                    if retention >= 0 then expiresAt = nowMs + retention end
                    redis.call(
                        'ZADD',
                        redisKey,
                        sequence,
                        '\{{OpaqueFormatTag}}' .. cmsgpack.pack({
                            originalKey,
                            value,
                            version,
                            expiresAt,
                            false
                        }))
                    redis.call('ZADD', indexKey, 0, originalKey)
                    redis.call('HSET', mapKey, originalKey, redisKey)
                    table.insert(putVersions, originalKey)
                    table.insert(putVersions, version)
                else
                    redis.call(
                        'ZADD',
                        redisKey,
                        sequence,
                        '\{{OpaqueFormatTag}}' .. cmsgpack.pack({
                            originalKey,
                            '',
                            version,
                            0,
                            true
                        }))
                    redis.call('ZADD', indexKey, 0, originalKey)
                    redis.call('HSET', mapKey, originalKey, redisKey)
                end
                local dueAt = nowMs + {{CleanupRetryMilliseconds}}
                local scheduled = redis.call(
                    'ZSCORE', cleanupKey, originalKey)
                if not scheduled or tonumber(scheduled) > dueAt then
                    redis.call('ZADD', cleanupKey, dueAt, originalKey)
                end
                arg = arg + {{WriteMutationStride}}
            end

            local result = { '{{AppliedToken}}', nowMs }
            for _, item in ipairs(putVersions) do table.insert(result, item) end
            return result
            """;

    private static readonly string OpaqueScanScript =
        PruneHistoryHelper
        + $$"""
            if redis.replicate_commands then redis.replicate_commands() end
            local function unpackTagged(raw)
                if string.byte(raw, 1) ~= {{OpaqueFormatTag}} then
                    return nil
                end
                return cmsgpack.unpack(string.sub(raw, {{OpaqueDataOffset}}))
            end
            local prefix = ARGV[1]
            local lastKey = ARGV[2]
            local limit = tonumber(ARGV[3])
            local create = ARGV[4] == '1'
            local snapshot = KEYS[3]
            local cleanupKey = KEYS[4]
            local sequenceKey = KEYS[5]
            local snapshotExpiryKey = KEYS[6]
            local snapshotBoundaryKey = KEYS[7]
            local snapshotId = ARGV[5]
            local time = redis.call('TIME')
            local nowMs = tonumber(time[1]) * 1000
                + math.floor(tonumber(time[2]) / 1000)

            local expiredSnapshots = redis.call(
                'ZRANGEBYSCORE',
                snapshotExpiryKey,
                '-inf',
                nowMs,
                'LIMIT',
                0,
                {{SnapshotCleanupBatch}})
            for _, expiredId in ipairs(expiredSnapshots) do
                redis.call('ZREM', snapshotExpiryKey, expiredId)
                redis.call('ZREM', snapshotBoundaryKey, expiredId)
            end
            local minimumBoundary = nil
            local boundaryEntry = redis.call(
                'ZRANGE', snapshotBoundaryKey, 0, 0, 'WITHSCORES')
            if #boundaryEntry == 2 then
                minimumBoundary = tonumber(boundaryEntry[2])
            end

            local due = redis.call(
                'ZRANGEBYSCORE', cleanupKey, '-inf', nowMs, 'LIMIT', 0, {{RecordCleanupBatch}})
            for _, original in ipairs(due) do
                local recordKey = redis.call('HGET', KEYS[2], original)
                local members = {}
                if recordKey then
                    members = redis.call(
                        'ZREVRANGE', recordKey, 0, 0, 'WITHSCORES')
                end
                if #members == 0 then
                    redis.call('ZREM', KEYS[1], original)
                    redis.call('HDEL', KEYS[2], original)
                    redis.call('ZREM', cleanupKey, original)
                elseif minimumBoundary then
                    pruneHistory(recordKey, minimumBoundary)
                    redis.call(
                        'ZADD', cleanupKey, nowMs + {{CleanupRetryMilliseconds}}, original)
                else
                    local record = unpackTagged(members[1])
                    if not record then
                        return { '{{FormatErrorToken}}' }
                    end
                    local expiresAt = tonumber(record[{{RecordExpiryPosition}}])
                    if record[{{RecordTombstonePosition}}] == true
                        or (expiresAt > 0 and expiresAt + {{ExpiredRecordGraceMilliseconds}} <= nowMs) then
                        redis.call('DEL', recordKey)
                        redis.call('ZREM', KEYS[1], original)
                        redis.call('HDEL', KEYS[2], original)
                        redis.call('ZREM', cleanupKey, original)
                    else
                        pruneHistory(recordKey, minimumBoundary)
                        if expiresAt > 0 then
                            redis.call(
                                'ZADD',
                                cleanupKey,
                                math.max(nowMs + {{CleanupRetryMilliseconds}}, expiresAt + {{ExpiredRecordGraceMilliseconds}}),
                                original)
                        else
                            redis.call('ZREM', cleanupKey, original)
                        end
                    end
                end
            end

            if create then
                if redis.call('ZCARD', snapshotExpiryKey) >= {{MaximumSnapshots}} then
                    return { '{{CapacityToken}}' }
                end
                redis.call('DEL', snapshot)
                local boundary = tonumber(redis.call('GET', sequenceKey) or '0')
                redis.call(
                    'HSET',
                    snapshot,
                    '{{SnapshotNowField}}',
                    tostring(nowMs),
                    '{{SnapshotBoundaryField}}',
                    tostring(boundary),
                    '{{SnapshotPrefixField}}',
                    prefix)
                redis.call('PEXPIRE', snapshot, {{SnapshotLifetimeMilliseconds}})
                redis.call(
                    'ZADD', snapshotExpiryKey, nowMs + {{SnapshotLifetimeMilliseconds}}, snapshotId)
                redis.call(
                    'ZADD', snapshotBoundaryKey, boundary, snapshotId)
            elseif redis.call('EXISTS', snapshot) == 0 then
                redis.call('ZREM', snapshotExpiryKey, snapshotId)
                redis.call('ZREM', snapshotBoundaryKey, snapshotId)
                return { '{{ExpiredToken}}' }
            end

            local metadata = redis.call(
                'HMGET', snapshot, '{{SnapshotNowField}}', '{{SnapshotBoundaryField}}', '{{SnapshotPrefixField}}')
            if not metadata[1] or metadata[3] ~= prefix then
                redis.call('ZREM', snapshotExpiryKey, snapshotId)
                redis.call('ZREM', snapshotBoundaryKey, snapshotId)
                return { '{{ExpiredToken}}' }
            end
            local snapshotNow = tonumber(metadata[1])
            local boundary = tonumber(metadata[2])
            local lower = '[' .. prefix
            -- Valid UTF-8 key suffixes sort below the range terminator byte.
            local upper = '[' .. prefix .. string.char({{Utf8PrefixUpperBoundByte}})
            if string.len(lastKey) > 0 then lower = '(' .. lastKey end
            local workLimit = math.max(limit * {{ScanWorkMultiplier}}, {{MinimumScanWork}})
            local originals = redis.call(
                'ZRANGEBYLEX', KEYS[1], lower, upper, 'LIMIT', 0, workLimit + 1)
            local emitted = 0
            local encodedBytes = 0
            local examined = 0
            local result = { '{{PageToken}}', tostring(snapshotNow), '' }
            while examined < #originals
                and examined < workLimit
                and emitted < limit do
                local original = originals[examined + 1]
                examined = examined + 1
                local recordKey = redis.call('HGET', KEYS[2], original)
                if recordKey then
                    local members = redis.call(
                        'ZREVRANGEBYSCORE',
                        recordKey,
                        boundary,
                        '-inf',
                        'LIMIT',
                        0,
                        1)
                    if #members > 0 then
                        local record = unpackTagged(members[1])
                        if not record then
                            return { '{{FormatErrorToken}}' }
                        end
                        local expiresAt = tonumber(record[{{RecordExpiryPosition}}])
                        if record[{{RecordKeyPosition}}] == original
                            and record[{{RecordTombstonePosition}}] ~= true
                            and (expiresAt == 0 or expiresAt > snapshotNow) then
                            local itemBytes = string.len(original)
                                + string.len(record[{{RecordValuePosition}}])
                                + string.len(record[{{RecordVersionPosition}}])
                                + {{ScanItemOverheadBytes}}
                            if emitted > 0
                                and encodedBytes + itemBytes > {{MaximumEncodedPageBytes}} then
                                examined = examined - 1
                                break
                            end
                            table.insert(result, original)
                            table.insert(result, record[{{RecordValuePosition}}])
                            table.insert(result, record[{{RecordVersionPosition}}])
                            table.insert(result, tostring(expiresAt))
                            encodedBytes = encodedBytes + itemBytes
                            emitted = emitted + 1
                        end
                    end
                end
            end

            local hasMore = examined < #originals
            if not hasMore and #originals > workLimit then hasMore = true end
            if hasMore then
                local nextKey = originals[examined]
                result[3] = nextKey
            else
                redis.call('DEL', snapshot)
                redis.call('ZREM', snapshotExpiryKey, snapshotId)
                redis.call('ZREM', snapshotBoundaryKey, snapshotId)
            end
            return result
            """;

    public async ValueTask<ZLinkStoreReadResult> ReadAsync(
        ZLinkStoreKey key,
        CancellationToken cancellationToken = default
    )
    {
        ValidateOpaqueKey(key, nameof(key));
        var result = await ExecuteAsync(
                async database =>
                    (RedisResult[])
                        (
                            await database
                                .ScriptEvaluateAsync(
                                    OpaqueReadScript,
                                    [_keys.OpaqueRecordKey(key.Value)],
                                    []
                                )
                                .ConfigureAwait(false)
                        )!,
                cancellationToken
            )
            .ConfigureAwait(false);
        var storeNow = DateTimeOffset.FromUnixTimeMilliseconds((long)result[ResultStoreNowIndex]);
        if ((string)result[ResultOutcomeIndex]! == FormatErrorToken)
            throw new InvalidDataException("The Redis opaque record format tag is unrecognized.");
        if ((string)result[ResultOutcomeIndex]! == MissingToken)
            return new ZLinkStoreReadResult.Missing(storeNow);
        if (!string.Equals((string)result[ReadKeyIndex]!, key.Value, StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                "The Redis opaque key digest resolved to a different key."
            );
        }
        // 0 is the wire sentinel for "no expiry" (21-location-runtime.md#2.4
        // / store-record-v1.json expiresAtMs), not -1 -- expiresAtMs is an
        // unsigned MessagePack field and a real epoch-ms value is always
        // positive.
        var expiresAtMs = (long)result[ReadExpiryIndex];
        return new ZLinkStoreReadResult.Found(
            new ZLinkStoreValue(
                (byte[])result[ReadValueIndex]!,
                new ZLinkStoreVersion((string)result[ReadVersionIndex]!),
                expiresAtMs > 0 ? DateTimeOffset.FromUnixTimeMilliseconds(expiresAtMs) : null,
                storeNow
            )
        );
    }

    public async ValueTask<ZLinkStoreWriteResult> WriteAsync(
        ZLinkStoreWriteRequest request,
        CancellationToken cancellationToken = default
    )
    {
        ArgumentNullException.ThrowIfNull(request);
        var uniqueKeys = ValidateWriteRequest(request);
        var keyIndex = uniqueKeys
            .Select((key, index) => (key, index: index + 1))
            .ToDictionary(static item => item.key, static item => item.index);
        var redisKeys = uniqueKeys
            .Select(key => _keys.OpaqueRecordKey(key.Value))
            .Append(_keys.OpaqueIndexKey())
            .Append(_keys.OpaqueMapKey())
            .Append(_keys.OpaqueCleanupKey())
            .Append(_keys.OpaqueSequenceKey())
            .Append(_keys.OpaqueSnapshotExpiryKey())
            .Append(_keys.OpaqueSnapshotBoundaryKey())
            .ToArray();
        var args = new List<RedisValue> { request.Conditions.Count, request.Mutations.Count };
        foreach (var condition in request.Conditions)
        {
            switch (condition)
            {
                case ZLinkStoreCondition.Missing missing:
                    args.Add(MissingToken);
                    args.Add(keyIndex[missing.Key]);
                    args.Add(string.Empty);
                    break;
                case ZLinkStoreCondition.Version version:
                    args.Add(VersionToken);
                    args.Add(keyIndex[version.Key]);
                    args.Add(version.Expected.Value);
                    break;
                case ZLinkStoreCondition.Value value:
                    args.Add(ValueToken);
                    args.Add(keyIndex[value.Key]);
                    args.Add(value.Expected.ToArray());
                    break;
                case ZLinkStoreCondition.SumAtMost sum:
                    args.Add(SumAtMostToken);
                    args.Add(sum.Delta.ToString(CultureInfo.InvariantCulture));
                    args.Add(sum.Limit.ToString(CultureInfo.InvariantCulture));
                    args.Add(sum.Keys.Count);
                    foreach (var key in sum.Keys)
                        args.Add(keyIndex[key]);
                    break;
            }
        }
        foreach (var mutation in request.Mutations)
        {
            switch (mutation)
            {
                case ZLinkStoreMutation.Put put:
                    args.Add(keyIndex[put.Key]);
                    args.Add(PutToken);
                    args.Add(put.Key.Value);
                    args.Add(put.Bytes.ToArray());
                    args.Add(Guid.NewGuid().ToString(UuidFormat));
                    args.Add(
                        put.Retention is { } retention
                            ? Zlink.Framework.Internal.ZLinkStoreRetention.ToMilliseconds(retention)
                            : -1
                    );
                    break;
                case ZLinkStoreMutation.Add add:
                    args.Add(keyIndex[add.Key]);
                    args.Add(AddToken);
                    args.Add(add.Key.Value);
                    args.Add(add.Delta.ToString(CultureInfo.InvariantCulture));
                    args.Add(Guid.NewGuid().ToString(UuidFormat));
                    args.Add(-1);
                    break;
                case ZLinkStoreMutation.Delete delete:
                    args.Add(keyIndex[delete.Key]);
                    args.Add(DeleteToken);
                    args.Add(delete.Key.Value);
                    args.Add(Array.Empty<byte>());
                    // A tombstone still carries a real, freshly issued
                    // version (store-record-v1.json's ownerLease-tombstone
                    // vector pins a non-empty version) -- not an empty
                    // string. Nothing in the current provider reads it back
                    // (a tombstoned key reads as Missing), but the ZSET
                    // append-log entry's shape is the public opaque-record
                    // contract, so it must match cross-language.
                    args.Add(Guid.NewGuid().ToString(UuidFormat));
                    args.Add(-1);
                    break;
            }
        }

        var result = await ExecuteAsync(
                async database =>
                    (RedisResult[])
                        (
                            await database
                                .ScriptEvaluateAsync(OpaqueWriteScript, redisKeys, args.ToArray())
                                .ConfigureAwait(false)
                        )!,
                cancellationToken
            )
            .ConfigureAwait(false);
        var storeNow = DateTimeOffset.FromUnixTimeMilliseconds((long)result[ResultStoreNowIndex]);
        var outcome = (string)result[ResultOutcomeIndex]!;
        if (outcome == ConflictToken)
            return new ZLinkStoreWriteResult.Conflict(storeNow);
        if (outcome == CorruptToken)
            return new ZLinkStoreWriteResult.Corrupt(storeNow);
        if (outcome == BacklogToken)
        {
            throw new IOException("The Redis Location Store version backlog is full.");
        }
        if (outcome != AppliedToken)
        {
            throw new InvalidDataException(
                "Redis returned an unknown Location Store write result."
            );
        }
        var versions = new Dictionary<ZLinkStoreKey, ZLinkStoreVersion>();
        for (var index = WriteVersionsIndex; index < result.Length; index += WriteVersionStride)
        {
            versions[new ZLinkStoreKey((string)result[index]!)] = new ZLinkStoreVersion(
                (string)result[index + WriteVersionOffset]!
            );
        }
        return new ZLinkStoreWriteResult.Applied(versions, storeNow);
    }

    public async ValueTask<ZLinkStoreScanResult> ScanAsync(
        ZLinkStoreScanRequest request,
        CancellationToken cancellationToken = default
    )
    {
        ArgumentNullException.ThrowIfNull(request);
        if (request.Limit is < 1 or > MaximumPageItems)
            throw new ArgumentOutOfRangeException(nameof(request));
        var prefix =
            request.Prefix
            ?? throw new ArgumentException("The scan prefix cannot be null.", nameof(request));
        _ = Encoding.UTF8.GetByteCount(prefix) is <= MaximumKeyBytes
            ? 0
            : throw new ArgumentException(
                $"The scan prefix exceeds {MaximumKeyBytes} UTF-8 bytes.",
                nameof(request)
            );
        string scanId;
        var lastKey = string.Empty;
        var create = request.Cursor is null;
        if (request.Cursor is { } cursor)
        {
            var cursorValue = cursor.Value ?? string.Empty;
            var cursorBytes = Encoding.UTF8.GetByteCount(cursorValue);
            if (cursorBytes is < 1 or > MaximumCursorBytes)
                throw new ArgumentException(
                    $"Store scan cursors must be non-empty UTF-8 values of at most {MaximumCursorBytes} bytes.",
                    nameof(request)
                );
            var separator = cursorValue.LastIndexOf(':');
            if (
                separator != CursorIdLength
                || !Guid.TryParseExact(cursorValue[..separator], UuidFormat, out _)
                || !TryDecodeCursorKey(cursorValue[(separator + 1)..], out lastKey)
            )
            {
                return new ZLinkStoreScanResult.Expired();
            }
            scanId = cursorValue[..separator];
        }
        else
        {
            scanId = Guid.NewGuid().ToString(UuidFormat);
        }
        var result = await ExecuteAsync(
                async database =>
                    (RedisResult[])
                        (
                            await database
                                .ScriptEvaluateAsync(
                                    OpaqueScanScript,
                                    [
                                        _keys.OpaqueIndexKey(),
                                        _keys.OpaqueMapKey(),
                                        _keys.OpaqueScanKey(scanId),
                                        _keys.OpaqueCleanupKey(),
                                        _keys.OpaqueSequenceKey(),
                                        _keys.OpaqueSnapshotExpiryKey(),
                                        _keys.OpaqueSnapshotBoundaryKey(),
                                    ],
                                    [prefix, lastKey, request.Limit, create ? 1 : 0, scanId]
                                )
                                .ConfigureAwait(false)
                        )!,
                cancellationToken
            )
            .ConfigureAwait(false);
        var outcome = (string)result[ResultOutcomeIndex]!;
        if (outcome == ExpiredToken)
            return new ZLinkStoreScanResult.Expired();
        if (outcome == CapacityToken)
        {
            throw new IOException("The Redis Location Store snapshot capacity is full.");
        }
        if (outcome != PageToken)
        {
            throw new InvalidDataException("Redis returned an unknown Location Store scan result.");
        }

        var storeNow = DateTimeOffset.FromUnixTimeMilliseconds(
            long.Parse(
                (string)result[ResultStoreNowIndex]!,
                NumberStyles.None,
                CultureInfo.InvariantCulture
            )
        );
        var nextKey = (string)result[ScanNextKeyIndex]!;
        var items = new List<KeyValuePair<ZLinkStoreKey, ZLinkStoreValue>>();
        for (var index = ScanItemsIndex; index < result.Length; index += ScanItemStride)
        {
            var expiresAtMs = long.Parse(
                (string)result[index + ScanExpiryOffset]!,
                NumberStyles.AllowLeadingSign,
                CultureInfo.InvariantCulture
            );
            items.Add(
                new KeyValuePair<ZLinkStoreKey, ZLinkStoreValue>(
                    new ZLinkStoreKey((string)result[index]!),
                    new ZLinkStoreValue(
                        (byte[])result[index + ScanValueOffset]!,
                        new ZLinkStoreVersion((string)result[index + ScanVersionOffset]!),
                        expiresAtMs > 0
                            ? DateTimeOffset.FromUnixTimeMilliseconds(expiresAtMs)
                            : null,
                        storeNow
                    )
                )
            );
        }
        return new ZLinkStoreScanResult.Page(
            new ZLinkStoreScanPage(
                items,
                nextKey.Length > 0
                    ? new ZLinkStoreScanCursor(
                        $"{scanId}:{Convert.ToHexString(
                            Encoding.UTF8.GetBytes(nextKey))}"
                    )
                    : null,
                storeNow
            )
        );
    }

    private static bool TryDecodeCursorKey(string encoded, out string key)
    {
        key = string.Empty;
        if (encoded.Length is < 2 or > MaximumKeyBytes * 2 || encoded.Length % 2 != 0)
            return false;
        try
        {
            key = new UTF8Encoding(
                encoderShouldEmitUTF8Identifier: false,
                throwOnInvalidBytes: true
            ).GetString(Convert.FromHexString(encoded));
            return Encoding.UTF8.GetByteCount(key) <= MaximumKeyBytes;
        }
        catch (FormatException)
        {
            return false;
        }
        catch (DecoderFallbackException)
        {
            return false;
        }
    }

    private static void ValidateOpaqueKey(ZLinkStoreKey key, string parameterName)
    {
        var length = Encoding.UTF8.GetByteCount(key.Value ?? string.Empty);
        if (length is < 1 or > MaximumKeyBytes)
            throw new ArgumentException(
                $"Location Store keys must be non-empty UTF-8 values of at most {MaximumKeyBytes} bytes.",
                parameterName
            );
    }

    private static ZLinkStoreKey[] ValidateWriteRequest(ZLinkStoreWriteRequest request)
    {
        var encodedBytes = 0L;
        var conditionKeys = request
            .Conditions.SelectMany(ZLinkStoreBatchKeys.ForCondition)
            .ToArray();
        var mutationKeys = request.Mutations.Select(ZLinkStoreBatchKeys.ForMutation).ToArray();
        if (
            conditionKeys.Distinct().Count() != conditionKeys.Length
            || mutationKeys.Distinct().Count() != mutationKeys.Length
        )
        {
            throw new ArgumentException(
                "A key cannot occur twice in conditions or mutations.",
                nameof(request)
            );
        }
        var uniqueKeys = conditionKeys.Union(mutationKeys).ToArray();
        if (uniqueKeys.Length > MaximumUniqueKeys)
        {
            throw new ArgumentException(
                $"A conditional batch can reference at most {MaximumUniqueKeys} keys.",
                nameof(request)
            );
        }
        foreach (var key in conditionKeys.Concat(mutationKeys))
        {
            ValidateOpaqueKey(key, nameof(request));
            encodedBytes += Encoding.UTF8.GetByteCount(key.Value);
        }
        foreach (var condition in request.Conditions)
        {
            switch (condition)
            {
                case ZLinkStoreCondition.Version version:
                    var length = Encoding.UTF8.GetByteCount(version.Expected.Value ?? string.Empty);
                    if (length is < 1 or > MaximumVersionBytes)
                        throw new ArgumentException(
                            $"Store versions must be non-empty UTF-8 values of at most {MaximumVersionBytes} bytes.",
                            nameof(request)
                        );
                    encodedBytes += length;
                    break;
                case ZLinkStoreCondition.Value value:
                    if (value.Expected.Length > MaximumValueBytes)
                        throw new ArgumentException(
                            "A Location Store value condition exceeds its value bound.",
                            nameof(request)
                        );
                    encodedBytes += value.Expected.Length;
                    break;
                case ZLinkStoreCondition.SumAtMost:
                    encodedBytes += 16;
                    break;
            }
        }
        foreach (var mutation in request.Mutations)
        {
            switch (mutation)
            {
                case ZLinkStoreMutation.Put put:
                    if (put.Bytes.Length > MaximumValueBytes)
                        throw new ArgumentException(
                            $"A Location Store value can contain at most {MaximumValueBytes} bytes.",
                            nameof(request)
                        );
                    if (put.Retention is { } retention && retention <= TimeSpan.Zero)
                        throw new ArgumentException("Retention must be positive.", nameof(request));
                    encodedBytes += put.Bytes.Length;
                    break;
                case ZLinkStoreMutation.Add:
                    encodedBytes += 8;
                    break;
            }
        }
        if (encodedBytes > MaximumEncodedBatchBytes)
            throw new ArgumentException(
                $"The encoded Store batch exceeds {MaximumEncodedBatchBytes} bytes.",
                nameof(request)
            );
        return uniqueKeys;
    }
}
