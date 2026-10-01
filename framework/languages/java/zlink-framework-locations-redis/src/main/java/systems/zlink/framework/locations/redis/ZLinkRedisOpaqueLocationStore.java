package systems.zlink.framework.locations.redis;

import io.lettuce.core.ScriptOutputType;

import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locationprovider.ZLinkStoreCondition;
import systems.zlink.framework.locationprovider.ZLinkStoreDelete;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStoreMissingCondition;
import systems.zlink.framework.locationprovider.ZLinkStoreMutation;
import systems.zlink.framework.locationprovider.ZLinkStorePut;
import systems.zlink.framework.locationprovider.ZLinkStoreReadFound;
import systems.zlink.framework.locationprovider.ZLinkStoreReadMissing;
import systems.zlink.framework.locationprovider.ZLinkStoreReadResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanCursor;
import systems.zlink.framework.locationprovider.ZLinkStoreScanExpired;
import systems.zlink.framework.locationprovider.ZLinkStoreScanItem;
import systems.zlink.framework.locationprovider.ZLinkStoreScanPage;
import systems.zlink.framework.locationprovider.ZLinkStoreScanPageResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreScanResult;
import systems.zlink.framework.locationprovider.ZLinkStoreValue;
import systems.zlink.framework.locationprovider.ZLinkStoreValueCondition;
import systems.zlink.framework.locationprovider.ZLinkStoreVersion;
import systems.zlink.framework.locationprovider.ZLinkStoreVersionCondition;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteApplied;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteConflict;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteResult;
import systems.zlink.framework.runtime.locations.ZLinkStoreRetention;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.HexFormat;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;

/**
 * Redis implementation of the opaque provider operations.
 *
 * <p>The version history retained by each key lets a scan keep one revision boundary instead of
 * copying every matching value into a snapshot.
 *
 * <p>Values are carried end to end as 8-bit-clean bytes: the connection uses a byte[] Redis value
 * codec (see {@link ZLinkRedisStringByteArrayCodec}), so caller-supplied record bytes reach {@code
 * cmsgpack.pack} through {@code EVAL} ARGV with no base64 sub-layer. Every stored ZSET member is
 * tagged with a leading {@code 0x01} format byte followed by a 5-element cmsgpack array {@code
 * [originalKey, value, version, expiresAtMs, tombstone]} per 21-location-runtime.md#2.4 /
 * 22-location-store-redis.md#7 and the store-record-v1 golden fixture. {@code expiresAtMs == 0} is
 * the "never expires" sentinel (a real epoch millisecond timestamp is never zero); {@code
 * tombstone} is a genuine msgpack boolean, not an integer flag. An unrecognized leading tag byte is
 * a hard failure (clean break, no read-old).
 */
final class ZLinkRedisOpaqueLocationStore implements ZLinkLocationStore {
    private static final int MAXIMUM_KEY_BYTES = 1024;
    private static final int MAXIMUM_VERSION_BYTES = 4096;
    private static final int MAXIMUM_VALUE_BYTES = 1024 * 1024;
    private static final int MAXIMUM_BATCH_KEYS = 2048;
    private static final int MAXIMUM_ENCODED_BATCH_BYTES = 4 * 1024 * 1024;

    private static final int MAXIMUM_SCAN_ITEMS = 1000;
    private static final int MAXIMUM_ENCODED_PAGE_BYTES = 4 * 1024 * 1024;
    private static final long VERSION_CLEANUP_GRACE_MILLIS = 60_000;
    private static final long SNAPSHOT_RETENTION_MILLIS = 60_000;
    private static final long CLEANUP_DELAY_MILLIS = 1000;
    private static final int EXPIRED_SNAPSHOT_CLEANUP_BATCH = 128;
    private static final int VERSION_CLEANUP_BATCH = 32;
    private static final int MAXIMUM_VERSION_HISTORY = 128;
    private static final int MAXIMUM_ACTIVE_SNAPSHOTS = 4096;
    private static final int SCAN_WORK_MULTIPLIER = 4;
    private static final int MINIMUM_SCAN_WORK = 128;
    private static final int ENCODED_ITEM_OVERHEAD_BYTES = 128;

    private enum ScriptToken {
        MISSING("missing"),
        FOUND("found"),
        CONFLICT("conflict"),
        BACKLOG("backlog"),
        APPLIED("applied"),
        EXPIRED("expired"),
        CAPACITY("capacity"),
        PAGE("page"),
        VALUE("value"),
        VERSION("version"),
        PUT("put"),
        DELETE("delete");

        private static final java.util.Map<String, ScriptToken> BY_WIRE =
                java.util.Arrays.stream(values())
                        .collect(
                                java.util.stream.Collectors.toUnmodifiableMap(
                                        token -> token.wire, token -> token));
        private final String wire;

        ScriptToken(String wire) {
            this.wire = wire;
        }

        static ScriptToken decode(Object value) {
            return BY_WIRE.get(text(value));
        }
    }

    private static final String UNPACK_TAGGED_HELPER =
            """
            local function unpackTagged(raw)
                if string.byte(raw, 1) ~= 1 then
                    error(
                        'zlink-opaque-record-tag: unsupported store record '
                            .. 'format tag')
                end
                return cmsgpack.unpack(string.sub(raw, 2))
            end
            """;

    private static final String READ_SCRIPT =
            script(
                    UNPACK_TAGGED_HELPER
                            + """
                    if redis.replicate_commands then redis.replicate_commands() end
                    local time = redis.call('TIME')
                    local nowMs = tonumber(time[1]) * 1000
                        + math.floor(tonumber(time[2]) / 1000)
                    local members = redis.call('ZREVRANGE', KEYS[1], 0, 0)
                    if #members == 0 then return { '${MISSING}', nowMs } end
                    local record = unpackTagged(members[1])
                    local expiresAt = tonumber(record[4])
                    if record[5] == true
                        or (expiresAt > 0 and expiresAt <= nowMs) then
                        return { '${MISSING}', nowMs }
                    end
                    return {
                        '${FOUND}', nowMs, record[1], record[2], record[3], expiresAt
                    }
                    """);

    private static final String WRITE_SCRIPT =
            script(
                    UNPACK_TAGGED_HELPER
                            + """
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
                        'ZRANGEBYSCORE', snapshotExpiryKey, '-inf', nowMs,
                        'LIMIT', 0, ${EXPIRED_SNAPSHOT_CLEANUP_BATCH})
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
                        'ZRANGEBYSCORE', cleanupKey, '-inf', nowMs,
                        'LIMIT', 0, ${VERSION_CLEANUP_BATCH})
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
                                'ZREVRANGEBYSCORE', recordKey,
                                minimumBoundary, '-inf', 'WITHSCORES',
                                'LIMIT', 0, 1)
                            if #anchor == 2 then
                                redis.call(
                                    'ZREMRANGEBYSCORE',
                                    recordKey, '-inf', '(' .. anchor[2])
                            end
                            redis.call('ZADD', cleanupKey, nowMs + ${CLEANUP_DELAY_MILLIS}, original)
                        else
                            local record = unpackTagged(members[1])
                            local expiresAt = tonumber(record[4])
                            if record[5] == true
                                or (expiresAt > 0 and expiresAt + ${VERSION_CLEANUP_GRACE_MILLIS} <= nowMs) then
                                redis.call('DEL', recordKey)
                                redis.call('ZREM', indexKey, original)
                                redis.call('HDEL', mapKey, original)
                                redis.call('ZREM', cleanupKey, original)
                            else
                                redis.call('ZREMRANGEBYRANK', recordKey, 0, -2)
                                if expiresAt > 0 then
                                    redis.call(
                                        'ZADD', cleanupKey,
                                        math.max(nowMs + ${CLEANUP_DELAY_MILLIS}, expiresAt + ${VERSION_CLEANUP_GRACE_MILLIS}),
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
                        local currentValue = nil
                        if #members > 0 then
                            local record = unpackTagged(members[1])
                            local expiresAt = tonumber(record[4])
                            if record[5] ~= true
                                and (expiresAt == 0 or expiresAt > nowMs) then
                                current = record[3]
                                currentValue = record[2]
                            end
                        end
                        if (kind == '${MISSING}' and current ~= nil)
                            or (kind == '${VERSION}' and current ~= expected)
                            or (kind == '${VALUE}' and currentValue ~= expected) then
                            return { '${CONFLICT}', nowMs }
                        end
                        arg = arg + 2
                    end

                    local checkArg = arg
                    for i = 1, mutationCount do
                        local keyIndex = tonumber(ARGV[checkArg])
                        if redis.call('ZCARD', KEYS[keyIndex]) >= ${MAXIMUM_VERSION_HISTORY} then
                            return { '${BACKLOG}', nowMs }
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
                        if kind == '${PUT}' then
                            local expiresAt = 0
                            if retention >= 0 then expiresAt = nowMs + retention end
                            redis.call(
                                'ZADD', redisKey, sequence,
                                '\\1' .. cmsgpack.pack({
                                    originalKey, value, version, expiresAt, false
                                }))
                            redis.call('ZADD', indexKey, 0, originalKey)
                            redis.call('HSET', mapKey, originalKey, redisKey)
                            table.insert(putVersions, originalKey)
                            table.insert(putVersions, version)
                        else
                            redis.call(
                                'ZADD', redisKey, sequence,
                                '\\1' .. cmsgpack.pack({
                                    originalKey, '', '', 0, true
                                }))
                            redis.call('ZADD', indexKey, 0, originalKey)
                            redis.call('HSET', mapKey, originalKey, redisKey)
                        end
                        local dueAt = nowMs + ${CLEANUP_DELAY_MILLIS}
                        local scheduled = redis.call('ZSCORE', cleanupKey, originalKey)
                        if not scheduled or tonumber(scheduled) > dueAt then
                            redis.call('ZADD', cleanupKey, dueAt, originalKey)
                        end
                        arg = arg + 6
                    end

                    local result = { '${APPLIED}', nowMs }
                    for _, item in ipairs(putVersions) do
                        table.insert(result, item)
                    end
                    return result
                    """);

    private static final String SCAN_SCRIPT =
            script(
                    UNPACK_TAGGED_HELPER
                            + """
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
                        'ZRANGEBYSCORE', snapshotExpiryKey, '-inf', nowMs,
                        'LIMIT', 0, ${EXPIRED_SNAPSHOT_CLEANUP_BATCH})
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
                        'ZRANGEBYSCORE', cleanupKey, '-inf', nowMs,
                        'LIMIT', 0, ${VERSION_CLEANUP_BATCH})
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
                                'ZREVRANGEBYSCORE', recordKey,
                                minimumBoundary, '-inf', 'WITHSCORES',
                                'LIMIT', 0, 1)
                            if #anchor == 2 then
                                redis.call(
                                    'ZREMRANGEBYSCORE',
                                    recordKey, '-inf', '(' .. anchor[2])
                            end
                            redis.call('ZADD', cleanupKey, nowMs + ${CLEANUP_DELAY_MILLIS}, original)
                        else
                            local record = unpackTagged(members[1])
                            local expiresAt = tonumber(record[4])
                            if record[5] == true
                                or (expiresAt > 0 and expiresAt + ${VERSION_CLEANUP_GRACE_MILLIS} <= nowMs) then
                                redis.call('DEL', recordKey)
                                redis.call('ZREM', KEYS[1], original)
                                redis.call('HDEL', KEYS[2], original)
                                redis.call('ZREM', cleanupKey, original)
                            else
                                redis.call('ZREMRANGEBYRANK', recordKey, 0, -2)
                                if expiresAt > 0 then
                                    redis.call(
                                        'ZADD', cleanupKey,
                                        math.max(nowMs + ${CLEANUP_DELAY_MILLIS}, expiresAt + ${VERSION_CLEANUP_GRACE_MILLIS}),
                                        original)
                                else
                                    redis.call('ZREM', cleanupKey, original)
                                end
                            end
                        end
                    end

                    if create then
                        if redis.call('ZCARD', snapshotExpiryKey) >= ${MAXIMUM_ACTIVE_SNAPSHOTS} then
                            return { '${CAPACITY}' }
                        end
                        redis.call('DEL', snapshot)
                        local boundary = tonumber(redis.call('GET', sequenceKey) or '0')
                        redis.call(
                            'HSET', snapshot,
                            'now', tostring(nowMs),
                            'boundary', tostring(boundary),
                            'prefix', prefix)
                        redis.call('PEXPIRE', snapshot, ${SNAPSHOT_RETENTION_MILLIS})
                        redis.call(
                            'ZADD', snapshotExpiryKey, nowMs + ${SNAPSHOT_RETENTION_MILLIS}, snapshotId)
                        redis.call(
                            'ZADD', snapshotBoundaryKey, boundary, snapshotId)
                    elseif redis.call('EXISTS', snapshot) == 0 then
                        redis.call('ZREM', snapshotExpiryKey, snapshotId)
                        redis.call('ZREM', snapshotBoundaryKey, snapshotId)
                        return { '${EXPIRED}' }
                    end

                    local metadata = redis.call(
                        'HMGET', snapshot, 'now', 'boundary', 'prefix')
                    if not metadata[1] or metadata[3] ~= prefix then
                        redis.call('ZREM', snapshotExpiryKey, snapshotId)
                        redis.call('ZREM', snapshotBoundaryKey, snapshotId)
                        return { '${EXPIRED}' }
                    end
                    local snapshotNow = tonumber(metadata[1])
                    local boundary = tonumber(metadata[2])
                    local lower = '-'
                    if string.len(lastKey) > 0 then lower = '(' .. lastKey end
                    local workLimit = math.max(limit * ${SCAN_WORK_MULTIPLIER}, ${MINIMUM_SCAN_WORK})
                    local originals = redis.call(
                        'ZRANGEBYLEX', KEYS[1], lower, '+',
                        'LIMIT', 0, workLimit + 1)
                    local emitted = 0
                    local encodedBytes = 0
                    local examined = 0
                    local result = { '${PAGE}', tostring(snapshotNow), '' }
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
                                    recordKey, boundary, '-inf',
                                    'LIMIT', 0, 1)
                                if #members > 0 then
                                    local record = unpackTagged(members[1])
                                    local expiresAt = tonumber(record[4])
                                    if record[1] == original
                                        and record[5] ~= true
                                        and (expiresAt == 0 or expiresAt > snapshotNow) then
                                        local itemBytes = string.len(original)
                                            + string.len(record[2])
                                            + string.len(record[3]) + ${ENCODED_ITEM_OVERHEAD_BYTES}
                                        if emitted > 0
                                            and encodedBytes + itemBytes > ${MAXIMUM_ENCODED_PAGE_BYTES} then
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
                    """);

    private final ZLinkRedisLocationConnection<byte[]> connection;
    private final ZLinkRedisLocationKeys keys;

    ZLinkRedisOpaqueLocationStore(
            ZLinkRedisLocationConnection<byte[]> connection, ZLinkRedisLocationKeys keys) {
        this.connection = Objects.requireNonNull(connection, "connection");
        this.keys = Objects.requireNonNull(keys, "keys");
    }

    @Override
    public CompletionStage<ZLinkStoreReadResult> read(
            ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
        validateKey(key);
        if (cancelled(cancellation)) {
            return cancelledStage();
        }
        return connection
                .commands()
                .thenCompose(
                        commands ->
                                commands.<List<Object>>eval(
                                        READ_SCRIPT,
                                        ScriptOutputType.MULTI,
                                        new String[] {keys.opaqueRecordKey(key.value())}))
                .thenApply(result -> decodeRead(key, result));
    }

    @Override
    public CompletionStage<ZLinkStoreWriteResult> write(
            ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
        ValidatedWrite validated = validateWrite(request);
        if (cancelled(cancellation)) {
            return cancelledStage();
        }
        return connection
                .commands()
                .thenCompose(
                        commands ->
                                commands.<List<Object>>eval(
                                        WRITE_SCRIPT,
                                        ScriptOutputType.MULTI,
                                        validated.redisKeys(),
                                        validated.arguments()))
                .thenApply(this::decodeWrite);
    }

    @Override
    public CompletionStage<ZLinkStoreScanResult> scan(
            ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
        ValidatedScan validated = validateScan(request);
        if (validated.expired()) {
            return CompletableFuture.completedFuture(new ZLinkStoreScanExpired());
        }
        if (cancelled(cancellation)) {
            return cancelledStage();
        }
        return connection
                .commands()
                .thenCompose(
                        commands ->
                                commands.<List<Object>>eval(
                                        SCAN_SCRIPT,
                                        ScriptOutputType.MULTI,
                                        new String[] {
                                            keys.opaqueIndexKey(),
                                            keys.opaqueMapKey(),
                                            keys.opaqueScanKey(validated.scanId()),
                                            keys.opaqueCleanupKey(),
                                            keys.opaqueSequenceKey(),
                                            keys.opaqueSnapshotExpiryKey(),
                                            keys.opaqueSnapshotBoundaryKey()
                                        },
                                        bytes(validated.prefix()),
                                        bytes(validated.lastKey()),
                                        bytes(Integer.toString(validated.limit())),
                                        bytes(validated.create() ? "1" : "0"),
                                        bytes(validated.scanId())))
                .thenApply(result -> decodeScan(validated.scanId(), result));
    }

    private ZLinkStoreReadResult decodeRead(ZLinkStoreKey expectedKey, List<Object> result) {
        ScriptToken outcome = ScriptToken.decode(result.getFirst());
        Instant storeNow = instant(result.get(1));
        if (outcome == ScriptToken.MISSING) {
            return new ZLinkStoreReadMissing(storeNow);
        }
        requireOutcome(ScriptToken.FOUND, outcome, "read", result.getFirst());
        if (!expectedKey.value().equals(text(result.get(2)))) {
            throw new IllegalStateException("Redis opaque key digest resolved to a different key.");
        }
        long expiresAt = number(result.get(5));
        return new ZLinkStoreReadFound(
                new ZLinkStoreValue(
                        rawBytes(result.get(3)),
                        new ZLinkStoreVersion(text(result.get(4))),
                        expiresAt > 0 ? Instant.ofEpochMilli(expiresAt) : null,
                        storeNow));
    }

    private ZLinkStoreWriteResult decodeWrite(List<Object> result) {
        ScriptToken outcome = ScriptToken.decode(result.getFirst());
        Instant storeNow = instant(result.get(1));
        if (outcome == ScriptToken.CONFLICT) {
            return new ZLinkStoreWriteConflict(storeNow);
        }
        if (outcome == ScriptToken.BACKLOG) {
            throw new CompletionException(
                    new IOException("Redis Location Store version backlog is full."));
        }
        requireOutcome(ScriptToken.APPLIED, outcome, "write", result.getFirst());
        Map<ZLinkStoreKey, ZLinkStoreVersion> versions = new LinkedHashMap<>();
        for (int index = 2; index < result.size(); index += 2) {
            versions.put(
                    new ZLinkStoreKey(text(result.get(index))),
                    new ZLinkStoreVersion(text(result.get(index + 1))));
        }
        return new ZLinkStoreWriteApplied(Map.copyOf(versions), storeNow);
    }

    private ZLinkStoreScanResult decodeScan(String scanId, List<Object> result) {
        ScriptToken outcome = ScriptToken.decode(result.getFirst());
        if (outcome == ScriptToken.EXPIRED) {
            return new ZLinkStoreScanExpired();
        }
        if (outcome == ScriptToken.CAPACITY) {
            throw new CompletionException(
                    new IOException("Redis Location Store snapshot capacity is full."));
        }
        requireOutcome(ScriptToken.PAGE, outcome, "scan", result.getFirst());
        Instant storeNow = instant(result.get(1));
        String nextKey = text(result.get(2));
        List<ZLinkStoreScanItem> items = new ArrayList<>();
        for (int index = 3; index < result.size(); index += 4) {
            long expiresAt = number(result.get(index + 3));
            items.add(
                    new ZLinkStoreScanItem(
                            new ZLinkStoreKey(text(result.get(index))),
                            new ZLinkStoreValue(
                                    rawBytes(result.get(index + 1)),
                                    new ZLinkStoreVersion(text(result.get(index + 2))),
                                    expiresAt > 0 ? Instant.ofEpochMilli(expiresAt) : null,
                                    storeNow)));
        }
        ZLinkStoreScanCursor next =
                nextKey.isEmpty()
                        ? null
                        : new ZLinkStoreScanCursor(
                                scanId
                                        + ":"
                                        + HexFormat.of()
                                                .formatHex(
                                                        nextKey.getBytes(StandardCharsets.UTF_8)));
        return new ZLinkStoreScanPageResult(
                new ZLinkStoreScanPage(List.copyOf(items), next, storeNow));
    }

    private ValidatedWrite validateWrite(ZLinkStoreWriteRequest request) {
        Objects.requireNonNull(request, "request");
        List<ZLinkStoreCondition> conditions =
                List.copyOf(Objects.requireNonNull(request.conditions(), "request.conditions"));
        List<ZLinkStoreMutation> mutations =
                List.copyOf(Objects.requireNonNull(request.mutations(), "request.mutations"));
        Set<ZLinkStoreKey> conditionKeys = new HashSet<>();
        Set<ZLinkStoreKey> mutationKeys = new HashSet<>();
        Set<ZLinkStoreKey> uniqueKeys = new LinkedHashSet<>();
        long encodedBytes = 0;
        for (ZLinkStoreCondition condition : conditions) {
            ZLinkStoreKey key;
            if (condition instanceof ZLinkStoreMissingCondition missing) {
                key = missing.key();
            } else if (condition instanceof ZLinkStoreVersionCondition version) {
                key = version.key();
                encodedBytes += validateVersion(version.expected());
            } else if (condition instanceof ZLinkStoreValueCondition value) {
                key = value.key();
                byte[] expected = Objects.requireNonNull(value.expected(), "value.expected");
                if (expected.length > MAXIMUM_VALUE_BYTES) {
                    throw new IllegalArgumentException(
                            "A Location Store value can contain at most 1 MiB.");
                }
                encodedBytes += expected.length;
            } else {
                throw new IllegalArgumentException("Unknown Location Store condition.");
            }
            encodedBytes += validateKey(key);
            if (!conditionKeys.add(key)) {
                throw new IllegalArgumentException("A key cannot occur twice in conditions.");
            }
            uniqueKeys.add(key);
        }
        for (ZLinkStoreMutation mutation : mutations) {
            ZLinkStoreKey key;
            if (mutation instanceof ZLinkStorePut put) {
                key = put.key();
                byte[] bytes = put.bytes();
                if (bytes.length > MAXIMUM_VALUE_BYTES) {
                    throw new IllegalArgumentException(
                            "A Location Store value can contain at most 1 MiB.");
                }
                Duration retention = put.retention();
                if (retention != null && (retention.isZero() || retention.isNegative())) {
                    throw new IllegalArgumentException("Retention must be positive.");
                }
                encodedBytes += bytes.length;
            } else if (mutation instanceof ZLinkStoreDelete delete) {
                key = delete.key();
            } else {
                throw new IllegalArgumentException("Unknown Location Store mutation.");
            }
            encodedBytes += validateKey(key);
            if (!mutationKeys.add(key)) {
                throw new IllegalArgumentException("A key cannot occur twice in mutations.");
            }
            uniqueKeys.add(key);
        }
        if (uniqueKeys.size() > MAXIMUM_BATCH_KEYS) {
            throw new IllegalArgumentException(
                    "A conditional batch can reference at most " + MAXIMUM_BATCH_KEYS + " keys.");
        }
        if (encodedBytes > MAXIMUM_ENCODED_BATCH_BYTES) {
            throw new IllegalArgumentException(
                    "The encoded Store batch exceeds "
                            + (MAXIMUM_ENCODED_BATCH_BYTES / (1024 * 1024))
                            + " MiB.");
        }

        List<ZLinkStoreKey> orderedKeys = new ArrayList<>(uniqueKeys);
        Map<ZLinkStoreKey, Integer> keyIndexes = new HashMap<>();
        String[] redisKeys = new String[orderedKeys.size() + 6];
        for (int index = 0; index < orderedKeys.size(); index++) {
            ZLinkStoreKey key = orderedKeys.get(index);
            keyIndexes.put(key, index + 1);
            redisKeys[index] = keys.opaqueRecordKey(key.value());
        }
        int tail = orderedKeys.size();
        redisKeys[tail] = keys.opaqueIndexKey();
        redisKeys[tail + 1] = keys.opaqueMapKey();
        redisKeys[tail + 2] = keys.opaqueCleanupKey();
        redisKeys[tail + 3] = keys.opaqueSequenceKey();
        redisKeys[tail + 4] = keys.opaqueSnapshotExpiryKey();
        redisKeys[tail + 5] = keys.opaqueSnapshotBoundaryKey();

        List<byte[]> arguments = new ArrayList<>();
        arguments.add(bytes(Integer.toString(conditions.size())));
        arguments.add(bytes(Integer.toString(mutations.size())));
        for (ZLinkStoreCondition condition : conditions) {
            if (condition instanceof ZLinkStoreMissingCondition) {
                arguments.add(bytes(ScriptToken.MISSING.wire));
                arguments.add(bytes(""));
            } else if (condition instanceof ZLinkStoreValueCondition value) {
                arguments.add(bytes(ScriptToken.VALUE.wire));
                arguments.add(value.expected());
            } else {
                ZLinkStoreVersionCondition version = (ZLinkStoreVersionCondition) condition;
                arguments.add(bytes(ScriptToken.VERSION.wire));
                arguments.add(bytes(version.expected().value()));
            }
        }
        for (ZLinkStoreMutation mutation : mutations) {
            ZLinkStoreKey key =
                    mutation instanceof ZLinkStorePut put
                            ? put.key()
                            : ((ZLinkStoreDelete) mutation).key();
            arguments.add(bytes(Integer.toString(keyIndexes.get(key))));
            if (mutation instanceof ZLinkStorePut put) {
                arguments.add(bytes(ScriptToken.PUT.wire));
                arguments.add(bytes(key.value()));
                arguments.add(put.bytes());
                arguments.add(bytes(uuidHex()));
                arguments.add(
                        bytes(
                                put.retention() == null
                                        ? "-1"
                                        : Long.toString(
                                                ZLinkStoreRetention.toMillis(put.retention()))));
            } else {
                arguments.add(bytes(ScriptToken.DELETE.wire));
                arguments.add(bytes(key.value()));
                arguments.add(bytes(""));
                arguments.add(bytes(""));
                arguments.add(bytes("-1"));
            }
        }
        return new ValidatedWrite(redisKeys, arguments.toArray(byte[][]::new));
    }

    private static ValidatedScan validateScan(ZLinkStoreScanRequest request) {
        Objects.requireNonNull(request, "request");
        String prefix = Objects.requireNonNull(request.prefix(), "request.prefix");
        if (utf8Length(prefix) > MAXIMUM_KEY_BYTES) {
            throw new IllegalArgumentException(
                    "The scan prefix exceeds " + MAXIMUM_KEY_BYTES + " UTF-8 bytes.");
        }
        if (request.limit() < 1 || request.limit() > MAXIMUM_SCAN_ITEMS) {
            throw new IllegalArgumentException(
                    "Scan limit must be in the range 1.." + MAXIMUM_SCAN_ITEMS + ".");
        }
        if (request.cursor() == null) {
            return new ValidatedScan(prefix, uuidHex(), "", request.limit(), true, false);
        }
        String cursor = Objects.requireNonNull(request.cursor().value(), "request.cursor.value");
        int cursorBytes = utf8Length(cursor);
        int separator = cursor.lastIndexOf(':');
        if (cursorBytes < 1 || cursorBytes > MAXIMUM_VERSION_BYTES || separator != 32) {
            return ValidatedScan.expired(prefix, request.limit());
        }
        String scanId = cursor.substring(0, separator);
        if (!scanId.matches("[0-9a-f]{32}")) {
            return ValidatedScan.expired(prefix, request.limit());
        }
        String lastKey = decodeCursorKey(cursor.substring(separator + 1));
        if (lastKey == null) {
            return ValidatedScan.expired(prefix, request.limit());
        }
        return new ValidatedScan(prefix, scanId, lastKey, request.limit(), false, false);
    }

    private static String decodeCursorKey(String encoded) {
        if (encoded.length() < 2
                || encoded.length() > MAXIMUM_KEY_BYTES * 2
                || (encoded.length() & 1) != 0) {
            return null;
        }
        try {
            byte[] bytes = HexFormat.of().parseHex(encoded);
            String key =
                    StandardCharsets.UTF_8
                            .newDecoder()
                            .onMalformedInput(CodingErrorAction.REPORT)
                            .onUnmappableCharacter(CodingErrorAction.REPORT)
                            .decode(ByteBuffer.wrap(bytes))
                            .toString();
            return utf8Length(key) <= MAXIMUM_KEY_BYTES ? key : null;
        } catch (IllegalArgumentException | CharacterCodingException error) {
            return null;
        }
    }

    private static int validateKey(ZLinkStoreKey key) {
        Objects.requireNonNull(key, "key");
        int length = utf8Length(Objects.requireNonNull(key.value(), "key.value"));
        if (length < 1 || length > MAXIMUM_KEY_BYTES) {
            throw new IllegalArgumentException(
                    "Location Store keys must contain 1.." + MAXIMUM_KEY_BYTES + " UTF-8 bytes.");
        }
        return length;
    }

    private static int validateVersion(ZLinkStoreVersion version) {
        Objects.requireNonNull(version, "version");
        int length = utf8Length(Objects.requireNonNull(version.value(), "version.value"));
        if (length < 1 || length > MAXIMUM_VERSION_BYTES) {
            throw new IllegalArgumentException(
                    "Store versions must contain 1.." + MAXIMUM_VERSION_BYTES + " UTF-8 bytes.");
        }
        return length;
    }

    private static int utf8Length(String value) {
        return value.getBytes(StandardCharsets.UTF_8).length;
    }

    private static boolean cancelled(ZLinkStoreCancellation cancellation) {
        return cancellation != null && cancellation.isCancellationRequested();
    }

    private static <T> CompletionStage<T> cancelledStage() {
        return CompletableFuture.failedFuture(
                new CancellationException("Location Store operation was cancelled."));
    }

    private static byte[] bytes(String value) {
        return value.getBytes(StandardCharsets.UTF_8);
    }

    private static byte[] rawBytes(Object value) {
        if (value instanceof byte[] raw) {
            return raw;
        }
        if (value == null) {
            return new byte[0];
        }
        return bytes(String.valueOf(value));
    }

    private static String text(Object value) {
        if (value instanceof byte[] raw) {
            return new String(raw, StandardCharsets.UTF_8);
        }
        return String.valueOf(value);
    }

    private static long number(Object value) {
        if (value instanceof Number number) {
            return number.longValue();
        }
        // Full-range u64 decimal strings (bit 63 set) are legal wire
        // values; Long.parseLong rejects them. Signed values (e.g. "-1"
        // no-retention sentinels) keep the signed parse.
        String text = text(value);
        return text.startsWith("-") ? Long.parseLong(text) : Long.parseUnsignedLong(text);
    }

    private static Instant instant(Object value) {
        return Instant.ofEpochMilli(number(value));
    }

    private static String uuidHex() {
        return UUID.randomUUID().toString().replace("-", "");
    }

    private static String script(String template) {
        return ZLinkRedisLocationConnection.script(template, ScriptToken.BY_WIRE)
                .replace(
                        "${VERSION_CLEANUP_GRACE_MILLIS}",
                        Long.toString(VERSION_CLEANUP_GRACE_MILLIS))
                .replace("${SNAPSHOT_RETENTION_MILLIS}", Long.toString(SNAPSHOT_RETENTION_MILLIS))
                .replace("${CLEANUP_DELAY_MILLIS}", Long.toString(CLEANUP_DELAY_MILLIS))
                .replace(
                        "${EXPIRED_SNAPSHOT_CLEANUP_BATCH}",
                        Long.toString(EXPIRED_SNAPSHOT_CLEANUP_BATCH))
                .replace("${VERSION_CLEANUP_BATCH}", Long.toString(VERSION_CLEANUP_BATCH))
                .replace("${MAXIMUM_VERSION_HISTORY}", Long.toString(MAXIMUM_VERSION_HISTORY))
                .replace("${MAXIMUM_ACTIVE_SNAPSHOTS}", Long.toString(MAXIMUM_ACTIVE_SNAPSHOTS))
                .replace("${SCAN_WORK_MULTIPLIER}", Long.toString(SCAN_WORK_MULTIPLIER))
                .replace("${MINIMUM_SCAN_WORK}", Long.toString(MINIMUM_SCAN_WORK))
                .replace(
                        "${ENCODED_ITEM_OVERHEAD_BYTES}",
                        Long.toString(ENCODED_ITEM_OVERHEAD_BYTES))
                .replace(
                        "${MAXIMUM_ENCODED_PAGE_BYTES}", Long.toString(MAXIMUM_ENCODED_PAGE_BYTES));
    }

    private static void requireOutcome(
            ScriptToken expected, ScriptToken actual, String operation, Object raw) {
        if (expected != actual) {
            throw new IllegalStateException(
                    "Redis returned an unknown Location Store "
                            + operation
                            + " result: "
                            + text(raw));
        }
    }

    private record ValidatedWrite(String[] redisKeys, byte[][] arguments) {}

    private record ValidatedScan(
            String prefix,
            String scanId,
            String lastKey,
            int limit,
            boolean create,
            boolean expired) {
        static ValidatedScan expired(String prefix, int limit) {
            return new ValidatedScan(
                    prefix, "00000000000000000000000000000000", "", limit, false, true);
        }
    }
}
