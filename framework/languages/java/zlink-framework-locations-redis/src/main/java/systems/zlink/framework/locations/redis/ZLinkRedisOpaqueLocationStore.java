package systems.zlink.framework.locations.redis;

import io.lettuce.core.ScriptOutputType;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Base64;
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
import systems.zlink.framework.locationprovider.ZLinkStoreVersion;
import systems.zlink.framework.locationprovider.ZLinkStoreVersionCondition;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteApplied;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteConflict;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteResult;

/**
 * Redis implementation of the opaque provider operations.
 *
 * <p>The version history retained by each key lets a scan keep one revision
 * boundary instead of copying every matching value into a snapshot.</p>
 */
final class ZLinkRedisOpaqueLocationStore implements ZLinkLocationStore {
    private static final int MAXIMUM_KEY_BYTES = 1024;
    private static final int MAXIMUM_VERSION_BYTES = 4096;
    private static final int MAXIMUM_VALUE_BYTES = 1024 * 1024;
    private static final int MAXIMUM_BATCH_KEYS = 2048;
    private static final int MAXIMUM_ENCODED_BATCH_BYTES = 4 * 1024 * 1024;

    private static final String READ_SCRIPT = """
        if redis.replicate_commands then redis.replicate_commands() end
        local time = redis.call('TIME')
        local nowMs = tonumber(time[1]) * 1000
            + math.floor(tonumber(time[2]) / 1000)
        local members = redis.call('ZREVRANGE', KEYS[1], 0, 0)
        if #members == 0 then return { 'missing', nowMs } end
        local record = cmsgpack.unpack(members[1])
        local expiresAt = tonumber(record[4])
        if record[5] == 1 or (expiresAt >= 0 and expiresAt <= nowMs) then
            return { 'missing', nowMs }
        end
        return {
            'found', nowMs, record[1], record[2], record[3], expiresAt
        }
        """;

    private static final String WRITE_SCRIPT = """
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
            'LIMIT', 0, 128)
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
            'LIMIT', 0, 32)
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
                redis.call('ZADD', cleanupKey, nowMs + 1000, original)
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
                            'ZADD', cleanupKey,
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
                    'ZADD', redisKey, sequence,
                    cmsgpack.pack({
                        originalKey, value, version, expiresAt, 0
                    }))
                redis.call('ZADD', indexKey, 0, originalKey)
                redis.call('HSET', mapKey, originalKey, redisKey)
                table.insert(putVersions, originalKey)
                table.insert(putVersions, version)
            else
                redis.call(
                    'ZADD', redisKey, sequence,
                    cmsgpack.pack({
                        originalKey, '', '', -1, 1
                    }))
                redis.call('ZADD', indexKey, 0, originalKey)
                redis.call('HSET', mapKey, originalKey, redisKey)
            end
            local dueAt = nowMs + 1000
            local scheduled = redis.call('ZSCORE', cleanupKey, originalKey)
            if not scheduled or tonumber(scheduled) > dueAt then
                redis.call('ZADD', cleanupKey, dueAt, originalKey)
            end
            arg = arg + 6
        end

        local result = { 'applied', nowMs }
        for _, item in ipairs(putVersions) do
            table.insert(result, item)
        end
        return result
        """;

    private static final String SCAN_SCRIPT = """
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
            'LIMIT', 0, 128)
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
            'LIMIT', 0, 32)
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
                redis.call('ZADD', cleanupKey, nowMs + 1000, original)
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
                            'ZADD', cleanupKey,
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
                'HSET', snapshot,
                'now', tostring(nowMs),
                'boundary', tostring(boundary),
                'prefix', prefix)
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
            'ZRANGEBYLEX', KEYS[1], lower, '+',
            'LIMIT', 0, workLimit + 1)
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
                        recordKey, boundary, '-inf',
                        'LIMIT', 0, 1)
                    if #members > 0 then
                        local record = cmsgpack.unpack(members[1])
                        local expiresAt = tonumber(record[4])
                        if record[1] == original
                            and record[5] ~= 1
                            and (expiresAt < 0 or expiresAt > snapshotNow) then
                            local itemBytes = string.len(original)
                                + string.len(record[2])
                                + string.len(record[3]) + 128
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

    private final ZLinkRedisLocationConnection connection;
    private final ZLinkRedisLocationKeys keys;

    ZLinkRedisOpaqueLocationStore(
        ZLinkRedisLocationConnection connection,
        ZLinkRedisLocationKeys keys) {
        this.connection = Objects.requireNonNull(connection, "connection");
        this.keys = Objects.requireNonNull(keys, "keys");
    }

    @Override
    public CompletionStage<ZLinkStoreReadResult> read(
        ZLinkStoreKey key,
        ZLinkStoreCancellation cancellation) {
        validateKey(key);
        if (cancelled(cancellation)) {
            return cancelledStage();
        }
        return connection.commands()
            .thenCompose(commands -> commands.<List<Object>>eval(
                READ_SCRIPT,
                ScriptOutputType.MULTI,
                new String[] {keys.opaqueRecordKey(key.value())}))
            .thenApply(result -> decodeRead(key, result));
    }

    @Override
    public CompletionStage<ZLinkStoreWriteResult> write(
        ZLinkStoreWriteRequest request,
        ZLinkStoreCancellation cancellation) {
        ValidatedWrite validated = validateWrite(request);
        if (cancelled(cancellation)) {
            return cancelledStage();
        }
        return connection.commands()
            .thenCompose(commands -> commands.<List<Object>>eval(
                WRITE_SCRIPT,
                ScriptOutputType.MULTI,
                validated.redisKeys(),
                validated.arguments()))
            .thenApply(this::decodeWrite);
    }

    @Override
    public CompletionStage<ZLinkStoreScanResult> scan(
        ZLinkStoreScanRequest request,
        ZLinkStoreCancellation cancellation) {
        ValidatedScan validated = validateScan(request);
        if (validated.expired()) {
            return CompletableFuture.completedFuture(
                new ZLinkStoreScanExpired());
        }
        if (cancelled(cancellation)) {
            return cancelledStage();
        }
        return connection.commands()
            .thenCompose(commands -> commands.<List<Object>>eval(
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
                validated.prefix(),
                validated.lastKey(),
                Integer.toString(validated.limit()),
                validated.create() ? "1" : "0",
                validated.scanId()))
            .thenApply(result -> decodeScan(validated.scanId(), result));
    }

    private ZLinkStoreReadResult decodeRead(
        ZLinkStoreKey expectedKey,
        List<Object> result) {
        String outcome = text(result.getFirst());
        Instant storeNow = instant(result.get(1));
        if ("missing".equals(outcome)) {
            return new ZLinkStoreReadMissing(storeNow);
        }
        requireOutcome("found", outcome, "read");
        if (!expectedKey.value().equals(text(result.get(2)))) {
            throw new IllegalStateException(
                "Redis opaque key digest resolved to a different key.");
        }
        long expiresAt = number(result.get(5));
        return new ZLinkStoreReadFound(
            new ZLinkStoreValue(
                Base64.getDecoder().decode(text(result.get(3))),
                new ZLinkStoreVersion(text(result.get(4))),
                expiresAt >= 0 ? Instant.ofEpochMilli(expiresAt) : null,
                storeNow));
    }

    private ZLinkStoreWriteResult decodeWrite(List<Object> result) {
        String outcome = text(result.getFirst());
        Instant storeNow = instant(result.get(1));
        if ("conflict".equals(outcome)) {
            return new ZLinkStoreWriteConflict(storeNow);
        }
        if ("backlog".equals(outcome)) {
            throw new CompletionException(new IOException(
                "Redis Location Store version backlog is full."));
        }
        requireOutcome("applied", outcome, "write");
        Map<ZLinkStoreKey, ZLinkStoreVersion> versions =
            new LinkedHashMap<>();
        for (int index = 2; index < result.size(); index += 2) {
            versions.put(
                new ZLinkStoreKey(text(result.get(index))),
                new ZLinkStoreVersion(text(result.get(index + 1))));
        }
        return new ZLinkStoreWriteApplied(Map.copyOf(versions), storeNow);
    }

    private ZLinkStoreScanResult decodeScan(
        String scanId,
        List<Object> result) {
        String outcome = text(result.getFirst());
        if ("expired".equals(outcome)) {
            return new ZLinkStoreScanExpired();
        }
        if ("capacity".equals(outcome)) {
            throw new CompletionException(new IOException(
                "Redis Location Store snapshot capacity is full."));
        }
        requireOutcome("page", outcome, "scan");
        Instant storeNow = instant(result.get(1));
        String nextKey = text(result.get(2));
        List<ZLinkStoreScanItem> items = new ArrayList<>();
        for (int index = 3; index < result.size(); index += 4) {
            long expiresAt = number(result.get(index + 3));
            items.add(new ZLinkStoreScanItem(
                new ZLinkStoreKey(text(result.get(index))),
                new ZLinkStoreValue(
                    Base64.getDecoder().decode(text(result.get(index + 1))),
                    new ZLinkStoreVersion(text(result.get(index + 2))),
                    expiresAt >= 0
                        ? Instant.ofEpochMilli(expiresAt)
                        : null,
                    storeNow)));
        }
        ZLinkStoreScanCursor next = nextKey.isEmpty()
            ? null
            : new ZLinkStoreScanCursor(
                scanId + ":"
                    + HexFormat.of().formatHex(
                        nextKey.getBytes(StandardCharsets.UTF_8)));
        return new ZLinkStoreScanPageResult(
            new ZLinkStoreScanPage(List.copyOf(items), next, storeNow));
    }

    private ValidatedWrite validateWrite(ZLinkStoreWriteRequest request) {
        Objects.requireNonNull(request, "request");
        List<ZLinkStoreCondition> conditions =
            List.copyOf(Objects.requireNonNull(
                request.conditions(),
                "request.conditions"));
        List<ZLinkStoreMutation> mutations =
            List.copyOf(Objects.requireNonNull(
                request.mutations(),
                "request.mutations"));
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
            } else {
                throw new IllegalArgumentException(
                    "Unknown Location Store condition.");
            }
            encodedBytes += validateKey(key);
            if (!conditionKeys.add(key)) {
                throw new IllegalArgumentException(
                    "A key cannot occur twice in conditions.");
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
                if (retention != null
                    && (retention.isZero() || retention.isNegative())) {
                    throw new IllegalArgumentException(
                        "Retention must be positive.");
                }
                encodedBytes += bytes.length;
            } else if (mutation instanceof ZLinkStoreDelete delete) {
                key = delete.key();
            } else {
                throw new IllegalArgumentException(
                    "Unknown Location Store mutation.");
            }
            encodedBytes += validateKey(key);
            if (!mutationKeys.add(key)) {
                throw new IllegalArgumentException(
                    "A key cannot occur twice in mutations.");
            }
            uniqueKeys.add(key);
        }
        if (uniqueKeys.size() > MAXIMUM_BATCH_KEYS) {
            throw new IllegalArgumentException(
                "A conditional batch can reference at most 2048 keys.");
        }
        if (encodedBytes > MAXIMUM_ENCODED_BATCH_BYTES) {
            throw new IllegalArgumentException(
                "The encoded Store batch exceeds 4 MiB.");
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

        List<String> arguments = new ArrayList<>();
        arguments.add(Integer.toString(conditions.size()));
        arguments.add(Integer.toString(mutations.size()));
        for (ZLinkStoreCondition condition : conditions) {
            if (condition instanceof ZLinkStoreMissingCondition) {
                arguments.add("missing");
                arguments.add("");
            } else {
                ZLinkStoreVersionCondition version =
                    (ZLinkStoreVersionCondition) condition;
                arguments.add("version");
                arguments.add(version.expected().value());
            }
        }
        Base64.Encoder encoder = Base64.getEncoder();
        for (ZLinkStoreMutation mutation : mutations) {
            ZLinkStoreKey key = mutation instanceof ZLinkStorePut put
                ? put.key()
                : ((ZLinkStoreDelete) mutation).key();
            arguments.add(Integer.toString(keyIndexes.get(key)));
            if (mutation instanceof ZLinkStorePut put) {
                arguments.add("put");
                arguments.add(key.value());
                arguments.add(encoder.encodeToString(put.bytes()));
                arguments.add(uuidHex());
                arguments.add(put.retention() == null
                    ? "-1"
                    : Long.toString(ceilMillis(put.retention())));
            } else {
                arguments.add("delete");
                arguments.add(key.value());
                arguments.add("");
                arguments.add("");
                arguments.add("-1");
            }
        }
        return new ValidatedWrite(
            redisKeys,
            arguments.toArray(String[]::new));
    }

    private static ValidatedScan validateScan(
        ZLinkStoreScanRequest request) {
        Objects.requireNonNull(request, "request");
        String prefix = Objects.requireNonNull(
            request.prefix(),
            "request.prefix");
        if (utf8Length(prefix) > MAXIMUM_KEY_BYTES) {
            throw new IllegalArgumentException(
                "The scan prefix exceeds 1024 UTF-8 bytes.");
        }
        if (request.limit() < 1 || request.limit() > 1000) {
            throw new IllegalArgumentException(
                "Scan limit must be in the range 1..1000.");
        }
        if (request.cursor() == null) {
            return new ValidatedScan(
                prefix,
                uuidHex(),
                "",
                request.limit(),
                true,
                false);
        }
        String cursor = Objects.requireNonNull(
            request.cursor().value(),
            "request.cursor.value");
        int cursorBytes = utf8Length(cursor);
        int separator = cursor.lastIndexOf(':');
        if (cursorBytes < 1
            || cursorBytes > MAXIMUM_VERSION_BYTES
            || separator != 32) {
            return ValidatedScan.expired(prefix, request.limit());
        }
        String scanId = cursor.substring(0, separator);
        if (!scanId.matches("[0-9a-f]{32}")) {
            return ValidatedScan.expired(prefix, request.limit());
        }
        String lastKey = decodeCursorKey(
            cursor.substring(separator + 1));
        if (lastKey == null) {
            return ValidatedScan.expired(prefix, request.limit());
        }
        return new ValidatedScan(
            prefix,
            scanId,
            lastKey,
            request.limit(),
            false,
            false);
    }

    private static String decodeCursorKey(String encoded) {
        if (encoded.length() < 2
            || encoded.length() > MAXIMUM_KEY_BYTES * 2
            || (encoded.length() & 1) != 0) {
            return null;
        }
        try {
            byte[] bytes = HexFormat.of().parseHex(encoded);
            String key = StandardCharsets.UTF_8.newDecoder()
                .onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
                .decode(ByteBuffer.wrap(bytes))
                .toString();
            return utf8Length(key) <= MAXIMUM_KEY_BYTES
                ? key
                : null;
        } catch (IllegalArgumentException | CharacterCodingException error) {
            return null;
        }
    }

    private static int validateKey(ZLinkStoreKey key) {
        Objects.requireNonNull(key, "key");
        int length = utf8Length(Objects.requireNonNull(
            key.value(),
            "key.value"));
        if (length < 1 || length > MAXIMUM_KEY_BYTES) {
            throw new IllegalArgumentException(
                "Location Store keys must contain 1..1024 UTF-8 bytes.");
        }
        return length;
    }

    private static int validateVersion(ZLinkStoreVersion version) {
        Objects.requireNonNull(version, "version");
        int length = utf8Length(Objects.requireNonNull(
            version.value(),
            "version.value"));
        if (length < 1 || length > MAXIMUM_VERSION_BYTES) {
            throw new IllegalArgumentException(
                "Store versions must contain 1..4096 UTF-8 bytes.");
        }
        return length;
    }

    private static int utf8Length(String value) {
        return value.getBytes(StandardCharsets.UTF_8).length;
    }

    private static long ceilMillis(Duration duration) {
        long seconds = duration.getSeconds();
        int nanos = duration.getNano();
        return Math.addExact(
            Math.multiplyExact(seconds, 1000),
            (nanos + 999_999L) / 1_000_000L);
    }

    private static boolean cancelled(ZLinkStoreCancellation cancellation) {
        return cancellation != null
            && cancellation.isCancellationRequested();
    }

    private static <T> CompletionStage<T> cancelledStage() {
        return CompletableFuture.failedFuture(
            new CancellationException(
                "Location Store operation was cancelled."));
    }

    private static String text(Object value) {
        return String.valueOf(value);
    }

    private static long number(Object value) {
        return Long.parseLong(text(value));
    }

    private static Instant instant(Object value) {
        return Instant.ofEpochMilli(number(value));
    }

    private static String uuidHex() {
        return UUID.randomUUID().toString()
            .replace("-", "");
    }

    private static void requireOutcome(
        String expected,
        String actual,
        String operation) {
        if (!expected.equals(actual)) {
            throw new IllegalStateException(
                "Redis returned an unknown Location Store "
                    + operation + " result: " + actual);
        }
    }

    private record ValidatedWrite(
        String[] redisKeys,
        String[] arguments) {}

    private record ValidatedScan(
        String prefix,
        String scanId,
        String lastKey,
        int limit,
        boolean create,
        boolean expired) {
        static ValidatedScan expired(String prefix, int limit) {
            return new ValidatedScan(
                prefix,
                "00000000000000000000000000000000",
                "",
                limit,
                false,
                true);
        }
    }
}
