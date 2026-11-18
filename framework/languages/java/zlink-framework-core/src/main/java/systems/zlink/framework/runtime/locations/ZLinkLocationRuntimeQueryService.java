package systems.zlink.framework.runtime.locations;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;

import java.nio.charset.StandardCharsets;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Base64;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;

/** Builds operational projections from descriptors and durable authority. */
public final class ZLinkLocationRuntimeQueryService implements ZLinkLocationRuntimeQuery {
    private static final int MAXIMUM_QUERY_PAGE_ITEMS = 1000;
    private static final int DESCRIPTOR_SCAN_PAGE_ITEMS = 1000;
    private static final int MAX_OBJECT_PAGE_BYTES = 4 * 1024 * 1024;
    private static final int MAX_CONTINUATION_TOKEN_BYTES = 4096;
    private static final ZLinkStoreCancellation OPEN = () -> false;
    private final ZLinkRegisteredLocationStores stores;
    private final ZLinkLocationRuntime runtime;
    private final ZLinkLocationOptions options;
    private final List<String> meshNames;
    private final ZLinkLiveLocationRows liveRows;

    public ZLinkLocationRuntimeQueryService(
            ZLinkRegisteredLocationStores stores,
            ZLinkLocationRuntime runtime,
            ZLinkLocationOptions options) {
        this.stores = Objects.requireNonNull(stores, "stores");
        this.runtime = Objects.requireNonNull(runtime, "runtime");
        this.options = Objects.requireNonNull(options, "options");
        this.meshNames = List.of();
        this.liveRows = ZLinkLiveLocationRows.create(stores, options);
    }

    public ZLinkLocationRuntimeQueryService(
            ZLinkRegisteredLocationStores stores,
            ZLinkLocationRuntime runtime,
            ZLinkLocationOptions options,
            ZLinkLiveLocationRows liveRows) {
        this.stores = Objects.requireNonNull(stores, "stores");
        this.runtime = Objects.requireNonNull(runtime, "runtime");
        this.options = Objects.requireNonNull(options, "options");
        this.meshNames = List.of();
        this.liveRows = Objects.requireNonNull(liveRows, "liveRows");
    }

    public ZLinkLocationRuntimeQueryService(
            ZLinkRegisteredLocationStores stores,
            ZLinkLocationRuntime runtime,
            ZLinkLocationOptions options,
            ZLinkLiveLocationRows liveRows,
            List<String> meshNames) {
        this.stores = Objects.requireNonNull(stores, "stores");
        this.runtime = Objects.requireNonNull(runtime, "runtime");
        this.options = Objects.requireNonNull(options, "options");
        this.meshNames = List.copyOf(meshNames);
        this.liveRows = Objects.requireNonNull(liveRows, "liveRows");
    }

    @Override
    public CompletionStage<ZLinkLocationRuntimeStatus> getStatus() {
        return CompletableFuture.completedFuture(
                new ZLinkLocationRuntimeStatus(
                        runtime.lastError() == null,
                        false,
                        options.pollingInterval(),
                        runtime.ownerLeaseRenewedAt(),
                        runtime.lastError(),
                        runtime.ownerLeaseHealthy(),
                        runtime.ownerLeaseRenewedAt()));
    }

    @Override
    public CompletionStage<ZLinkLocationPage<ZLinkLocationTopologyEntry>> listTopology(
            ZLinkLocationTopologyFilter filter, ZLinkPageRequest page) {
        ZLinkLocationTopologyFilter safe =
                filter == null ? ZLinkLocationTopologyFilter.all() : filter;
        ZLinkPageRequest safePage = boundedPage(page);
        return unavailable(
                loadTopologyPage(safe, safePage, parseOffset(safePage.continuationToken())),
                "list topology");
    }

    @Override
    public CompletionStage<ZLinkLocationPage<ZLinkLocationServiceSummary>> listServiceSummaries(
            ZLinkLocationServiceSummaryFilter filter, ZLinkPageRequest page) {
        ZLinkLocationServiceSummaryFilter safe =
                filter == null ? ZLinkLocationServiceSummaryFilter.all() : filter;
        ZLinkPageRequest safePage = boundedPage(page);
        return unavailable(
                loadServiceSummaryPage(safe)
                        .thenApply(
                                grouped -> {
                                    List<ZLinkLocationServiceSummary> summaries =
                                            grouped.entrySet().stream()
                                                    .map(
                                                            entry ->
                                                                    entry.getValue()
                                                                            .toSummary(
                                                                                    entry.getKey()))
                                                    .sorted(
                                                            Comparator.comparing(
                                                                    ZLinkLocationServiceSummary
                                                                            ::meshName))
                                                    .toList();
                                    return pageInMemory(summaries, safePage);
                                }),
                "list service summaries");
    }

    @Override
    public CompletionStage<Optional<ZLinkLocationObjectEntry>> findActorLocation(String actorId) {
        return findObjectLocation(
                ZLinkAuthorityKeyCodec.actor(actorId), ZLinkPlacementObjectKind.ACTOR);
    }

    @Override
    public CompletionStage<Optional<ZLinkLocationObjectEntry>> findSpotLocation(String spotId) {
        return findObjectLocation(ZLinkAuthorityKeyCodec.spot(spotId), null);
    }

    @Override
    public CompletionStage<ZLinkLocationPage<ZLinkLocationObjectEntry>> listObjectLocations(
            ZLinkLocationObjectFilter filter, ZLinkPageRequest page) {
        Objects.requireNonNull(filter, "filter");
        ZLinkPageRequest safe = normalize(page);
        int pageSize = safe.pageSize();
        if (pageSize < 1 || pageSize > MAXIMUM_QUERY_PAGE_ITEMS)
            throw new IllegalArgumentException(
                    "pageSize must be in 1.." + MAXIMUM_QUERY_PAGE_ITEMS);
        String prefix =
                filter.objectKind() == ZLinkPlacementObjectKind.ACTOR
                        ? ZLinkAuthorityKeyCodec.actorPrefix()
                        : ZLinkAuthorityKeyCodec.spotPrefix();
        Optional<ZLinkAuthorityScanCursor> cursor =
                safe.continuationToken() == null
                        ? Optional.empty()
                        : Optional.of(
                                new ZLinkAuthorityScanCursor(
                                        parseCursor(safe.continuationToken())));
        return unavailable(
                loadObjectPage(
                        filter, prefix, cursor, pageSize, new ArrayList<>(), 0, new ArrayList<>()),
                "list object locations");
    }

    private CompletionStage<Optional<ZLinkLocationObjectEntry>> findObjectLocation(
            String key, ZLinkPlacementObjectKind expectedKind) {
        return unavailable(
                stores.unifiedStore()
                        .read(key, OPEN)
                        .thenCompose(
                                result -> {
                                    if (!(result instanceof ZLinkAuthoritySnapshot snapshot)
                                            || (expectedKind != null
                                                    && snapshot.allocation().objectKind()
                                                            != expectedKind)
                                            || (expectedKind == null
                                                    && snapshot.allocation().objectKind()
                                                            != ZLinkPlacementObjectKind.USER_SPOT
                                                    && snapshot.allocation().objectKind()
                                                            != ZLinkPlacementObjectKind
                                                                    .INSTANCE_SPOT))
                                        return CompletableFuture.completedFuture(Optional.empty());
                                    return toObjectEntry(key, snapshot).thenApply(Optional::of);
                                }),
                "find object location");
    }

    private CompletionStage<ZLinkLocationPage<ZLinkLocationObjectEntry>> loadObjectPage(
            ZLinkLocationObjectFilter filter,
            String prefix,
            Optional<ZLinkAuthorityScanCursor> cursor,
            int pageSize,
            List<ZLinkLocationObjectEntry> values,
            int encodedBytes,
            List<BatchPosition> itemPositions) {
        return stores.unifiedStore()
                .list(prefix, cursor, pageSize - values.size(), OPEN)
                .thenComposeAsync(
                        page -> {
                            if (!(page instanceof ZLinkAuthorityPage batch))
                                return CompletableFuture.failedFuture(
                                        new IllegalStateException("location scan cursor expired"));
                            // Carry the existing byte total through sequential asynchronous
                            // projections.
                            int[] bytes = {encodedBytes};
                            CompletionStage<Integer> consumed =
                                    CompletableFuture.completedFuture(0);
                            for (int index = 0; index < batch.items().size(); index++) {
                                final int row = index;
                                consumed =
                                        consumed.thenCompose(
                                                used -> {
                                                    if (used != row || values.size() >= pageSize)
                                                        return CompletableFuture.completedFuture(
                                                                used);
                                                    var entry = batch.items().get(row);
                                                    var allocation = entry.snapshot().allocation();
                                                    if (allocation.objectKind()
                                                                    != filter.objectKind()
                                                            || (filter.stableType() != null
                                                                    && !filter.stableType()
                                                                            .equals(
                                                                                    allocation
                                                                                            .stableType()))
                                                            || (filter.meshName() != null
                                                                    && !filter.meshName()
                                                                            .equals(
                                                                                    allocation
                                                                                            .descriptor()
                                                                                            .meshName())))
                                                        return CompletableFuture.completedFuture(
                                                                used + 1);
                                                    return toObjectEntry(
                                                                    entry.key(), entry.snapshot())
                                                            .thenApply(
                                                                    projected -> {
                                                                        int entryBytes =
                                                                                encodedObjectBytes(
                                                                                        projected);
                                                                        String continuation =
                                                                                batch.nextCursor()
                                                                                        .map(
                                                                                                next ->
                                                                                                        encodeCursor(
                                                                                                                next
                                                                                                                        .encoded()))
                                                                                        .orElse(
                                                                                                null);
                                                                        int candidateItemsBytes =
                                                                                bytes[0]
                                                                                        + entryBytes;
                                                                        if (!encodedObjectPageFits(
                                                                                candidateItemsBytes,
                                                                                values.size() + 1,
                                                                                continuation))
                                                                            return used;
                                                                        values.add(projected);
                                                                        itemPositions.add(
                                                                                new BatchPosition(
                                                                                        cursor,
                                                                                        row + 1));
                                                                        bytes[0] =
                                                                                candidateItemsBytes;
                                                                        return used + 1;
                                                                    });
                                                });
                            }
                            return consumed.thenCompose(
                                    used -> {
                                        if (used < batch.items().size()) {
                                            if (used == 0)
                                                return CompletableFuture.completedFuture(
                                                        new ZLinkLocationPage<>(
                                                                List.copyOf(values),
                                                                cursor.map(
                                                                                next ->
                                                                                        encodeCursor(
                                                                                                next
                                                                                                        .encoded()))
                                                                        .orElse(null)));
                                            return stores.unifiedStore()
                                                    .list(prefix, cursor, used, OPEN)
                                                    .thenCompose(
                                                            read -> {
                                                                if (!(read
                                                                        instanceof
                                                                        ZLinkAuthorityPage partial))
                                                                    return CompletableFuture
                                                                            .failedFuture(
                                                                                    new IllegalStateException(
                                                                                            "location"
                                                                                                    + " scan"
                                                                                                    + " cursor"
                                                                                                    + " expired"));
                                                                return fitObjectPage(
                                                                        prefix,
                                                                        values,
                                                                        bytes[0],
                                                                        itemPositions,
                                                                        partial.nextCursor(),
                                                                        cursor);
                                                            });
                                        }
                                        if (values.size() >= pageSize
                                                || batch.nextCursor().isEmpty())
                                            return CompletableFuture.completedFuture(
                                                    new ZLinkLocationPage<>(
                                                            List.copyOf(values),
                                                            batch.nextCursor()
                                                                    .map(
                                                                            next ->
                                                                                    encodeCursor(
                                                                                            next
                                                                                                    .encoded()))
                                                                    .orElse(null)));
                                        return loadObjectPage(
                                                filter,
                                                prefix,
                                                batch.nextCursor(),
                                                pageSize,
                                                values,
                                                bytes[0],
                                                itemPositions);
                                    });
                        });
    }

    private CompletionStage<ZLinkLocationPage<ZLinkLocationObjectEntry>> fitObjectPage(
            String prefix,
            List<ZLinkLocationObjectEntry> values,
            int encodedBytes,
            List<BatchPosition> itemPositions,
            Optional<ZLinkAuthorityScanCursor> finalCursor,
            Optional<ZLinkAuthorityScanCursor> batchStart) {
        String token = finalCursor.map(next -> encodeCursor(next.encoded())).orElse(null);
        if (encodedObjectPageFits(encodedBytes, values.size(), token))
            return CompletableFuture.completedFuture(
                    new ZLinkLocationPage<>(List.copyOf(values), token));
        if (itemPositions.isEmpty())
            return CompletableFuture.completedFuture(
                    new ZLinkLocationPage<>(
                            List.of(),
                            batchStart.map(next -> encodeCursor(next.encoded())).orElse(null)));
        var removed = values.remove(values.size() - 1);
        encodedBytes -= encodedObjectBytes(removed);
        int remainingBytes = encodedBytes;
        var position = itemPositions.remove(itemPositions.size() - 1);
        if (values.isEmpty())
            return CompletableFuture.failedFuture(
                    new IllegalStateException("encoded object location entry exceeds 4 MiB"));
        CompletionStage<Optional<ZLinkAuthorityScanCursor>> priorCursor =
                position.rows() == 1
                        ? CompletableFuture.completedFuture(position.start())
                        : stores.unifiedStore()
                                .list(prefix, position.start(), position.rows() - 1, OPEN)
                                .thenApply(
                                        read -> {
                                            if (!(read instanceof ZLinkAuthorityPage page))
                                                throw new IllegalStateException(
                                                        "location scan cursor expired");
                                            return page.nextCursor();
                                        });
        return priorCursor.thenComposeAsync(
                previous ->
                        fitObjectPage(
                                prefix,
                                values,
                                remainingBytes,
                                itemPositions,
                                previous,
                                batchStart));
    }

    private record BatchPosition(Optional<ZLinkAuthorityScanCursor> start, int rows) {}

    static boolean encodedObjectPageFits(
            int encodedItemsBytes, int itemCount, String continuationToken) {
        return "{\"items\":[".getBytes(StandardCharsets.UTF_8).length
                        + encodedItemsBytes
                        + Math.max(0, itemCount - 1)
                        + "],\"continuationToken\":".getBytes(StandardCharsets.UTF_8).length
                        + (continuationToken == null ? 4 : jsonStringBytes(continuationToken))
                        + 1
                <= MAX_OBJECT_PAGE_BYTES;
    }

    static int encodedObjectBytes(ZLinkLocationObjectEntry object) {
        return "{\"globalId\":".getBytes(StandardCharsets.UTF_8).length
                + jsonStringBytes(object.globalId())
                + ",\"objectGeneration\":".getBytes(StandardCharsets.UTF_8).length
                + Long.toString(object.objectGeneration()).length()
                + ",\"meshName\":".getBytes(StandardCharsets.UTF_8).length
                + jsonStringBytes(object.meshName())
                + ",\"nodeRid\":".getBytes(StandardCharsets.UTF_8).length
                + jsonStringBytes(object.nodeRid().toString())
                + ",\"state\":".getBytes(StandardCharsets.UTF_8).length
                + jsonStringBytes(object.state().name())
                + ",\"stableType\":".getBytes(StandardCharsets.UTF_8).length
                + jsonStringBytes(object.stableType())
                + 1;
    }

    private static int jsonStringBytes(String value) {
        int bytes = value.getBytes(StandardCharsets.UTF_8).length + 2;
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i);
            if (c == '"' || c == '\\') bytes++;
            else if (c < 0x20)
                bytes += c == '\b' || c == '\t' || c == '\n' || c == '\f' || c == '\r' ? 1 : 5;
        }
        return bytes;
    }

    private static String parseCursor(String token) {
        if (token == null || token.isBlank())
            throw new IllegalArgumentException("location continuation token is required");
        if (token.length() > MAX_CONTINUATION_TOKEN_BYTES)
            throw new IllegalArgumentException("location continuation token is too large");
        try {
            String value = new String(Base64.getUrlDecoder().decode(token), StandardCharsets.UTF_8);
            if (value.isEmpty())
                throw new IllegalArgumentException("invalid location continuation token");
            return value;
        } catch (RuntimeException error) {
            throw new IllegalArgumentException("invalid location continuation token", error);
        }
    }

    private static String encodeCursor(String value) {
        return Base64.getUrlEncoder()
                .withoutPadding()
                .encodeToString(value.getBytes(StandardCharsets.UTF_8));
    }

    private CompletionStage<ZLinkLocationObjectEntry> toObjectEntry(
            String authorityKey, ZLinkAuthoritySnapshot snapshot) {
        ZLinkPlacementAllocation allocation = snapshot.allocation();
        String globalId =
                allocation.objectKind() == ZLinkPlacementObjectKind.ACTOR
                        ? ZLinkAuthorityKeyCodec.actorId(authorityKey)
                        : ZLinkAuthorityKeyCodec.spotId(authorityKey);
        if (allocation.state() == ZLinkPlacementAllocationState.PENDING) {
            return CompletableFuture.completedFuture(
                    new ZLinkLocationObjectEntry(
                            globalId,
                            snapshot.objectGeneration(),
                            allocation.descriptor().meshName(),
                            allocation.descriptor().rid(),
                            ZLinkLocationObjectState.CREATING,
                            allocation.stableType()));
        }
        return liveRows.ownerLeaseRemaining(snapshot.ownerId(), snapshot.ownerLeaseGeneration())
                .thenApply(
                        remaining ->
                                new ZLinkLocationObjectEntry(
                                        globalId,
                                        snapshot.objectGeneration(),
                                        allocation.descriptor().meshName(),
                                        allocation.descriptor().rid(),
                                        remaining == null
                                                ? ZLinkLocationObjectState.UNAVAILABLE
                                                : ZLinkLocationObjectState.READY,
                                        allocation.stableType()));
    }

    private static <T> CompletionStage<T> unavailable(
            CompletionStage<T> operation, String description) {
        CompletableFuture<T> result = new CompletableFuture<>();
        operation.whenComplete(
                (value, failure) -> {
                    if (failure == null) {
                        result.complete(value);
                        return;
                    }
                    Throwable cause = failure;
                    while (cause instanceof CompletionException && cause.getCause() != null) {
                        cause = cause.getCause();
                    }
                    if (cause instanceof ZLinkFrameworkException) {
                        result.completeExceptionally(cause);
                    } else {
                        result.completeExceptionally(
                                new ZLinkFrameworkException(
                                        ZLinkFrameworkErrorKind.UNAVAILABLE,
                                        "location store failed to " + description,
                                        cause));
                    }
                });
        return result;
    }

    private CompletionStage<ZLinkLocationPage<ZLinkLocationTopologyEntry>> loadTopologyPage(
            ZLinkLocationTopologyFilter filter, ZLinkPageRequest page, int offset) {
        List<ZLinkLocationTopologyEntry> values = new ArrayList<>();
        return scanTopologyMesh(
                queryMeshes(filter.meshName()),
                0,
                null,
                filter,
                page.pageSize(),
                offset,
                new int[] {offset},
                values);
    }

    private CompletionStage<ZLinkLocationPage<ZLinkLocationTopologyEntry>> scanTopologyMesh(
            List<String> meshes,
            int meshIndex,
            String continuation,
            ZLinkLocationTopologyFilter filter,
            int pageSize,
            int originalOffset,
            int[] remainingOffset,
            List<ZLinkLocationTopologyEntry> values) {
        if (meshIndex >= meshes.size()) {
            return CompletableFuture.completedFuture(
                    new ZLinkLocationPage<>(List.copyOf(values), null));
        }
        String meshName = meshes.get(meshIndex);
        return stores.unifiedStore()
                .listMeshNodes(
                        meshName, new ZLinkPageRequest(DESCRIPTOR_SCAN_PAGE_ITEMS, continuation))
                .thenComposeAsync(
                        page ->
                                scanTopologyItems(
                                        meshes,
                                        meshIndex,
                                        page,
                                        0,
                                        filter,
                                        pageSize,
                                        originalOffset,
                                        remainingOffset,
                                        values));
    }

    private CompletionStage<ZLinkLocationPage<ZLinkLocationTopologyEntry>> scanTopologyItems(
            List<String> meshes,
            int meshIndex,
            ZLinkLocationPage<ZLinkMeshNodeDescriptor> page,
            int itemIndex,
            ZLinkLocationTopologyFilter filter,
            int pageSize,
            int originalOffset,
            int[] remainingOffset,
            List<ZLinkLocationTopologyEntry> values) {
        if (itemIndex >= page.items().size()) {
            return page.continuationToken() == null
                    ? scanTopologyMesh(
                            meshes,
                            meshIndex + 1,
                            null,
                            filter,
                            pageSize,
                            originalOffset,
                            remainingOffset,
                            values)
                    : scanTopologyMesh(
                            meshes,
                            meshIndex,
                            page.continuationToken(),
                            filter,
                            pageSize,
                            originalOffset,
                            remainingOffset,
                            values);
        }
        ZLinkMeshNodeDescriptor node = page.items().get(itemIndex);
        return liveRows.ownerLeaseRemaining(node.ownerId(), node.leaseGeneration())
                .thenComposeAsync(
                        remaining -> {
                            ZLinkLocationTopologyEntry entry =
                                    new ZLinkLocationTopologyEntry(
                                            node.meshName(),
                                            node.rid(),
                                            node.endpoint(),
                                            node.state()
                                                    == systems.zlink.framework.runtime.host
                                                            .ZLinkFrameworkRuntimeState.DRAINING,
                                            remaining == null
                                                    ? ZLinkLocationTopologyState.LOST
                                                    : ZLinkLocationTopologyState.READY,
                                            node.updatedAt());
                            if (matches(entry, filter)) {
                                if (remainingOffset[0] > 0) {
                                    remainingOffset[0]--;
                                } else {
                                    values.add(entry);
                                }
                            }
                            if (values.size() >= pageSize) {
                                boolean more =
                                        itemIndex + 1 < page.items().size()
                                                || page.continuationToken() != null
                                                || meshIndex + 1 < meshes.size();
                                return CompletableFuture.completedFuture(
                                        new ZLinkLocationPage<>(
                                                List.copyOf(values),
                                                more
                                                        ? Integer.toString(
                                                                originalOffset + values.size())
                                                        : null));
                            }
                            return scanTopologyItems(
                                    meshes,
                                    meshIndex,
                                    page,
                                    itemIndex + 1,
                                    filter,
                                    pageSize,
                                    originalOffset,
                                    remainingOffset,
                                    values);
                        });
    }

    private CompletionStage<Map<String, MutableSummary>> loadServiceSummaryPage(
            ZLinkLocationServiceSummaryFilter filter) {
        Map<String, MutableSummary> grouped = new LinkedHashMap<>();
        return scanServiceSummaryMesh(queryMeshes(filter.meshName()), 0, null, grouped);
    }

    private CompletionStage<Map<String, MutableSummary>> scanServiceSummaryMesh(
            List<String> meshes,
            int meshIndex,
            String continuation,
            Map<String, MutableSummary> grouped) {
        if (meshIndex >= meshes.size()) {
            return CompletableFuture.completedFuture(grouped);
        }
        String meshName = meshes.get(meshIndex);
        return stores.unifiedStore()
                .listMeshNodes(
                        meshName, new ZLinkPageRequest(DESCRIPTOR_SCAN_PAGE_ITEMS, continuation))
                .thenComposeAsync(
                        page -> scanServiceSummaryItems(meshes, meshIndex, page, 0, grouped));
    }

    private CompletionStage<Map<String, MutableSummary>> scanServiceSummaryItems(
            List<String> meshes,
            int meshIndex,
            ZLinkLocationPage<ZLinkMeshNodeDescriptor> page,
            int itemIndex,
            Map<String, MutableSummary> grouped) {
        if (itemIndex >= page.items().size()) {
            return page.continuationToken() == null
                    ? scanServiceSummaryMesh(meshes, meshIndex + 1, null, grouped)
                    : scanServiceSummaryMesh(meshes, meshIndex, page.continuationToken(), grouped);
        }
        ZLinkMeshNodeDescriptor node = page.items().get(itemIndex);
        return liveRows.ownerLeaseRemaining(node.ownerId(), node.leaseGeneration())
                .thenComposeAsync(
                        remaining -> {
                            grouped.computeIfAbsent(
                                            node.meshName(), ignored -> new MutableSummary())
                                    .add(node.updatedAt(), remaining != null);
                            return scanServiceSummaryItems(
                                    meshes, meshIndex, page, itemIndex + 1, grouped);
                        });
    }

    private List<String> queryMeshes(String requestedMesh) {
        if (requestedMesh != null && !requestedMesh.isBlank()) {
            return List.of(requestedMesh);
        }
        return meshNames;
    }

    private static boolean matches(
            ZLinkLocationTopologyEntry entry, ZLinkLocationTopologyFilter filter) {
        return (filter.meshName() == null || Objects.equals(entry.meshName(), filter.meshName()))
                && (filter.nodeRid() == null || Objects.equals(entry.nodeRid(), filter.nodeRid()))
                && (filter.state() == null || entry.state() == filter.state());
    }

    private <T> ZLinkLocationPage<T> pageInMemory(List<T> rows, ZLinkPageRequest page) {
        ZLinkPageRequest safe = normalize(page);
        int offset = parseOffset(safe.continuationToken());
        int limit = safe.pageSize();
        List<T> items = rows.stream().skip(offset).limit(limit).toList();
        int nextOffset = offset + items.size();
        return new ZLinkLocationPage<>(
                items, nextOffset < rows.size() ? Integer.toString(nextOffset) : null);
    }

    private ZLinkPageRequest normalize(ZLinkPageRequest page) {
        ZLinkPageRequest safe = page == null ? ZLinkPageRequest.firstPage() : page;
        return safe.pageSize() > 0
                ? safe
                : new ZLinkPageRequest(
                        ZLinkPageRequest.firstPage().pageSize(), safe.continuationToken());
    }

    private ZLinkPageRequest boundedPage(ZLinkPageRequest page) {
        ZLinkPageRequest safe = normalize(page);
        if (safe.pageSize() > MAXIMUM_QUERY_PAGE_ITEMS) {
            throw new IllegalArgumentException(
                    "pageSize must be in 1.." + MAXIMUM_QUERY_PAGE_ITEMS);
        }
        return safe;
    }

    private static int parseOffset(String token) {
        try {
            return token == null || token.isBlank() ? 0 : Math.max(0, Integer.parseInt(token));
        } catch (NumberFormatException ignored) {
            return 0;
        }
    }

    private static final class MutableSummary {
        private long total;
        private long ready;
        private Instant updatedAt = Instant.EPOCH;

        void add(Instant value, boolean serving) {
            total++;
            if (serving) ready++;
            if (value.isAfter(updatedAt)) updatedAt = value;
        }

        ZLinkLocationServiceSummary toSummary(String meshName) {
            return new ZLinkLocationServiceSummary(
                    meshName, total, ready, 0, total - ready, updatedAt);
        }
    }
}
