package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.*;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddress;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddressResolver;
import systems.zlink.framework.runtime.locations.*;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorOrigin;
import systems.zlink.framework.spots.ZLinkStoreSpotHandleResolver;

import java.lang.reflect.Proxy;
import java.time.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

final class ZLinkInstanceSpotStaleCacheTest {
    private static final String MESH = "stale-cache";
    private static final String TYPE = "cache-instance";
    private static final String SPOT = "cached-spot";
    private static final Duration WAIT = Duration.ofSeconds(5);

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    @SuppressWarnings("unchecked")
    void refusalEvictsBeforeAuthorityReadAndPreservesReadyTerminal(boolean missing)
            throws Exception {
        var current = new AtomicReference<ZLinkAuthorityReadResult>(snapshot("old", 2));
        var reads = new AtomicInteger();
        var invalidations = new AtomicInteger();
        var terminal = ZLinkFrameworkErrorOrigin.ownerFenceRefusal("retired owner");
        var repository =
                (ZLinkLocationRepository)
                        Proxy.newProxyInstance(
                                getClass().getClassLoader(),
                                new Class<?>[] {ZLinkLocationRepository.class},
                                (proxy, method, args) ->
                                        switch (method.getName()) {
                                            case "read" -> {
                                                if (reads.incrementAndGet() == 2) {
                                                    assertEquals(
                                                            1,
                                                            invalidations.get(),
                                                            "evict before the authority read");
                                                }
                                                yield CompletableFuture.completedFuture(
                                                        current.get());
                                            }
                                            case "readOwnerLease" ->
                                                    CompletableFuture.completedFuture(
                                                            new ZLinkOwnerLeaseFound(
                                                                    new ZLinkLocationOwnerToken(
                                                                            (String) args[0], 1),
                                                                    Instant.now().plusSeconds(30),
                                                                    Instant.now()));
                                            case "listMeshNodes" ->
                                                    CompletableFuture.completedFuture(
                                                            new ZLinkLocationPage<>(
                                                                    List.of(descriptor()), null));
                                            default -> throw new AssertionError(method.getName());
                                        });
        var locationOptions = new ZLinkLocationOptions();
        locationOptions.setRouteCacheMaxAge(Duration.ofSeconds(10));
        var routes =
                new ZLinkStoreLocationResolvers(
                        ZLinkRegisteredLocationStores.fromUnified(repository), locationOptions);
        var cachedResolver =
                new ZLinkStoreSpotHandleResolver(
                        new ZLinkStoreLocationResolvers.AddressResolvers(List.of(MESH), routes));
        var resolver =
                new SpotTransportAddressResolver() {
                    public CompletionStage<Optional<SpotTransportAddress>> resolve(String spotId) {
                        return cachedResolver.resolve(spotId);
                    }

                    public void invalidate(String spotId) {
                        invalidations.incrementAndGet();
                        cachedResolver.invalidate(spotId);
                    }
                };
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        options.addRouteMesh(MESH).listen("inproc://cache-" + UUID.randomUUID()).objects().server();
        try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options);
                var payload = Message.from(new byte[] {1})) {
            var host = (ZLinkSpotRuntime) runtime.spotManager();
            var storeField = ZLinkSpotRuntime.class.getDeclaredField("userSpotLocationStore");
            storeField.setAccessible(true);
            var originalStore = storeField.get(host);
            storeField.set(host, repository);
            var nodesField = ZLinkSpotRuntime.class.getDeclaredField("routeMeshNodesByName");
            nodesField.setAccessible(true);
            var nodes = (Map<String, ZLinkInternalMeshNode>) nodesField.get(host);
            var originalNode = nodes.get(MESH);
            var submissions = new AtomicInteger();
            var accepted = new AtomicInteger();
            var requestParts = new AtomicReference<Object>();
            var firstTimeout = new AtomicReference<Duration>();
            var source =
                    (ZLinkInternalMeshNode)
                            Proxy.newProxyInstance(
                                    getClass().getClassLoader(),
                                    new Class<?>[] {ZLinkInternalMeshNode.class},
                                    (proxy, method, args) -> {
                                        if (method.getName().equals("requestInstanceSpot")) {
                                            int submitted = submissions.incrementAndGet();
                                            if (submitted == (missing ? 3 : 2)) {
                                                assertEquals(
                                                        "InstanceRouteFence",
                                                        args[0].getClass().getSimpleName());
                                                accepted.incrementAndGet();
                                                return CompletableFuture.completedFuture(
                                                        List.<Message>of());
                                            }
                                            if (submitted == 1) {
                                                assertEquals(
                                                        "InstanceRouteFence",
                                                        args[0].getClass().getSimpleName());
                                                requestParts.set(args[4]);
                                                firstTimeout.set((Duration) args[5]);
                                                current.set(
                                                        missing
                                                                ? new ZLinkAuthorityMissing(
                                                                        Instant.now())
                                                                : snapshot("new", 3));
                                                return CompletableFuture.failedFuture(terminal);
                                            }
                                            assertTrue(
                                                    missing, "Ready must retain the first refusal");
                                            assertEquals(
                                                    "InstanceColdActivation",
                                                    args[0].getClass().getSimpleName());
                                            assertSame(
                                                    requestParts.get(),
                                                    args[4],
                                                    "cold admission retains the original payload");
                                            assertTrue(
                                                    ((Duration) args[5])
                                                                    .compareTo(firstTimeout.get())
                                                            <= 0,
                                                    "cold admission retains the deadline");
                                            accepted.incrementAndGet();
                                            current.set(snapshot("new", 3));
                                            return CompletableFuture.completedFuture(
                                                    List.<Message>of());
                                        }
                                        try {
                                            return method.invoke(originalNode, args);
                                        } catch (
                                                java.lang.reflect.InvocationTargetException
                                                        failure) {
                                            throw failure.getCause();
                                        }
                                    });
            var substituted = new HashMap<>(nodes);
            substituted.put(MESH, source);
            nodesField.set(host, Map.copyOf(substituted));
            try {
                var ready = resolver.resolve(SPOT).toCompletableFuture().join().orElseThrow();
                assertEquals("old", ready.ownerId());
                var call =
                        host.instanceSpotCalls()
                                .request(
                                        SPOT,
                                        TYPE,
                                        MESH,
                                        payload,
                                        Optional.of("Probe"),
                                        "application/json",
                                        Map.of(),
                                        WAIT,
                                        ready,
                                        resolver,
                                        null);
                if (missing)
                    assertTrue(
                            call.toCompletableFuture()
                                    .get(WAIT.toMillis(), TimeUnit.MILLISECONDS)
                                    .isEmpty());
                else
                    assertSame(
                            terminal,
                            assertThrows(
                                            ExecutionException.class,
                                            () ->
                                                    call.toCompletableFuture()
                                                            .get(
                                                                    WAIT.toMillis(),
                                                                    TimeUnit.MILLISECONDS))
                                    .getCause());
                assertEquals(missing ? 2 : 1, submissions.get());
                assertEquals(missing ? 1 : 0, accepted.get());
                assertEquals(1, invalidations.get());
                int beforeNext = reads.get();
                var next = resolver.resolve(SPOT).toCompletableFuture().join().orElseThrow();
                assertEquals(
                        "new",
                        next.ownerId(),
                        "next independent operation must read the current owner");
                assertEquals(3, next.spotGeneration());
                assertEquals(beforeNext + 1, reads.get());
                host.instanceSpotCalls()
                        .request(
                                SPOT,
                                TYPE,
                                MESH,
                                payload,
                                Optional.of("Probe"),
                                "application/json",
                                Map.of(),
                                WAIT,
                                next,
                                resolver,
                                null)
                        .toCompletableFuture()
                        .get(WAIT.toMillis(), TimeUnit.MILLISECONDS);
                assertEquals(missing ? 3 : 2, submissions.get());
                assertEquals(missing ? 2 : 1, accepted.get());
                assertEquals(
                        1,
                        invalidations.get(),
                        "successful next operation must not invalidate again");
            } finally {
                nodesField.set(host, nodes);
                storeField.set(host, originalStore);
            }
        } finally {
            routes.close();
        }
    }

    private static ZLinkAuthoritySnapshot snapshot(String owner, long generation) {
        var node = RoutingId.from(owner);
        return new ZLinkAuthoritySnapshot(
                owner,
                new ZLinkServiceAuthorityPayloadCodec()
                        .encodeInstance(
                                ZLinkServiceAuthorityPayloadCodec.State.READY,
                                TYPE,
                                SPOT,
                                owner,
                                1,
                                MESH,
                                node,
                                1),
                generation,
                generation,
                owner,
                1,
                new ZLinkPlacementAllocation(
                        ZLinkPlacementAllocationState.ACTIVE,
                        ZLinkPlacementObjectKind.INSTANCE_SPOT,
                        TYPE,
                        new ZLinkMeshNodeDescriptorKey(MESH, node),
                        1,
                        ZLinkPlacementCapacityBundle.spot(
                                ZLinkPlacementObjectKind.INSTANCE_SPOT, TYPE, 1)),
                Optional.empty(),
                Instant.now());
    }

    private static ZLinkMeshNodeDescriptor descriptor() {
        return new ZLinkMeshNodeDescriptor(
                MESH,
                RoutingId.from("new"),
                1,
                1,
                "tcp://127.0.0.1:1",
                Map.of(),
                1,
                List.of(
                        new ZLinkObjectCapability(
                                ZLinkPlacementObjectKind.INSTANCE_SPOT,
                                TYPE,
                                ZLinkObjectMaintenancePolicyKind.SNAPSHOT,
                                true,
                                0)),
                ZLinkMeshNodeObjectRole.SERVER,
                Optional.empty(),
                1,
                new ZLinkPlacementCapacity(
                        new ZLinkCapacityUsage(0, 0, 0),
                        new ZLinkCapacityUsage(0, 0, 8),
                        List.of(
                                new ZLinkSpotTypeCapacity(
                                        ZLinkPlacementObjectKind.INSTANCE_SPOT,
                                        TYPE,
                                        new ZLinkCapacityUsage(0, 0, 8)))),
                new ZLinkActivationConcurrency(0, 8),
                Optional.empty(),
                ZLinkFrameworkRuntimeState.SERVING,
                "security",
                "new",
                1,
                Instant.now());
    }
}
