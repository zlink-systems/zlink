package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.locations.ZLinkLocationOptions;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.configuration.ZLinkDispatchOptionsRegistration;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.internal.spots.ZLinkInstanceSpotCallRuntime;
import systems.zlink.framework.runtime.locations.ZLinkRegisteredLocationStores;
import systems.zlink.framework.runtime.locations.ZLinkServiceAuthorityPayloadCodec;
import systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers;
import systems.zlink.framework.spots.ZLinkStoreSpotHandleResolver;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkInstanceRouteCacheTest {
    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void readyAndCachedInstanceIntentUseTheSameStoreReadsAsOrdinarySpot(boolean request) {
        int[] ordinary = exercise(request, false);
        int[] instance = exercise(request, true);
        assertArrayEquals(new int[] {1, 1}, ordinary);
        assertArrayEquals(
                ordinary, instance, "authority and owner lease reads match ordinary routing");
    }

    private int[] exercise(boolean request, boolean instance) {
        var reads = new AtomicInteger();
        var leases = new AtomicInteger();
        var node = RoutingId.from("node");
        var now = Instant.now();
        var snapshot =
                new ZLinkAuthoritySnapshot(
                        "v1",
                        new ZLinkServiceAuthorityPayloadCodec()
                                .encodeInstance(
                                        ZLinkServiceAuthorityPayloadCodec.State.READY,
                                        "room",
                                        "spot",
                                        "owner",
                                        1,
                                        "mesh",
                                        node,
                                        1),
                        1,
                        1,
                        "owner",
                        1,
                        new ZLinkPlacementAllocation(
                                ZLinkPlacementAllocationState.ACTIVE,
                                ZLinkPlacementObjectKind.INSTANCE_SPOT,
                                "room",
                                new ZLinkMeshNodeDescriptorKey("mesh", node),
                                1,
                                ZLinkPlacementCapacityBundle.spot(
                                        ZLinkPlacementObjectKind.INSTANCE_SPOT, "room", 1)),
                        Optional.empty(),
                        now);
        var store =
                (ZLinkLocationRepository)
                        Proxy.newProxyInstance(
                                getClass().getClassLoader(),
                                new Class<?>[] {ZLinkLocationRepository.class},
                                (proxy, method, args) ->
                                        switch (method.getName()) {
                                            case "read" -> {
                                                reads.incrementAndGet();
                                                yield CompletableFuture.completedFuture(snapshot);
                                            }
                                            case "readOwnerLease" -> {
                                                leases.incrementAndGet();
                                                yield CompletableFuture.completedFuture(
                                                        new ZLinkOwnerLeaseFound(
                                                                new ZLinkLocationOwnerToken(
                                                                        "owner", 1),
                                                                now.plusSeconds(30),
                                                                now));
                                            }
                                            default -> throw new AssertionError(method.getName());
                                        });
        var options = new ZLinkLocationOptions();
        options.setRouteCacheMaxAge(Duration.ofSeconds(10));
        var routes =
                new ZLinkStoreLocationResolvers(
                        ZLinkRegisteredLocationStores.fromUnified(store), options);
        var resolver =
                new ZLinkStoreSpotHandleResolver(
                        new ZLinkStoreLocationResolvers.AddressResolvers(List.of("mesh"), routes));
        var terminal = new IllegalStateException("transport terminal");
        var activation =
                (ZLinkInstanceSpotCallRuntime)
                        Proxy.newProxyInstance(
                                getClass().getClassLoader(),
                                new Class<?>[] {ZLinkInstanceSpotCallRuntime.class},
                                (proxy, method, args) ->
                                        switch (method.getName()) {
                                            case "activationDeadline" -> 0L;
                                            case "metricMeshName" -> "mesh";
                                            case "send", "request" -> {
                                                assertEquals(9, args.length);
                                                var ready =
                                                        assertInstanceOf(
                                                                systems.zlink.framework.runtime
                                                                        .internal.spots
                                                                        .SpotTransportAddress.class,
                                                                args[8]);
                                                assertEquals("owner", ready.ownerId());
                                                assertEquals("v1", ready.storeVersion());
                                                assertEquals("room", ready.stableType());
                                                yield CompletableFuture.failedFuture(terminal);
                                            }
                                            default ->
                                                    throw new AssertionError(
                                                            "Ready route entered coordinator: "
                                                                    + method.getName());
                                        });
        var scheduler = Executors.newSingleThreadScheduledExecutor();
        var runtime =
                new ZLinkChannelCallRuntime(
                        new ZLinkMessageFlowTracer(
                                new ZLinkDispatchOptionsRegistration(), null, Runnable::run),
                        scheduler,
                        null,
                        (channel, target, spot, generation, authority, lease, parts) ->
                                CompletableFuture.failedFuture(terminal),
                        (channel,
                                target,
                                spot,
                                generation,
                                authority,
                                lease,
                                parts,
                                timeout,
                                registry,
                                operation) -> CompletableFuture.failedFuture(terminal));
        try {
            for (int i = 0; i < 2; i++) {
                try (var payload = Message.from(new byte[] {1})) {
                    java.util.concurrent.CompletionStage<?> submitted;
                    if (request) {
                        var call =
                                new RouteSpotRequestCall(
                                        runtime,
                                        "channel",
                                        "mesh",
                                        resolver,
                                        () -> activation,
                                        "spot",
                                        payload,
                                        Optional.of("Probe"),
                                        Duration.ofSeconds(1));
                        submitted =
                                (instance ? call.instanceSpot("room") : call).submit(String.class);
                    } else {
                        var call =
                                new RouteSpotSendCall(
                                        runtime,
                                        "channel",
                                        resolver,
                                        () -> activation,
                                        "spot",
                                        payload,
                                        Optional.of("Probe"));
                        submitted = (instance ? call.instanceSpot("room") : call).submit();
                    }
                    assertSame(
                            terminal,
                            assertThrows(
                                            CompletionException.class,
                                            () -> submitted.toCompletableFuture().join())
                                    .getCause());
                    assertEquals(1, reads.get());
                    assertEquals(1, leases.get());
                }
            }
            return new int[] {reads.get(), leases.get()};
        } finally {
            runtime.beginClose();
            scheduler.shutdownNow();
            routes.close();
        }
    }
}
