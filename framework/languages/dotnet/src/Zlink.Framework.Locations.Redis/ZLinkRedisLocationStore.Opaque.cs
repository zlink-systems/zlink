using System.Globalization;
using System.Text;
using StackExchange.Redis;

namespace Zlink.Framework.Locations.Redis;

public sealed partial class ZLinkRedisLocationStore
{
    private const int MaximumKeyBytes = 1024;
    private const int MaximumVersionBytes = 4096;
    private const int MaximumValueBytes = 1024 * 1024;
    private const int MaximumBatchKeys = 2048;
    private const int MaximumEncodedBatchBytes = 4 * 1024 * 1024;

    private const string OpaqueReadScript = """
        if redis.replicate_commands then redis.replicate_commands() end
        local time = redis.call('TIME')
        local nowMs = tonumber(time[1]) * 1000 + math.floor(tonumber(time[2]) / 1000)
        local members = redis.call('ZREVRANGE', KEYS[1], 0, 0)
        if #members == 0 then
            return { 'missing', nowMs }
        end
        local record = cmsgpack.unpack(members[1])
        local expiresAt = tonumber(record[4])
        if record[5] == 1 or (expiresAt >= 0 and expiresAt <= nowMs) then
            return { 'missing', nowMs }
        end
        return {
            'found',
            nowMs,
            record[1],
            record[2],
            record[3],
            expiresAt
        }
        """;

    private const string OpaqueWriteScript = """
        if redis.replicate_commands then redis.replicate_commands() end
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
            128)
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
            'ZRANGEBYSCORE', cleanupKey, '-inf', nowMs, 'LIMIT', 0, 32)
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
                local anchor = redis.call(
                    'ZREVRANGEBYSCORE',
                    recordKey,
                    minimumBoundary,
                    '-inf',
                    'WITHSCORES',
                    'LIMIT',
                    0,
                    1)
                if #anchor == 2 then
                    redis.call(
                        'ZREMRANGEBYSCORE',
                        recordKey,
                        '-inf',
                        '(' .. anchor[2])
                end
                redis.call(
                    'ZADD', cleanupKey, nowMs + 1000, original)
            else
                local record = cmsgpack.unpack(members[1])
                local expiresAt = tonumber(record[4])
                if record[5] == 1
                    or (expiresAt >= 0 and expiresAt + 60000 <= nowMs) then
                    redis.call('DEL', recordKey)
                    redis.call('ZREM', indexKey, original)
                    redis.call('HDEL', mapKey, original)
                    redis.call('ZREM', cleanupKey, original)
                else
                    redis.call('ZREMRANGEBYRANK', recordKey, 0, -2)
                    if expiresAt >= 0 then
                        redis.call(
                            'ZADD',
                            cleanupKey,
                            math.max(nowMs + 1000, expiresAt + 60000),
                            original)
                    else
                        redis.call('ZREM', cleanupKey, original)
                    end
                end
            end
        end

        local arg = 3
        for i = 1, conditionCount do
            local kind = ARGV[arg]
            local expected = ARGV[arg + 1]
            local members = redis.call('ZREVRANGE', KEYS[i], 0, 0)
            local current = nil
            if #members > 0 then
                local record = cmsgpack.unpack(members[1])
                local expiresAt = tonumber(record[4])
                if record[5] ~= 1
                    and (expiresAt < 0 or expiresAt > nowMs) then
                    current = record[3]
                end
            end
            if (kind == 'missing' and current ~= nil)
                or (kind == 'version' and current ~= expected) then
                return { 'conflict', nowMs }
            end
            arg = arg + 2
        end
        local checkArg = arg
        for i = 1, mutationCount do
            local keyIndex = tonumber(ARGV[checkArg])
            if not minimumBoundary then
                redis.call('ZREMRANGEBYRANK', KEYS[keyIndex], 0, -2)
            end
            if redis.call('ZCARD', KEYS[keyIndex]) >= 128 then
                return { 'backlog', nowMs }
            end
            checkArg = checkArg + 6
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
            if kind == 'put' then
                local expiresAt = -1
                if retention >= 0 then expiresAt = nowMs + retention end
                redis.call(
                    'ZADD',
                    redisKey,
                    sequence,
                    cmsgpack.pack({
                        originalKey,
                        value,
                        version,
                        expiresAt,
                        0
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
                    cmsgpack.pack({
                        originalKey,
                        '',
                        '',
                        -1,
                        1
                    }))
                redis.call('ZADD', indexKey, 0, originalKey)
                redis.call('HSET', mapKey, originalKey, redisKey)
            end
            local dueAt = nowMs + 1000
            local scheduled = redis.call(
                'ZSCORE', cleanupKey, originalKey)
            if not scheduled or tonumber(scheduled) > dueAt then
                redis.call('ZADD', cleanupKey, dueAt, originalKey)
            end
            arg = arg + 6
        end

        local result = { 'applied', nowMs }
        for _, item in ipairs(putVersions) do table.insert(result, item) end
        return result
        """;

    private const string OpaqueScanScript = """
        if redis.replicate_commands then redis.replicate_commands() end
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
            128)
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
            'ZRANGEBYSCORE', cleanupKey, '-inf', nowMs, 'LIMIT', 0, 32)
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
                local anchor = redis.call(
                    'ZREVRANGEBYSCORE',
                    recordKey,
                    minimumBoundary,
                    '-inf',
                    'WITHSCORES',
                    'LIMIT',
                    0,
                    1)
                if #anchor == 2 then
                    redis.call(
                        'ZREMRANGEBYSCORE',
                        recordKey,
                        '-inf',
                        '(' .. anchor[2])
                end
                redis.call(
                    'ZADD', cleanupKey, nowMs + 1000, original)
            else
                local record = cmsgpack.unpack(members[1])
                local expiresAt = tonumber(record[4])
                if record[5] == 1
                    or (expiresAt >= 0 and expiresAt + 60000 <= nowMs) then
                    redis.call('DEL', recordKey)
                    redis.call('ZREM', KEYS[1], original)
                    redis.call('HDEL', KEYS[2], original)
                    redis.call('ZREM', cleanupKey, original)
                else
                    redis.call('ZREMRANGEBYRANK', recordKey, 0, -2)
                    if expiresAt >= 0 then
                        redis.call(
                            'ZADD',
                            cleanupKey,
                            math.max(nowMs + 1000, expiresAt + 60000),
                            original)
                    else
                        redis.call('ZREM', cleanupKey, original)
                    end
                end
            end
        end

        if create then
            if redis.call('ZCARD', snapshotExpiryKey) >= 4096 then
                return { 'capacity' }
            end
            redis.call('DEL', snapshot)
            local boundary = tonumber(redis.call('GET', sequenceKey) or '0')
            redis.call(
                'HSET',
                snapshot,
                'now',
                tostring(nowMs),
                'boundary',
                tostring(boundary),
                'prefix',
                prefix)
            redis.call('PEXPIRE', snapshot, 60000)
            redis.call(
                'ZADD', snapshotExpiryKey, nowMs + 60000, snapshotId)
            redis.call(
                'ZADD', snapshotBoundaryKey, boundary, snapshotId)
        elseif redis.call('EXISTS', snapshot) == 0 then
            redis.call('ZREM', snapshotExpiryKey, snapshotId)
            redis.call('ZREM', snapshotBoundaryKey, snapshotId)
            return { 'expired' }
        end

        local metadata = redis.call(
            'HMGET', snapshot, 'now', 'boundary', 'prefix')
        if not metadata[1] or metadata[3] ~= prefix then
            redis.call('ZREM', snapshotExpiryKey, snapshotId)
            redis.call('ZREM', snapshotBoundaryKey, snapshotId)
            return { 'expired' }
        end
        local snapshotNow = tonumber(metadata[1])
        local boundary = tonumber(metadata[2])
        local lower = '-'
        if string.len(lastKey) > 0 then lower = '(' .. lastKey end
        local workLimit = math.max(limit * 4, 128)
        local originals = redis.call(
            'ZRANGEBYLEX', KEYS[1], lower, '+', 'LIMIT', 0, workLimit + 1)
        local emitted = 0
        local encodedBytes = 0
        local examined = 0
        local result = { 'page', tostring(snapshotNow), '' }
        while examined < #originals
            and examined < workLimit
            and emitted < limit do
            local original = originals[examined + 1]
            examined = examined + 1
            if string.sub(original, 1, string.len(prefix)) == prefix then
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
                        local record = cmsgpack.unpack(members[1])
                        local expiresAt = tonumber(record[4])
                        if record[1] == original
                            and record[5] ~= 1
                            and (expiresAt < 0 or expiresAt > snapshotNow) then
                            local itemBytes = string.len(original)
                                + string.len(record[2])
                                + string.len(record[3])
                                + 128
                            if emitted > 0
                                and encodedBytes + itemBytes > 4194304 then
                                examined = examined - 1
                                break
                            end
                            table.insert(result, original)
                            table.insert(result, record[2])
                            table.insert(result, record[3])
                            table.insert(result, tostring(expiresAt))
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
        CancellationToken cancellationToken = default)
    {
        ValidateOpaqueKey(key, nameof(key));
        var result = await ExecuteAsync(
                async database => (RedisResult[])(await database
                    .ScriptEvaluateAsync(
                        OpaqueReadScript,
                        [_keys.OpaqueRecordKey(key.Value)],
                        [])
                    .ConfigureAwait(false))!,
                cancellationToken)
            .ConfigureAwait(false);
        var storeNow = DateTimeOffset.FromUnixTimeMilliseconds(
            (long)result[1]);
        if ((string)result[0]! == "missing")
            return new ZLinkStoreReadResult.Missing(storeNow);
        if (!string.Equals((string)result[2]!, key.Value, StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                "The Redis opaque key digest resolved to a different key.");
        }
        var expiresAtMs = (long)result[5];
        return new ZLinkStoreReadResult.Found(
            new ZLinkStoreValue(
                (byte[])result[3]!,
                new ZLinkStoreVersion((string)result[4]!),
                expiresAtMs >= 0
                    ? DateTimeOffset.FromUnixTimeMilliseconds(expiresAtMs)
                    : null,
                storeNow));
    }

    public async ValueTask<ZLinkStoreWriteResult> WriteAsync(
        ZLinkStoreWriteRequest request,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(request);
        ValidateWriteRequest(request);
        var uniqueKeys = request.Conditions
            .Select(static condition => condition switch
            {
                ZLinkStoreCondition.Missing missing => missing.Key,
                ZLinkStoreCondition.Version version => version.Key,
                _ => throw new ArgumentException(
                    "Unknown Location Store condition.",
                    nameof(request))
            })
            .Concat(request.Mutations.Select(static mutation => mutation switch
            {
                ZLinkStoreMutation.Put put => put.Key,
                ZLinkStoreMutation.Delete delete => delete.Key,
                _ => throw new ArgumentException(
                    "Unknown Location Store mutation.",
                    nameof(request))
            }))
            .Distinct()
            .ToArray();
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
        var args = new List<RedisValue>
        {
            request.Conditions.Count,
            request.Mutations.Count
        };
        foreach (var condition in request.Conditions)
        {
            switch (condition)
            {
                case ZLinkStoreCondition.Missing:
                    args.Add("missing");
                    args.Add(string.Empty);
                    break;
                case ZLinkStoreCondition.Version version:
                    args.Add("version");
                    args.Add(version.Expected.Value);
                    break;
            }
        }
        foreach (var mutation in request.Mutations)
        {
            switch (mutation)
            {
                case ZLinkStoreMutation.Put put:
                    args.Add(keyIndex[put.Key]);
                    args.Add("put");
                    args.Add(put.Key.Value);
                    args.Add(put.Bytes.ToArray());
                    args.Add(Guid.NewGuid().ToString("N"));
                    args.Add(put.Retention is { } retention
                        ? checked((long)Math.Ceiling(
                            retention.TotalMilliseconds))
                        : -1);
                    break;
                case ZLinkStoreMutation.Delete delete:
                    args.Add(keyIndex[delete.Key]);
                    args.Add("delete");
                    args.Add(delete.Key.Value);
                    args.Add(Array.Empty<byte>());
                    args.Add(string.Empty);
                    args.Add(-1);
                    break;
            }
        }

        var result = await ExecuteAsync(
                async database => (RedisResult[])(await database
                    .ScriptEvaluateAsync(
                        OpaqueWriteScript,
                        redisKeys,
                        args.ToArray())
                    .ConfigureAwait(false))!,
                cancellationToken)
            .ConfigureAwait(false);
        var storeNow = DateTimeOffset.FromUnixTimeMilliseconds(
            (long)result[1]);
        var outcome = (string)result[0]!;
        if (outcome == "conflict")
            return new ZLinkStoreWriteResult.Conflict(storeNow);
        if (outcome == "backlog")
        {
            throw new IOException(
                "The Redis Location Store version backlog is full.");
        }
        if (outcome != "applied")
        {
            throw new InvalidDataException(
                "Redis returned an unknown Location Store write result.");
        }
        var versions = new Dictionary<ZLinkStoreKey, ZLinkStoreVersion>();
        for (var index = 2; index < result.Length; index += 2)
        {
            versions[new ZLinkStoreKey((string)result[index]!)] =
                new ZLinkStoreVersion((string)result[index + 1]!);
        }
        return new ZLinkStoreWriteResult.Applied(versions, storeNow);
    }

    public async ValueTask<ZLinkStoreScanResult> ScanAsync(
        ZLinkStoreScanRequest request,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(request);
        if (request.Limit is < 1 or > 1000)
            throw new ArgumentOutOfRangeException(nameof(request));
        var prefix = request.Prefix
                     ?? throw new ArgumentException(
                         "The scan prefix cannot be null.",
                         nameof(request));
        _ = Encoding.UTF8.GetByteCount(prefix) is <= MaximumKeyBytes
            ? 0
            : throw new ArgumentException(
                "The scan prefix exceeds 1024 UTF-8 bytes.",
                nameof(request));
        string scanId;
        var lastKey = string.Empty;
        var create = request.Cursor is null;
        if (request.Cursor is { } cursor)
        {
            var cursorValue = cursor.Value ?? string.Empty;
            var cursorBytes = Encoding.UTF8.GetByteCount(cursorValue);
            if (cursorBytes is < 1 or > MaximumVersionBytes)
                throw new ArgumentException(
                    "Store scan cursors must contain 1..4096 UTF-8 bytes.",
                    nameof(request));
            var separator = cursorValue.LastIndexOf(':');
            if (separator != 32
                || !Guid.TryParseExact(cursorValue[..separator], "N", out _)
                || !TryDecodeCursorKey(
                    cursorValue[(separator + 1)..],
                    out lastKey))
            {
                return new ZLinkStoreScanResult.Expired();
            }
            scanId = cursorValue[..separator];
        }
        else
        {
            scanId = Guid.NewGuid().ToString("N");
        }
        var result = await ExecuteAsync(
                async database => (RedisResult[])(await database
                    .ScriptEvaluateAsync(
                        OpaqueScanScript,
                        [
                            _keys.OpaqueIndexKey(),
                            _keys.OpaqueMapKey(),
                            _keys.OpaqueScanKey(scanId),
                            _keys.OpaqueCleanupKey(),
                            _keys.OpaqueSequenceKey(),
                            _keys.OpaqueSnapshotExpiryKey(),
                            _keys.OpaqueSnapshotBoundaryKey()
                        ],
                        [
                            prefix,
                            lastKey,
                            request.Limit,
                            create ? 1 : 0,
                            scanId
                        ])
                    .ConfigureAwait(false))!,
                cancellationToken)
            .ConfigureAwait(false);
        var outcome = (string)result[0]!;
        if (outcome == "expired")
            return new ZLinkStoreScanResult.Expired();
        if (outcome == "capacity")
        {
            throw new IOException(
                "The Redis Location Store snapshot capacity is full.");
        }
        if (outcome != "page")
        {
            throw new InvalidDataException(
                "Redis returned an unknown Location Store scan result.");
        }

        var storeNow = DateTimeOffset.FromUnixTimeMilliseconds(
            long.Parse(
                (string)result[1]!,
                NumberStyles.None,
                CultureInfo.InvariantCulture));
        var nextKey = (string)result[2]!;
        var items = new List<KeyValuePair<ZLinkStoreKey, ZLinkStoreValue>>();
        for (var index = 3; index < result.Length; index += 4)
        {
            var expiresAtMs = long.Parse(
                (string)result[index + 3]!,
                NumberStyles.AllowLeadingSign,
                CultureInfo.InvariantCulture);
            items.Add(new KeyValuePair<ZLinkStoreKey, ZLinkStoreValue>(
                new ZLinkStoreKey((string)result[index]!),
                new ZLinkStoreValue(
                    (byte[])result[index + 1]!,
                    new ZLinkStoreVersion((string)result[index + 2]!),
                    expiresAtMs >= 0
                        ? DateTimeOffset.FromUnixTimeMilliseconds(expiresAtMs)
                        : null,
                    storeNow)));
        }
        return new ZLinkStoreScanResult.Page(
            new ZLinkStoreScanPage(
                items,
                nextKey.Length > 0
                    ? new ZLinkStoreScanCursor(
                        $"{scanId}:{Convert.ToHexString(
                            Encoding.UTF8.GetBytes(nextKey))}")
                    : null,
                storeNow));
    }

    private static bool TryDecodeCursorKey(
        string encoded,
        out string key)
    {
        key = string.Empty;
        if (encoded.Length is < 2 or > MaximumKeyBytes * 2
            || encoded.Length % 2 != 0)
            return false;
        try
        {
            key = new UTF8Encoding(
                    encoderShouldEmitUTF8Identifier: false,
                    throwOnInvalidBytes: true)
                .GetString(Convert.FromHexString(encoded));
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

    private static void ValidateOpaqueKey(
        ZLinkStoreKey key,
        string parameterName)
    {
        var length = Encoding.UTF8.GetByteCount(key.Value ?? string.Empty);
        if (length is < 1 or > MaximumKeyBytes)
            throw new ArgumentException(
                "Location Store keys must contain 1..1024 UTF-8 bytes.",
                parameterName);
    }

    private static void ValidateWriteRequest(ZLinkStoreWriteRequest request)
    {
        var encodedBytes = 0L;
        var conditionKeys = request.Conditions.Select(
            static condition => condition switch
            {
                ZLinkStoreCondition.Missing missing => missing.Key,
                ZLinkStoreCondition.Version version => version.Key,
                _ => throw new ArgumentException(
                    "Unknown Location Store condition.")
            }).ToArray();
        var mutationKeys = request.Mutations.Select(
            static mutation => mutation switch
            {
                ZLinkStoreMutation.Put put => put.Key,
                ZLinkStoreMutation.Delete delete => delete.Key,
                _ => throw new ArgumentException(
                    "Unknown Location Store mutation.")
            }).ToArray();
        if (conditionKeys.Distinct().Count() != conditionKeys.Length
            || mutationKeys.Distinct().Count() != mutationKeys.Length)
        {
            throw new ArgumentException(
                "A key cannot occur twice in conditions or mutations.",
                nameof(request));
        }
        if (conditionKeys.Concat(mutationKeys).Distinct().Count()
            > MaximumBatchKeys)
        {
            throw new ArgumentException(
                "A conditional batch can reference at most 2048 keys.",
                nameof(request));
        }
        foreach (var condition in request.Conditions)
        {
            switch (condition)
            {
                case ZLinkStoreCondition.Missing missing:
                    ValidateOpaqueKey(missing.Key, nameof(request));
                    encodedBytes += Encoding.UTF8.GetByteCount(
                        missing.Key.Value);
                    break;
                case ZLinkStoreCondition.Version version:
                    ValidateOpaqueKey(version.Key, nameof(request));
                    encodedBytes += Encoding.UTF8.GetByteCount(
                        version.Key.Value);
                    var length = Encoding.UTF8.GetByteCount(
                        version.Expected.Value ?? string.Empty);
                    if (length is < 1 or > MaximumVersionBytes)
                        throw new ArgumentException(
                            "Store versions must contain 1..4096 UTF-8 bytes.",
                            nameof(request));
                    encodedBytes += length;
                    break;
            }
        }
        foreach (var mutation in request.Mutations)
        {
            switch (mutation)
            {
                case ZLinkStoreMutation.Put put:
                    ValidateOpaqueKey(put.Key, nameof(request));
                    encodedBytes += Encoding.UTF8.GetByteCount(put.Key.Value);
                    if (put.Bytes.Length > MaximumValueBytes)
                        throw new ArgumentException(
                            "A Location Store value can contain at most 1 MiB.",
                            nameof(request));
                    if (put.Retention is { } retention
                        && retention <= TimeSpan.Zero)
                        throw new ArgumentException(
                            "Retention must be positive.",
                            nameof(request));
                    encodedBytes += put.Bytes.Length;
                    break;
                case ZLinkStoreMutation.Delete delete:
                    ValidateOpaqueKey(delete.Key, nameof(request));
                    encodedBytes += Encoding.UTF8.GetByteCount(
                        delete.Key.Value);
                    break;
            }
        }
        if (encodedBytes > MaximumEncodedBatchBytes)
            throw new ArgumentException(
                "The encoded Store batch exceeds 4 MiB.",
                nameof(request));
    }
}
