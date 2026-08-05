package systems.zlink.framework.runtime.locations;

import java.time.Clock;
import java.time.Instant;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.locationprovider.*;

/**
 * Atomic opaque Store used by the built-in in-memory configuration.
 */
public final class ZLinkInMemoryProviderLocationStore
    implements systems.zlink.framework.locationprovider.ZLinkLocationStore {
    private static final int MAXIMUM_ACTIVE_SCANS = 4096;
    private static final long MAXIMUM_ENCODED_PAGE_BYTES =
        4L * 1024 * 1024;
    private static final java.time.Duration SCAN_RETENTION =
        java.time.Duration.ofMinutes(1);
    private final Object gate = new Object();
    private final Clock clock;
    private final Map<String, Entry> rows = new HashMap<>();
    private final Map<String, ScanSnapshot> snapshots =
        new HashMap<>();
    private long version;

    public ZLinkInMemoryProviderLocationStore() {
        this(Clock.systemUTC());
    }

    public ZLinkInMemoryProviderLocationStore(Clock clock) {
        this.clock = Objects.requireNonNull(clock, "clock");
    }

    @Override
    public CompletionStage<ZLinkStoreReadResult> read(
        ZLinkStoreKey key,
        ZLinkStoreCancellation cancellation) {
        requireActive(cancellation);
        String value = requireKey(key);
        synchronized (gate) {
            Instant now = clock.instant();
            Entry entry = live(value, now);
            return completed(entry == null
                ? new ZLinkStoreReadMissing(now)
                : new ZLinkStoreReadFound(entry.value(now)));
        }
    }

    @Override
    public CompletionStage<ZLinkStoreWriteResult> write(
        ZLinkStoreWriteRequest request,
        ZLinkStoreCancellation cancellation) {
        requireActive(cancellation);
        Objects.requireNonNull(request, "request");
        List<ZLinkStoreCondition> conditions =
            List.copyOf(request.conditions());
        List<ZLinkStoreMutation> mutations =
            List.copyOf(request.mutations());
        validateUniqueKeys(conditions, mutations);
        synchronized (gate) {
            Instant now = clock.instant();
            for (ZLinkStoreCondition condition : conditions) {
                if (!matches(condition, now)) {
                    return completed(new ZLinkStoreWriteConflict(now));
                }
            }
            Map<ZLinkStoreKey, ZLinkStoreVersion> putVersions =
                new LinkedHashMap<>();
            for (ZLinkStoreMutation mutation : mutations) {
                if (mutation instanceof ZLinkStoreDelete delete) {
                    rows.remove(requireKey(delete.key()));
                } else if (mutation instanceof ZLinkStorePut put) {
                    String key = requireKey(put.key());
                    byte[] bytes = requireBytes(put.bytes());
                    Instant expiresAt = put.retention() == null
                        ? null
                        : now.plus(put.retention());
                    var next = new ZLinkStoreVersion(
                        Long.toUnsignedString(++version));
                    rows.put(key, new Entry(bytes, next, expiresAt));
                    putVersions.put(put.key(), next);
                }
            }
            return completed(new ZLinkStoreWriteApplied(
                Map.copyOf(putVersions), now));
        }
    }

    @Override
    public CompletionStage<ZLinkStoreScanResult> scan(
        ZLinkStoreScanRequest request,
        ZLinkStoreCancellation cancellation) {
        requireActive(cancellation);
        Objects.requireNonNull(request, "request");
        if (request.limit() < 1 || request.limit() > 1000) {
            throw new IllegalArgumentException("scan limit must be 1..1000");
        }
        String prefix = Objects.requireNonNull(request.prefix(), "prefix");
        if (prefix.getBytes(java.nio.charset.StandardCharsets.UTF_8).length
            > 1024) {
            throw new IllegalArgumentException(
                "scan prefix exceeds 1024 UTF-8 bytes");
        }
        synchronized (gate) {
            Instant now = clock.instant();
            removeExpiredSnapshots(now);
            String snapshotId;
            int offset;
            List<ZLinkStoreScanItem> items;
            if (request.cursor() == null) {
                if (snapshots.size() >= MAXIMUM_ACTIVE_SCANS) {
                    throw new IllegalStateException(
                        "Location Store scan capacity is full");
                }
                snapshotId = UUID.randomUUID().toString();
                offset = 0;
                rows.entrySet().removeIf(entry ->
                    entry.getValue().expiresAt() != null
                        && !entry.getValue().expiresAt().isAfter(now));
                items = rows.entrySet().stream()
                    .filter(entry -> entry.getKey().startsWith(prefix))
                    .sorted(Map.Entry.comparingByKey())
                    .map(entry -> new ZLinkStoreScanItem(
                        new ZLinkStoreKey(entry.getKey()),
                        entry.getValue().value(now)))
                    .toList();
                snapshots.put(
                    snapshotId,
                    new ScanSnapshot(items, now.plus(SCAN_RETENTION)));
            } else {
                String[] cursor = request.cursor().value().split(":", 2);
                if (cursor.length != 2 || !snapshots.containsKey(cursor[0])) {
                    return completed(new ZLinkStoreScanExpired());
                }
                snapshotId = cursor[0];
                offset = Integer.parseInt(cursor[1]);
                items = snapshots.get(snapshotId).items();
            }
            if (offset < 0 || offset > items.size()) {
                snapshots.remove(snapshotId);
                return completed(new ZLinkStoreScanExpired());
            }
            int end = offset;
            long encodedBytes = 0;
            while (end < items.size()
                && end - offset < request.limit()) {
                ZLinkStoreScanItem item = items.get(end);
                long itemBytes = item.key().value().getBytes(
                        java.nio.charset.StandardCharsets.UTF_8).length
                    + item.value().version().value().getBytes(
                        java.nio.charset.StandardCharsets.UTF_8).length
                    + item.value().bytes().length;
                if (end != offset
                    && encodedBytes + itemBytes
                        > MAXIMUM_ENCODED_PAGE_BYTES) {
                    break;
                }
                encodedBytes += itemBytes;
                end++;
            }
            List<ZLinkStoreScanItem> page = List.copyOf(
                items.subList(offset, end));
            ZLinkStoreScanCursor next = end < items.size()
                ? new ZLinkStoreScanCursor(snapshotId + ":" + end)
                : null;
            if (next == null) {
                snapshots.remove(snapshotId);
            }
            return completed(new ZLinkStoreScanPageResult(
                new ZLinkStoreScanPage(page, next, now)));
        }
    }

    private void removeExpiredSnapshots(Instant now) {
        snapshots.entrySet().removeIf(entry ->
            !entry.getValue().expiresAt().isAfter(now));
    }

    private boolean matches(ZLinkStoreCondition condition, Instant now) {
        if (condition instanceof ZLinkStoreMissingCondition missing) {
            return live(requireKey(missing.key()), now) == null;
        }
        var expected = (ZLinkStoreVersionCondition) condition;
        Entry current = live(requireKey(expected.key()), now);
        return current != null && current.version().equals(expected.expected());
    }

    private Entry live(String key, Instant now) {
        Entry entry = rows.get(key);
        if (entry != null
            && entry.expiresAt() != null
            && !entry.expiresAt().isAfter(now)) {
            rows.remove(key);
            return null;
        }
        return entry;
    }

    private static void validateUniqueKeys(
        List<ZLinkStoreCondition> conditions,
        List<ZLinkStoreMutation> mutations) {
        var conditionKeys = new java.util.HashSet<String>();
        var mutationKeys = new java.util.HashSet<String>();
        long encodedBytes = 0;
        for (var condition : conditions) {
            String key = requireKey(condition instanceof ZLinkStoreMissingCondition value
                ? value.key()
                : ((ZLinkStoreVersionCondition) condition).key());
            if (!conditionKeys.add(key)) {
                throw new IllegalArgumentException("duplicate condition key");
            }
            encodedBytes += key.getBytes(
                java.nio.charset.StandardCharsets.UTF_8).length;
        }
        for (var mutation : mutations) {
            String key = requireKey(mutation instanceof ZLinkStorePut value
                ? value.key()
                : ((ZLinkStoreDelete) mutation).key());
            if (!mutationKeys.add(key)) {
                throw new IllegalArgumentException("duplicate mutation key");
            }
            encodedBytes += key.getBytes(
                java.nio.charset.StandardCharsets.UTF_8).length;
            if (mutation instanceof ZLinkStorePut put) {
                encodedBytes += Objects.requireNonNull(put.bytes(), "bytes").length;
                requireBytes(put.bytes());
                if (put.retention() != null
                    && (put.retention().isZero()
                        || put.retention().isNegative())) {
                    throw new IllegalArgumentException(
                        "retention must be positive");
                }
            }
        }
        var all = new java.util.HashSet<>(conditionKeys);
        all.addAll(mutationKeys);
        if (all.size() > 2048) {
            throw new IllegalArgumentException("write exceeds 2,048 keys");
        }
        if (encodedBytes > 4L * 1024 * 1024) {
            throw new IllegalArgumentException("write exceeds 4 MiB");
        }
    }

    private static String requireKey(ZLinkStoreKey key) {
        String value = Objects.requireNonNull(
            Objects.requireNonNull(key, "key").value(), "key.value");
        int size = value.getBytes(java.nio.charset.StandardCharsets.UTF_8).length;
        if (size < 1 || size > 1024) {
            throw new IllegalArgumentException("key must be 1..1024 UTF-8 bytes");
        }
        return value;
    }

    private static byte[] requireBytes(byte[] bytes) {
        byte[] copy = Objects.requireNonNull(bytes, "bytes").clone();
        if (copy.length > 1024 * 1024) {
            throw new IllegalArgumentException("value exceeds 1 MiB");
        }
        return copy;
    }

    private static void requireActive(ZLinkStoreCancellation cancellation) {
        if (Objects.requireNonNull(cancellation, "cancellation")
            .isCancellationRequested()) {
            throw new java.util.concurrent.CancellationException();
        }
    }

    private static <T> CompletionStage<T> completed(T value) {
        return CompletableFuture.completedFuture(value);
    }

    private record Entry(
        byte[] bytes,
        ZLinkStoreVersion version,
        Instant expiresAt) {
        private Entry {
            bytes = bytes.clone();
        }

        private ZLinkStoreValue value(Instant now) {
            return new ZLinkStoreValue(
                bytes.clone(), version, expiresAt, now);
        }
    }

    private record ScanSnapshot(
        List<ZLinkStoreScanItem> items,
        Instant expiresAt) {}
}
