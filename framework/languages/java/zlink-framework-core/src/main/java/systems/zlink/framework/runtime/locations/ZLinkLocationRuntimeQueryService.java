package systems.zlink.framework.runtime.locations;

import java.time.Instant;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;

/** Builds operational projections from descriptors and durable authority. */
public final class ZLinkLocationRuntimeQueryService implements ZLinkLocationRuntimeQuery {
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
