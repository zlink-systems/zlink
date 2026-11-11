package systems.zlink.framework.runtime.locations;

import systems.zlink.framework.locations.ZLinkLocationOptions;
import systems.zlink.framework.runtime.internal.locations.ZLinkClientServerServerDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;

import java.time.Duration;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Function;
import java.util.function.ToLongFunction;
import java.util.stream.IntStream;

/** Applies owner-lease admission to authority-derived runtime projections. */
public final class ZLinkLiveLocationRows {
    private final ZLinkOwnerLeaseTracker leaseTracker;
    private final Duration routeCacheMaxAge;

    public static ZLinkLiveLocationRows create(
            ZLinkRegisteredLocationStores stores, ZLinkLocationOptions options) {
        Objects.requireNonNull(stores, "stores");
        Objects.requireNonNull(options, "options");
        return new ZLinkLiveLocationRows(
                new ZLinkOwnerLeaseTracker(stores.unifiedStore(), options.pollingInterval()),
                options.routeCacheMaxAge());
    }

    ZLinkLiveLocationRows(ZLinkOwnerLeaseTracker leaseTracker, Duration routeCacheMaxAge) {
        this.leaseTracker = Objects.requireNonNull(leaseTracker, "leaseTracker");
        this.routeCacheMaxAge = Objects.requireNonNull(routeCacheMaxAge, "routeCacheMaxAge");
    }

    CompletionStage<List<ZLinkMeshNodeDescriptor>> meshNodes(List<ZLinkMeshNodeDescriptor> rows) {
        return live(
                rows, ZLinkMeshNodeDescriptor::ownerId, ZLinkMeshNodeDescriptor::leaseGeneration);
    }

    public CompletionStage<List<ZLinkClientServerServerDescriptor>> clientServers(
            List<ZLinkClientServerServerDescriptor> rows) {
        return live(
                rows,
                ZLinkClientServerServerDescriptor::ownerId,
                ZLinkClientServerServerDescriptor::leaseGeneration);
    }

    private <T> CompletionStage<List<T>> live(
            List<T> rows, Function<T, String> ownerId, ToLongFunction<T> generation) {
        List<CompletableFuture<Boolean>> liveness =
                rows.stream()
                        .map(
                                row ->
                                        ownerLeaseRemaining(
                                                        ownerId.apply(row),
                                                        generation.applyAsLong(row))
                                                .thenApply(remaining -> remaining != null)
                                                .toCompletableFuture())
                        .toList();
        return CompletableFuture.allOf(liveness.toArray(CompletableFuture[]::new))
                .thenApply(
                        ignored ->
                                IntStream.range(0, rows.size())
                                        .filter(index -> liveness.get(index).join())
                                        .mapToObj(rows::get)
                                        .toList());
    }

    CompletionStage<Duration> ownerLeaseRemaining(String ownerId, long ownerLeaseGeneration) {
        return leaseTracker.remainingAdmissionLifetime(ownerId, ownerLeaseGeneration);
    }

    Duration routeCacheMaxAge() {
        return routeCacheMaxAge;
    }
}
