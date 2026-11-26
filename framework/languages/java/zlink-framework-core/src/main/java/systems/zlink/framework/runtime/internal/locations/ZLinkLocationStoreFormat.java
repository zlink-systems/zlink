package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;

import java.io.ByteArrayOutputStream;
import java.net.URLDecoder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;

/** Converts the Location Store before the first owner claim, as required by Store §8.1. */
final class ZLinkLocationStoreFormat {
    static final ZLinkStoreKey FLOOR = new ZLinkStoreKey("zlink:v12:generation-floor");
    static final ZLinkStoreKey LEGACY_OBJECT_COUNTER =
            new ZLinkStoreKey("zlink:v11:object-counter");
    static final ZLinkStoreKey LEGACY_AUTHORITY_OWNER_COUNTER =
            new ZLinkStoreKey("zlink:v11:authority-owner-counter");

    private static final ZLinkStoreKey FORMAT = new ZLinkStoreKey("zlink:v12:format");
    private static final byte[] CONVERTED = "converted".getBytes(StandardCharsets.UTF_8);
    private static final int PAGE_SIZE = 1000;

    private final ZLinkLocationStore provider;
    private final ZLinkStoreCancellation cancellation;
    private final ZLinkProviderAuthorityRepository authority;
    private final Map<String, List<RoutingId>> descriptorRids = new HashMap<>();

    ZLinkLocationStoreFormat(
            ZLinkLocationStore provider,
            ZLinkStoreCancellation cancellation,
            ZLinkProviderAuthorityRepository authority) {
        this.provider = provider;
        this.cancellation = cancellation;
        this.authority = authority;
    }

    CompletionStage<Void> convert() {
        return CompletableFuture.completedFuture(FORMAT)
                .thenCompose(this::read)
                .thenCompose(
                        format -> {
                            if (format instanceof ZLinkStoreReadFound found) {
                                if (!Arrays.equals(CONVERTED, found.value().bytes()))
                                    throw new ZLinkConfigurationException(
                                            "Unrecognized Location Store format");
                                return CompletableFuture.completedFuture(null);
                            }
                            return authority
                                    .settleLegacyAggregates(cancellation)
                                    .exceptionallyCompose(
                                            ZLinkLocationStoreFormat::settlementFailure)
                                    .thenCompose(ignored -> ensureFloor())
                                    .thenCompose(ignored -> movePage(null));
                        });
    }

    private static CompletionStage<Void> settlementFailure(Throwable failure) {
        Throwable cause = unwrap(failure);
        if (cause instanceof IllegalArgumentException
                || cause instanceof IllegalStateException
                || cause instanceof ZLinkFrameworkException error
                        && error.kind() == ZLinkFrameworkErrorKind.DATA_LOST) {
            cause =
                    new ZLinkConfigurationException(
                            "Cannot settle v11 aggregates before conversion", cause);
        }
        return CompletableFuture.failedFuture(cause);
    }

    private CompletionStage<Void> ensureFloor() {
        return read(FLOOR)
                .thenCompose(
                        floor -> {
                            if (floor instanceof ZLinkStoreReadFound) {
                                generation(floor);
                                return CompletableFuture.completedFuture(null);
                            }
                            return read(LEGACY_OBJECT_COUNTER)
                                    .thenCombine(
                                            read(LEGACY_AUTHORITY_OWNER_COUNTER),
                                            (object, owner) ->
                                                    Math.max(generation(object), generation(owner)))
                                    .thenCompose(value -> putMissing(FLOOR, encodeCounter(value)));
                        });
    }

    private CompletionStage<Void> movePage(ZLinkStoreScanCursor cursor) {
        return scan(ZLinkLegacyCapacity.PREFIX, cursor)
                .thenCompose(
                        result -> {
                            if (result instanceof ZLinkStoreScanExpired) return nextPage(null);
                            var page = ((ZLinkStoreScanPageResult) result).value();
                            if (cursor == null
                                    && page.items().isEmpty()
                                    && page.nextCursor() == null) {
                                return putMissing(FORMAT, CONVERTED);
                            }
                            CompletionStage<Void> moved = CompletableFuture.completedFuture(null);
                            for (var item : page.items()) {
                                moved = moved.thenCompose(ignored -> move(item.key()));
                            }
                            // Start a fresh scan after the final page to confirm no v11 rows
                            // remain.
                            return moved.thenCompose(ignored -> nextPage(page.nextCursor()));
                        });
    }

    private CompletionStage<Void> nextPage(ZLinkStoreScanCursor cursor) {
        return CompletableFuture.completedFuture(cursor).thenComposeAsync(this::movePage);
    }

    private CompletionStage<Void> move(ZLinkStoreKey oldKey) {
        String identity = oldKey.value().substring(ZLinkLegacyCapacity.PREFIX.length());
        int separator = identity.indexOf(':');
        if (separator < 0)
            throw new ZLinkConfigurationException("Malformed v11 capacity key " + oldKey.value());
        String mesh = URLDecoder.decode(identity.substring(0, separator), StandardCharsets.UTF_8);
        byte[] rid = decodeRid(oldKey, identity.substring(separator + 1));
        var known = descriptorRids.get(mesh);
        CompletionStage<List<RoutingId>> current =
                known == null
                        ? scanDescriptors(mesh, null, new ArrayList<>())
                                .thenApply(
                                        rids -> {
                                            descriptorRids.put(mesh, rids);
                                            return rids;
                                        })
                        : CompletableFuture.completedFuture(known);
        // A later startup may publish descriptors once every old row has been removed.
        // Read the old row after the descriptor scan to use those descriptors only while it exists.
        return current.thenCompose(
                candidates ->
                        read(oldKey)
                                .thenCompose(
                                        old -> {
                                            if (!(old instanceof ZLinkStoreReadFound found))
                                                return CompletableFuture.completedFuture(null);
                                            var target =
                                                    new ZLinkMeshNodeDescriptorKey(
                                                            mesh,
                                                            resolveRid(oldKey, rid, candidates));
                                            return writeCapacity(oldKey, target, found.value());
                                        }));
    }

    private CompletionStage<List<RoutingId>> scanDescriptors(
            String mesh, ZLinkStoreScanCursor cursor, List<RoutingId> rids) {
        return scan(ZLinkProviderDescriptorRepository.meshPrefix(mesh), cursor)
                .thenComposeAsync(
                        result -> {
                            if (result instanceof ZLinkStoreScanExpired)
                                return scanDescriptors(mesh, null, new ArrayList<>());
                            var page = ((ZLinkStoreScanPageResult) result).value();
                            for (var item : page.items()) {
                                rids.add(
                                        ZLinkProviderDescriptorRepository.decodeMeshNodeRecord(
                                                        item.value().bytes())
                                                .rid());
                            }
                            return page.nextCursor() == null
                                    ? CompletableFuture.completedFuture(rids)
                                    : scanDescriptors(mesh, page.nextCursor(), rids);
                        });
    }

    private CompletionStage<Void> writeCapacity(
            ZLinkStoreKey oldKey, ZLinkMeshNodeDescriptorKey target, ZLinkStoreValue oldValue) {
        byte[] bytes = oldValue.bytes();
        var counters = ZLinkLegacyCapacity.decode(oldKey, bytes).counterPuts(target);
        // Zero-valued counters are not target keys: only Put keys are conditioned and counted.
        var counterKeys = counters.stream().map(ZLinkStorePut::key).toList();
        if (counters.size() >= ZLinkLocationStoreLimits.MAXIMUM_BATCH_KEYS)
            throw new ZLinkConfigurationException(
                    "v11 capacity conversion exceeds the Location Store batch key limit for "
                            + oldKey.value());
        var conditions = new ArrayList<ZLinkStoreCondition>(counters.size() + 1);
        var mutations = new ArrayList<ZLinkStoreMutation>(counters.size() + 1);
        conditions.add(new ZLinkStoreValueCondition(oldKey, bytes));
        for (var counter : counters) {
            conditions.add(new ZLinkStoreMissingCondition(counter.key()));
            mutations.add(counter);
        }
        mutations.add(new ZLinkStoreDelete(oldKey));
        return write(new ZLinkStoreWriteRequest(conditions, mutations))
                .<CompletionStage<Void>>handle(
                        (result, failure) -> {
                            if (failure != null) {
                                Throwable cause = unwrap(failure);
                                if (cause instanceof IllegalArgumentException)
                                    return CompletableFuture.<Void>failedFuture(
                                            new ZLinkConfigurationException(
                                                    "Invalid v11 capacity conversion batch for "
                                                            + oldKey.value(),
                                                    cause));
                                if (!(cause instanceof ZLinkLocationStoreFailure))
                                    return CompletableFuture.<Void>failedFuture(cause);
                            } else if (result instanceof ZLinkStoreWriteApplied) {
                                return CompletableFuture.<Void>completedFuture(null);
                            } else if (!(result instanceof ZLinkStoreWriteConflict)) {
                                return CompletableFuture.<Void>failedFuture(
                                        new ZLinkConfigurationException(
                                                "Location Store rejected v11 capacity conversion"));
                            }
                            return confirmMove(oldKey, bytes, counterKeys);
                        })
                .thenCompose(stage -> stage);
    }

    private CompletionStage<Void> confirmMove(
            ZLinkStoreKey oldKey, byte[] bytes, List<ZLinkStoreKey> counterKeys) {
        return read(oldKey)
                .thenCompose(
                        old -> {
                            if (!(old instanceof ZLinkStoreReadFound found))
                                return CompletableFuture.completedFuture(null);
                            if (!Arrays.equals(bytes, found.value().bytes()))
                                return CompletableFuture.completedFuture(null)
                                        .thenComposeAsync(ignored -> move(oldKey));
                            var destinations =
                                    counterKeys.stream()
                                            .map(key -> read(key).toCompletableFuture())
                                            .toList();
                            return CompletableFuture.allOf(
                                            destinations.toArray(CompletableFuture<?>[]::new))
                                    .thenComposeAsync(
                                            ignored -> {
                                                if (destinations.stream()
                                                        .anyMatch(result -> result.join()
                                                                instanceof ZLinkStoreReadFound))
                                                    return CompletableFuture.<Void>failedFuture(
                                                            new ZLinkConfigurationException(
                                                                    "Existing Location Store counter for "
                                                                            + oldKey.value()));
                                                return move(oldKey);
                                            });
                        });
    }

    private CompletionStage<Void> putMissing(ZLinkStoreKey key, byte[] bytes) {
        var request =
                new ZLinkStoreWriteRequest(
                        List.of(new ZLinkStoreMissingCondition(key)),
                        List.of(new ZLinkStorePut(key, bytes, null)));
        return write(request)
                .<CompletionStage<Void>>handle(
                        (result, failure) -> {
                            if (failure == null) {
                                if (result instanceof ZLinkStoreWriteApplied
                                        || result instanceof ZLinkStoreWriteConflict)
                                    return CompletableFuture.<Void>completedFuture(null);
                                return CompletableFuture.<Void>failedFuture(
                                        new ZLinkConfigurationException(
                                                "Location Store rejected conversion key "
                                                        + key.value()));
                            }
                            Throwable cause = unwrap(failure);
                            if (!(cause instanceof ZLinkLocationStoreFailure))
                                return CompletableFuture.<Void>failedFuture(cause);
                            return read(key)
                                    .thenCompose(
                                            read ->
                                                    read instanceof ZLinkStoreReadFound
                                                            ? CompletableFuture.completedFuture(
                                                                    null)
                                                            : CompletableFuture.failedFuture(
                                                                    cause));
                        })
                .thenCompose(stage -> stage);
    }

    private CompletionStage<ZLinkStoreReadResult> read(ZLinkStoreKey key) {
        return provider.read(key, cancellation);
    }

    private CompletionStage<ZLinkStoreScanResult> scan(String prefix, ZLinkStoreScanCursor cursor) {
        return provider.scan(new ZLinkStoreScanRequest(prefix, cursor, PAGE_SIZE), cancellation);
    }

    private CompletionStage<ZLinkStoreWriteResult> write(ZLinkStoreWriteRequest request) {
        return CompletableFuture.completedFuture(request)
                .thenCompose(batch -> provider.write(batch, cancellation));
    }

    private static byte[] decodeRid(ZLinkStoreKey key, String encoded) {
        byte[] bytes = encoded.getBytes(StandardCharsets.UTF_8);
        var decoded = new ByteArrayOutputStream(bytes.length);
        for (int i = 0; i < bytes.length; i++) {
            int b = bytes[i];
            if (b == '%') {
                int high = i + 2 < bytes.length ? Character.digit(bytes[i + 1], 16) : -1;
                int low = i + 2 < bytes.length ? Character.digit(bytes[i + 2], 16) : -1;
                if (high < 0 || low < 0)
                    throw new ZLinkConfigurationException(
                            "Malformed v11 capacity RID " + key.value());
                decoded.write((high << 4) | low);
                i += 2;
            } else {
                decoded.write(b == '+' ? ' ' : b);
            }
        }
        return decoded.toByteArray();
    }

    private static RoutingId resolveRid(
            ZLinkStoreKey oldKey, byte[] rid, List<RoutingId> candidates) {
        var matches =
                candidates.stream()
                        .distinct()
                        .filter(
                                candidate ->
                                        Arrays.equals(candidate.toBytes(), rid)
                                                || Arrays.equals(
                                                        candidate
                                                                .toString()
                                                                .getBytes(StandardCharsets.UTF_8),
                                                        rid))
                        .limit(2)
                        .toList();
        if (matches.size() > 1)
            throw new ZLinkConfigurationException(
                    "Several MeshNode descriptors match v11 capacity key " + oldKey.value());
        if (!matches.isEmpty()) return matches.getFirst();
        try {
            return RoutingId.from(rid);
        } catch (IllegalArgumentException failure) {
            throw new ZLinkConfigurationException(
                    "Invalid v11 capacity RID " + oldKey.value(), failure);
        }
    }

    static byte[] encodeCounter(long value) {
        return Long.toString(value).getBytes(StandardCharsets.UTF_8);
    }

    static long decodeCounter(byte[] bytes) {
        String text = new String(bytes, StandardCharsets.UTF_8);
        long value = Long.parseLong(text);
        if (value >= 0 && Long.toString(value).equals(text)) return value;
        throw new NumberFormatException("invalid counter decimal");
    }

    static long decodeGeneration(byte[] bytes) {
        try {
            long value = decodeCounter(bytes);
            if (value != 0) return value;
            throw new NumberFormatException("outside generation range");
        } catch (NumberFormatException failure) {
            throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.DATA_LOST,
                    "Location Store counter is invalid",
                    failure);
        }
    }

    private static long generation(ZLinkStoreReadResult result) {
        if (!(result instanceof ZLinkStoreReadFound found)) return 1L;
        try {
            return decodeGeneration(found.value().bytes());
        } catch (ZLinkFrameworkException invalid) {
            throw new ZLinkConfigurationException(
                    "Invalid Location Store generation floor input", invalid);
        }
    }

    private static Throwable unwrap(Throwable failure) {
        return failure instanceof CompletionException completion && completion.getCause() != null
                ? completion.getCause()
                : failure;
    }
}
