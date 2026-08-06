package systems.zlink.framework.runtime.locations;

import java.time.Instant;
import java.nio.charset.StandardCharsets;
import java.util.Base64;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;

/** Builds operational projections from descriptors and durable authority. */
public final class ZLinkLocationRuntimeQueryService implements ZLinkLocationRuntimeQuery {
    private static final int MAX_OBJECT_PAGE_BYTES = 4 * 1024 * 1024;
    private static final int MAX_CONTINUATION_TOKEN_BYTES = 4096;
    private static final int OBJECT_JSON_FIXED_BYTES =
        "{\"globalId\":,\"objectGeneration\":,\"meshName\":,\"nodeRid\":,\"state\":,\"stableType\":}"
            .getBytes(StandardCharsets.UTF_8).length;
    private static final ZLinkStoreCancellation OPEN = () -> false;
    private final ZLinkRegisteredLocationStores stores;
    private final ZLinkLocationRuntime runtime;
    private final ZLinkLocationOptions options;
    private final List<String> meshNames;
    private final ZLinkLiveLocationRows liveRows;
    private final ConcurrentHashMap<String, ObjectLocationSnapshot> objectSnapshots =
        new ConcurrentHashMap<>();

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
        return CompletableFuture.completedFuture(new ZLinkLocationRuntimeStatus(
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
        ZLinkLocationTopologyFilter filter,
        ZLinkPageRequest page) {
        ZLinkLocationTopologyFilter safe = filter == null
            ? ZLinkLocationTopologyFilter.all() : filter;
        return meshEntries(safe)
            .thenApply(items -> pageInMemory(items, page));
    }

    @Override
    public CompletionStage<ZLinkLocationPage<ZLinkLocationServiceSummary>> listServiceSummaries(
        ZLinkLocationServiceSummaryFilter filter,
        ZLinkPageRequest page) {
        ZLinkLocationServiceSummaryFilter safe = filter == null
            ? ZLinkLocationServiceSummaryFilter.all() : filter;
        return meshEntries(new ZLinkLocationTopologyFilter(
            safe.meshName(), null, null)).thenApply(nodes -> {
            Map<String, MutableSummary> grouped = new LinkedHashMap<>();
            nodes.forEach(node -> grouped
                .computeIfAbsent(node.meshName(), ignored -> new MutableSummary())
                .add(node.updatedAt(), node.state() == ZLinkLocationTopologyState.READY));
            List<ZLinkLocationServiceSummary> summaries = grouped.entrySet().stream()
                .map(entry -> entry.getValue().toSummary(entry.getKey()))
                .sorted(Comparator.comparing(ZLinkLocationServiceSummary::meshName))
                .toList();
            return pageInMemory(summaries, page);
        });
    }

    @Override
    public CompletionStage<Optional<ZLinkLocationObjectEntry>> findActorLocation(String actorId) {
        return findObjectLocation(ZLinkAuthorityKeyCodec.actor(actorId), ZLinkPlacementObjectKind.ACTOR);
    }

    @Override
    public CompletionStage<Optional<ZLinkLocationObjectEntry>> findSpotLocation(String spotId) {
        return findObjectLocation(ZLinkAuthorityKeyCodec.spot(spotId), null);
    }

    @Override
    public CompletionStage<ZLinkLocationPage<ZLinkLocationObjectEntry>> listObjectLocations(
        ZLinkLocationObjectFilter filter,
        ZLinkPageRequest page) {
        Objects.requireNonNull(filter, "filter");
        ZLinkPageRequest safe = normalize(page);
        int pageSize = safe.pageSize();
        if (pageSize < 1 || pageSize > 1000)
            throw new IllegalArgumentException("pageSize must be in 1..1000");
        String prefix = filter.objectKind() == ZLinkPlacementObjectKind.ACTOR
            ? ZLinkAuthorityKeyCodec.actorPrefix()
            : ZLinkAuthorityKeyCodec.spotPrefix();
        if (safe.continuationToken() == null) {
            return loadObjectSnapshot(filter, prefix)
                .thenApply(entries -> createSnapshotPage(entries, pageSize));
        }
        SnapshotPosition position = parsePosition(safe.continuationToken());
        ObjectLocationSnapshot snapshot = objectSnapshots.get(position.snapshotId());
        if (snapshot == null)
            return CompletableFuture.failedFuture(
                new IllegalArgumentException("location continuation token expired"));
        if (position.index() > snapshot.entries().size())
            return CompletableFuture.failedFuture(
                new IllegalArgumentException("invalid location continuation index"));
        return CompletableFuture.completedFuture(
            pageSnapshot(position.snapshotId(), snapshot.entries(), position.index(), pageSize));
    }

    private CompletionStage<Optional<ZLinkLocationObjectEntry>> findObjectLocation(
        String key,
        ZLinkPlacementObjectKind expectedKind) {
        return stores.unifiedStore().read(key, OPEN).thenApply(result -> {
            if (!(result instanceof ZLinkAuthoritySnapshot snapshot)
                || (expectedKind != null
                    && snapshot.allocation().objectKind() != expectedKind)
                || (expectedKind == null
                    && snapshot.allocation().objectKind() != ZLinkPlacementObjectKind.USER_SPOT
                    && snapshot.allocation().objectKind() != ZLinkPlacementObjectKind.INSTANCE_SPOT))
                return Optional.empty();
            return Optional.of(toObjectEntry(key, snapshot));
        });
    }

    private CompletionStage<List<ZLinkLocationObjectEntry>> loadObjectSnapshot(
        ZLinkLocationObjectFilter filter,
        String prefix,
        Optional<ZLinkAuthorityScanCursor> cursor,
        List<ZLinkLocationObjectEntry> values) {
        return stores.unifiedStore().list(
            prefix,
            cursor,
            1000,
            OPEN)
            .thenCompose(page -> {
                if (!(page instanceof ZLinkAuthorityPage authorityPage))
                    return CompletableFuture.failedFuture(
                        new IllegalStateException("location scan cursor expired"));
                List<ZLinkAuthorityEntry> authorityItems = authorityPage.items();
                for (ZLinkAuthorityEntry entry : authorityItems) {
                    ZLinkAuthoritySnapshot snapshot = entry.snapshot();
                    ZLinkPlacementAllocation allocation = snapshot.allocation();
                    if (allocation.objectKind() != filter.objectKind()
                        || (filter.stableType() != null
                            && !filter.stableType().equals(allocation.stableType()))
                        || (filter.meshName() != null
                            && !filter.meshName().equals(allocation.descriptor().meshName())))
                        continue;
                    values.add(toObjectEntry(entry.key(), snapshot));
                }
                return authorityPage.nextCursor().isPresent()
                    ? loadObjectSnapshot(
                        filter, prefix, authorityPage.nextCursor(), values)
                    : CompletableFuture.completedFuture(List.copyOf(values));
            });
    }

    private CompletionStage<List<ZLinkLocationObjectEntry>> loadObjectSnapshot(
        ZLinkLocationObjectFilter filter,
        String prefix) {
        return loadObjectSnapshot(filter, prefix, Optional.empty(), new ArrayList<>());
    }

    private ZLinkLocationPage<ZLinkLocationObjectEntry> createSnapshotPage(
        List<ZLinkLocationObjectEntry> entries,
        int pageSize) {
        if (entries.isEmpty())
            return new ZLinkLocationPage<>(List.of(), null);
        String snapshotId = Long.toUnsignedString(System.nanoTime(), 36)
            + Long.toUnsignedString(System.identityHashCode(entries), 36);
        objectSnapshots.put(snapshotId, new ObjectLocationSnapshot(entries));
        trimObjectSnapshots();
        return pageSnapshot(snapshotId, entries, 0, pageSize);
    }

    private ZLinkLocationPage<ZLinkLocationObjectEntry> pageSnapshot(
        String snapshotId,
        List<ZLinkLocationObjectEntry> entries,
        int offset,
        int pageSize) {
        List<ZLinkLocationObjectEntry> page = new ArrayList<>(Math.min(pageSize, entries.size() - offset));
        int encodedBytes = 256;
        int index = offset;
        while (index < entries.size() && page.size() < pageSize) {
            ZLinkLocationObjectEntry entry = entries.get(index);
            int entryBytes = encodedObjectBytes(entry);
            if (entryBytes + 256 > MAX_OBJECT_PAGE_BYTES)
                throw new IllegalStateException(
                    "location object entry exceeds the 4 MiB response limit");
            if (!page.isEmpty() && encodedBytes + entryBytes > MAX_OBJECT_PAGE_BYTES - 256)
                break;
            page.add(entry);
            encodedBytes += entryBytes + 1;
            index++;
        }
        if (page.isEmpty())
            throw new IllegalStateException("location object page cannot fit the 4 MiB response limit");
        String token = index < entries.size()
            ? encodePosition(snapshotId, index)
            : null;
        if (token == null)
            objectSnapshots.remove(snapshotId);
        return new ZLinkLocationPage<>(List.copyOf(page), token);
    }

    private void trimObjectSnapshots() {
        while (objectSnapshots.size() > 64) {
            String first = objectSnapshots.keys().nextElement();
            objectSnapshots.remove(first);
        }
    }

    private static int encodedObjectBytes(ZLinkLocationObjectEntry object) {
        return OBJECT_JSON_FIXED_BYTES
            + jsonStringBytes(object.globalId())
            + Long.toString(object.objectGeneration()).getBytes(StandardCharsets.UTF_8).length
            + jsonStringBytes(object.meshName())
            + jsonStringBytes(object.nodeRid().toString())
            + jsonStringBytes(object.state().name())
            + jsonStringBytes(object.stableType());
    }

    private static int jsonStringBytes(String value) {
        int bytes = value.getBytes(StandardCharsets.UTF_8).length + 2;
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i);
            if (c == '"' || c == '\\')
                bytes++;
            else if (c < 0x20)
                bytes += 5;
        }
        return bytes;
    }

    private record ObjectLocationSnapshot(
        List<ZLinkLocationObjectEntry> entries) { }

    private record SnapshotPosition(
        String snapshotId,
        int index) { }

    private static SnapshotPosition parsePosition(String token) {
        if (token == null || token.isBlank())
            throw new IllegalArgumentException("location continuation token is required");
        if (token.length() > MAX_CONTINUATION_TOKEN_BYTES)
            throw new IllegalArgumentException("location continuation token is too large");
        try {
            String value = new String(Base64.getUrlDecoder().decode(token), StandardCharsets.UTF_8);
            int separator = value.lastIndexOf(':');
            if (separator <= 0)
                throw new IllegalArgumentException("invalid location continuation token");
            int index = Integer.parseInt(value.substring(separator + 1));
            if (index < 0)
                throw new IllegalArgumentException("invalid location continuation index");
            return new SnapshotPosition(value.substring(0, separator), index);
        } catch (RuntimeException error) {
            throw new IllegalArgumentException("invalid location continuation token", error);
        }
    }

    private static String encodePosition(
        String snapshotId,
        int index) {
        String value = snapshotId + ':' + index;
        return Base64.getUrlEncoder().withoutPadding()
            .encodeToString(value.getBytes(StandardCharsets.UTF_8));
    }

    private static ZLinkLocationObjectEntry toObjectEntry(
        String authorityKey,
        ZLinkAuthoritySnapshot snapshot) {
        ZLinkPlacementAllocation allocation = snapshot.allocation();
        String globalId = allocation.objectKind() == ZLinkPlacementObjectKind.ACTOR
            ? ZLinkAuthorityKeyCodec.actorId(authorityKey)
            : ZLinkAuthorityKeyCodec.spotId(authorityKey);
        ZLinkLocationObjectState state = allocation.state()
            == ZLinkPlacementAllocationState.PENDING
            ? ZLinkLocationObjectState.CREATING
            : ZLinkLocationObjectState.READY;
        return new ZLinkLocationObjectEntry(
            globalId,
            snapshot.objectGeneration(),
            allocation.descriptor().meshName(),
            allocation.descriptor().rid(),
            state,
            allocation.stableType());
    }

    private CompletionStage<List<ZLinkLocationTopologyEntry>> meshEntries(
        ZLinkLocationTopologyFilter filter) {
        return allMeshNodes(filter.meshName()).thenCompose(nodes -> {
            List<CompletableFuture<ZLinkLocationTopologyEntry>> projections =
                nodes.stream().map(node -> liveRows.ownerLeaseRemaining(
                        node.ownerId(), node.leaseGeneration())
                    .thenApply(remaining -> new ZLinkLocationTopologyEntry(
                        node.meshName(),
                        node.rid(),
                        node.endpoint(),
                        node.state() == systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState.DRAINING,
                        remaining == null
                            ? ZLinkLocationTopologyState.LOST
                            : ZLinkLocationTopologyState.READY,
                        node.updatedAt()))
                    .toCompletableFuture()).toList();
            return CompletableFuture.allOf(
                    projections.toArray(CompletableFuture[]::new))
                .thenApply(ignored -> projections.stream()
                    .map(CompletableFuture::join)
                    .filter(entry -> matches(entry, filter))
                    .toList());
        });
    }

    private CompletionStage<List<ZLinkMeshNodeDescriptor>> allMeshNodes(String meshName) {
        if (meshName == null || meshName.isBlank()) {
            CompletionStage<List<ZLinkMeshNodeDescriptor>> values =
                CompletableFuture.completedFuture(List.of());
            for (String configured : meshNames) {
                values = values.thenCombine(
                    allMeshNodes(configured, null, new ArrayList<>()),
                    ZLinkLocationRuntimeQueryService::concat);
            }
            return values;
        }
        return allMeshNodes(meshName, null, new ArrayList<>());
    }

    private CompletionStage<List<ZLinkMeshNodeDescriptor>> allMeshNodes(
        String meshName,
        String continuation,
        List<ZLinkMeshNodeDescriptor> values) {
        return stores.unifiedStore().listMeshNodes(
            meshName, new ZLinkPageRequest(1000, continuation)).thenCompose(page -> {
                values.addAll(page.items());
                return page.continuationToken() == null
                    ? CompletableFuture.completedFuture(List.copyOf(values))
                    : allMeshNodes(meshName, page.continuationToken(), values);
            });
    }

    private static boolean matches(
        ZLinkLocationTopologyEntry entry,
        ZLinkLocationTopologyFilter filter) {
        return (filter.meshName() == null || Objects.equals(entry.meshName(), filter.meshName()))
            && (filter.nodeRid() == null || Objects.equals(entry.nodeRid(), filter.nodeRid()))
            && (filter.state() == null || entry.state() == filter.state());
    }

    private static <T> List<T> concat(List<T> left, List<T> right) {
        ArrayList<T> result = new ArrayList<>(left.size() + right.size());
        result.addAll(left);
        result.addAll(right);
        return List.copyOf(result);
    }

    private <T> ZLinkLocationPage<T> pageInMemory(List<T> rows, ZLinkPageRequest page) {
        ZLinkPageRequest safe = normalize(page);
        int offset = parseOffset(safe.continuationToken());
        int limit = safe.pageSize();
        List<T> items = rows.stream().skip(offset).limit(limit).toList();
        int nextOffset = offset + items.size();
        return new ZLinkLocationPage<>(items, nextOffset < rows.size() ? Integer.toString(nextOffset) : null);
    }

    private ZLinkPageRequest normalize(ZLinkPageRequest page) {
        ZLinkPageRequest safe = page == null ? ZLinkPageRequest.firstPage() : page;
        return safe.pageSize() > 0 ? safe : new ZLinkPageRequest(1000, safe.continuationToken());
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
            return new ZLinkLocationServiceSummary(meshName, total, ready, total - ready, 0, updatedAt);
        }
    }
}
