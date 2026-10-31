package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.ZLinkLocationOptions;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.locations.ZLinkRegisteredLocationStores;
import systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers;

import java.lang.reflect.Proxy;
import java.time.Instant;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkActorRouteCacheTriggerTest {
    @Test
    void exactAuthorityFailurePreservesCachedRoute() {
        for (boolean actorRef : new boolean[] {false, true}) {
            var current = new AtomicReference<>(row("v1", "target", 1));
            var reads = new AtomicInteger();
            var resolver = resolver(current, reads);
            var coordinator = new ZLinkActorLocationCoordinator(ignored -> "player");
            coordinator.setResolvers(resolver);
            var cached = resolver.resolveActor("actor").toCompletableFuture().join();
            current.set(null);
            assertThrows(
                    CompletionException.class,
                    () -> {
                        if (actorRef)
                            coordinator
                                    .findStoredActorRefExact("actor")
                                    .toCompletableFuture()
                                    .join();
                        else
                            coordinator
                                    .resolveStoredActorRoute("actor")
                                    .toCompletableFuture()
                                    .join();
                    });
            assertSame(cached, resolver.resolveActor("actor").toCompletableFuture().join());
            assertEquals(2, reads.get(), "authority failure must not discard a valid route");
        }
    }

    @Test
    void sessionAuthorityReadSeesOwnerThatReturnedToSameTarget() {
        var current = new AtomicReference<>(row("v1", "target", 1));
        var reads = new AtomicInteger();
        var resolver = resolver(current, reads);
        var coordinator = new ZLinkActorLocationCoordinator(ignored -> "player");
        coordinator.setResolvers(resolver);
        assertEquals(
                1,
                resolver.resolveActor("actor")
                        .toCompletableFuture()
                        .join()
                        .authorityOwnerGeneration());
        current.set(row("v2", "source", 2));
        assertEquals(
                RoutingId.from("source"),
                coordinator
                        .findStoredActorRefExact("actor")
                        .toCompletableFuture()
                        .join()
                        .orElseThrow()
                        .nodeRid());
        current.set(row("v3", "target", 3));
        var latest = coordinator.resolveStoredActorRoute("actor").toCompletableFuture().join();
        assertEquals(3, latest.authorityOwnerGeneration());
        assertEquals(RoutingId.from("target"), latest.nodeRid());
        assertSame(latest, resolver.resolveActor("actor").toCompletableFuture().join());
        assertEquals(3, reads.get());
    }

    private static ZLinkStoreLocationResolvers resolver(
            AtomicReference<ZLinkAuthoritySnapshot> current, AtomicInteger reads) {
        var store =
                (ZLinkLocationRepository)
                        Proxy.newProxyInstance(
                                ZLinkLocationRepository.class.getClassLoader(),
                                new Class<?>[] {ZLinkLocationRepository.class},
                                (proxy, method, arguments) ->
                                        switch (method.getName()) {
                                            case "read" -> {
                                                reads.incrementAndGet();
                                                yield current.get() == null
                                                        ? CompletableFuture.failedFuture(
                                                                new IllegalStateException(
                                                                        "store unavailable"))
                                                        : CompletableFuture.completedFuture(
                                                                current.get());
                                            }
                                            case "readOwnerLease" ->
                                                    CompletableFuture.completedFuture(
                                                            new ZLinkOwnerLeaseFound(
                                                                    new ZLinkLocationOwnerToken(
                                                                            "owner", 1),
                                                                    Instant.now().plusSeconds(60),
                                                                    Instant.now()));
                                            default ->
                                                    throw new UnsupportedOperationException(
                                                            method.getName());
                                        });
        return new ZLinkStoreLocationResolvers(
                ZLinkRegisteredLocationStores.fromUnified(store), new ZLinkLocationOptions());
    }

    private static ZLinkAuthoritySnapshot row(String version, String node, long generation) {
        return new ZLinkAuthoritySnapshot(
                version,
                new byte[0],
                7,
                generation,
                "owner",
                1,
                new ZLinkPlacementAllocation(
                        ZLinkPlacementAllocationState.ACTIVE,
                        ZLinkPlacementObjectKind.ACTOR,
                        "player",
                        new ZLinkMeshNodeDescriptorKey("mesh", RoutingId.from(node)),
                        1,
                        ZLinkPlacementCapacityBundle.actor(1)),
                Instant.now());
    }
}
