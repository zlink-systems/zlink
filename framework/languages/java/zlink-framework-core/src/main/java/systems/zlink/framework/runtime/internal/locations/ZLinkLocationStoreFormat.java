package systems.zlink.framework.runtime.internal.locations;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;

import java.io.IOException;
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
    private static final String LEGACY_CAPACITY = "zlink:v11:capacity:";
    private static final int PAGE_SIZE = 1000;
    private static final ObjectMapper JSON = new ObjectMapper();

    private final ZLinkLocationStore provider;
    private final ZLinkStoreCancellation cancellation;
    private final Map<String, List<RoutingId>> descriptorRids = new HashMap<>();

    ZLinkLocationStoreFormat(ZLinkLocationStore provider, ZLinkStoreCancellation cancellation) {
        this.provider = provider;
        this.cancellation = cancellation;
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
                            return ensureFloor().thenCompose(ignored -> movePage(null));
                        });
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
        return scan(LEGACY_CAPACITY, cursor)
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
        String identity = oldKey.value().substring(LEGACY_CAPACITY.length());
        int separator = identity.indexOf(':');
        if (separator < 0)
            throw new ZLinkConfigurationException("Malformed v11 capacity key " + oldKey.value());
        String mesh = URLDecoder.decode(identity.substring(0, separator), StandardCharsets.UTF_8);
        String rid = URLDecoder.decode(identity.substring(separator + 1), StandardCharsets.UTF_8);
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
        var counters = capacityPuts(oldKey, target, bytes);
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
                            return confirmMove(oldKey, counters);
                        })
                .thenCompose(stage -> stage);
    }

    private CompletionStage<Void> confirmMove(ZLinkStoreKey oldKey, List<ZLinkStorePut> counters) {
        return read(oldKey)
                .thenCompose(
                        old -> {
                            var destinations =
                                    counters.stream()
                                            .map(
                                                    counter ->
                                                            read(counter.key())
                                                                    .toCompletableFuture())
                                            .toList();
                            return CompletableFuture.allOf(
                                            destinations.toArray(CompletableFuture<?>[]::new))
                                    .thenComposeAsync(
                                            ignored -> {
                                                if (old instanceof ZLinkStoreReadFound)
                                                    return move(oldKey);
                                                if (destinations.stream()
                                                        .allMatch(
                                                                result ->
                                                                        result.join()
                                                                                instanceof
                                                                                ZLinkStoreReadFound))
                                                    return CompletableFuture.completedFuture(null);
                                                // Counters are never deleted, so a missing row with
                                                // a missing counter cannot become consistent by
                                                // reading again.
                                                return CompletableFuture.<Void>failedFuture(
                                                        new ZLinkConfigurationException(
                                                                "The migrated Location Store"
                                                                        + " counter is missing for "
                                                                        + oldKey.value()));
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

    private static RoutingId resolveRid(
            ZLinkStoreKey oldKey, String rid, List<RoutingId> candidates) {
        var matches =
                candidates.stream()
                        .distinct()
                        .filter(candidate -> candidate.toString().equals(rid))
                        .limit(2)
                        .toList();
        if (matches.size() > 1)
            throw new ZLinkConfigurationException(
                    "Several MeshNode descriptors match v11 capacity key " + oldKey.value());
        if (!matches.isEmpty()) return matches.getFirst();
        // A v11 display string longer than the maximum RID cannot have been a text RID.
        byte[] utf8 = rid.getBytes(StandardCharsets.UTF_8);
        if (utf8.length > RoutingId.MAX_LENGTH && rid.startsWith("hex:"))
            return RoutingId.fromHex(rid.substring("hex:".length()));
        return RoutingId.from(utf8);
    }

    private static List<ZLinkStorePut> capacityPuts(
            ZLinkStoreKey oldKey, ZLinkMeshNodeDescriptorKey target, byte[] bytes) {
        try {
            JsonNode root = JSON.readTree(bytes);
            if (root == null || !root.isObject())
                throw new IllegalArgumentException("Invalid v11 capacity record");
            var counters = new ArrayList<ZLinkStorePut>();
            for (String state : List.of("active", "pending")) {
                JsonNode usage = root.required(state);
                addCounter(counters, target, "actors", state, usage.required("actors"));
                addCounter(counters, target, "spots", state, usage.required("spots"));
                JsonNode spotTypes = usage.required("spotTypes");
                if (!spotTypes.isObject())
                    throw new IllegalArgumentException("Invalid v11 spotTypes value");
                for (var entry : spotTypes.properties()) {
                    String key = entry.getKey();
                    int separator = key.indexOf('\0');
                    if (separator < 0)
                        throw new IllegalArgumentException("Invalid v11 capacity spot type");
                    var kind =
                            switch (key.substring(0, separator)) {
                                case "user_spot" -> ZLinkPlacementObjectKind.USER_SPOT;
                                case "instance_spot" -> ZLinkPlacementObjectKind.INSTANCE_SPOT;
                                default ->
                                        throw new IllegalArgumentException(
                                                "Invalid v11 capacity spot kind");
                            };
                    addCounter(
                            counters,
                            target,
                            ZLinkCapacityCounters.spotType(kind, key.substring(separator + 1)),
                            state,
                            entry.getValue());
                }
            }
            if (counters.size() >= ZLinkLocationStoreLimits.MAXIMUM_BATCH_KEYS)
                throw new ZLinkConfigurationException(
                        "v11 capacity conversion exceeds the Location Store batch key limit for "
                                + oldKey.value());
            return counters;
        } catch (IOException | IllegalArgumentException failure) {
            throw new ZLinkConfigurationException(
                    "Invalid v11 capacity record " + oldKey.value(), failure);
        }
    }

    private static void addCounter(
            List<ZLinkStorePut> counters,
            ZLinkMeshNodeDescriptorKey target,
            String dimension,
            String state,
            JsonNode value) {
        if (!value.isIntegralNumber() || !value.canConvertToLong() || value.longValue() < 0)
            throw new IllegalArgumentException("Invalid v11 capacity count");
        if (value.longValue() != 0)
            counters.add(
                    new ZLinkStorePut(
                            ZLinkCapacityCounters.key(target, dimension, state),
                            encodeCounter(value.longValue()),
                            null));
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
            throw new IllegalStateException("Location Store counter is invalid", failure);
        }
    }

    private static long generation(ZLinkStoreReadResult result) {
        if (!(result instanceof ZLinkStoreReadFound found)) return 1L;
        try {
            return decodeGeneration(found.value().bytes());
        } catch (IllegalStateException invalid) {
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
