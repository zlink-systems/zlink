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

    @org.junit.jupiter.api.Test
    void successfulRequestAllocationDoesNotDependOnErrorTracing() throws Exception {
        systems.zlink.framework.testsupport.ChildJvm.runInJvm(
                getClass(),
                List.of("-XX:-UseTLAB", "-XX:-DoEscapeAnalysis", "-XX:TieredStopAtLevel=1"));
    }

    public static void main(String[] args) throws Exception {
        new ZLinkInstanceSpotStaleCacheTest().compareSuccessAllocation();
    }

    private void compareSuccessAllocation() throws Exception {
        long off =
                measureSuccess(systems.zlink.framework.configuration.ZLinkMessageFlowLogMode.OFF);
        long errors =
                measureSuccess(
                        systems.zlink.framework.configuration.ZLinkMessageFlowLogMode.ERRORS);
        assertEquals(off, errors);
    }

    private long measureSuccess(systems.zlink.framework.configuration.ZLinkMessageFlowLogMode level)
            throws Exception {
        return followAuthority("Ready", null, level);
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    @SuppressWarnings("unchecked")
    void refusalEvictsBeforeAuthorityReadAndFollowsCurrentOwner(boolean missing) throws Exception {
        followAuthority(missing ? "Missing" : "Ready");
    }

    private CompletableFuture<Void> recordedBeforeClose;

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void notReadyFailureRetainsSelectedTargetOnce(boolean request) throws Exception {
        var records = new CopyOnWriteArrayList<String>();
        var recorded = new CompletableFuture<Void>();
        var logger =
                java.util.logging.Logger.getLogger(
                        systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer.class
                                .getName());
        var previous = logger.getLevel();
        var handler =
                new java.util.logging.Handler() {
                    public void publish(java.util.logging.LogRecord record) {
                        if (record.getMessage().contains("target_rid=old")) {
                            records.add(record.getMessage());
                            recorded.complete(null);
                        }
                    }

                    public void flush() {}

                    public void close() {}
                };
        logger.addHandler(handler);
        logger.setLevel(java.util.logging.Level.ALL);
        try {
            // The telemetry worker delivers asynchronously and runtime close drops
            // undelivered records, so the record is awaited while the runtime is open.
            recordedBeforeClose = recorded;
            followAuthority("Ready", request);
            String failure =
                    assertDoesNotThrow(
                            () -> {
                                assertEquals(1, records.size());
                                return records.getFirst();
                            });
            assertTrue(failure.contains("reason=stale_target"), failure);
            assertTrue(failure.contains("error_type=ZlinkSubmitException"), failure);
            assertTrue(failure.contains("error_message="), failure);
        } finally {
            logger.removeHandler(handler);
            logger.setLevel(previous);
        }
    }

    void followAuthority(String state) throws Exception {
        followAuthority(state, null);
    }

    @SuppressWarnings("unchecked")
    void followAuthority(String state, Boolean requestFailure) throws Exception {
        followAuthority(state, requestFailure, null);
    }

    @SuppressWarnings("unchecked")
    long followAuthority(
            String state,
            Boolean requestFailure,
            systems.zlink.framework.configuration.ZLinkMessageFlowLogMode allocationLevel)
            throws Exception {
        boolean missing = !state.equals("Ready");
        boolean creating = state.equals("Creating");
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
        if (allocationLevel != null) options.configureDispatch().messageFlow(allocationLevel);
        if (requestFailure != null)
            options.configureDispatch()
                    .messageFlow(
                            systems.zlink.framework.configuration.ZLinkMessageFlowLogMode.NORMAL);
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        options.addRouteMesh(MESH).listen("inproc://cache-" + UUID.randomUUID()).objects().server();
        var submissions = new AtomicInteger();
        var accepted = new AtomicInteger();
        var requestParts = new AtomicReference<Object>();
        var firstTimeout = new AtomicReference<Duration>();
        var backendProvider =
                ZLinkFrameworkRuntimeTestAccess.withMeshNodeDecorator(
                        new systems.zlink.framework.runtime.binding
                                .ZLinkJavaBackendAdapterFactory(),
                        originalNode ->
                                (ZLinkInternalMeshNode)
                                        Proxy.newProxyInstance(
                                                getClass().getClassLoader(),
                                                new Class<?>[] {ZLinkInternalMeshNode.class},
                                                (proxy, method, args) -> {
                                                    if (allocationLevel != null
                                                            && method.getName()
                                                                    .equals(
                                                                            "requestInstanceSpot")) {
                                                        return CompletableFuture.completedFuture(
                                                                List.of());
                                                    }
                                                    if (requestFailure != null
                                                            && (method.getName()
                                                                            .equals(
                                                                                    "requestInstanceSpot")
                                                                    || method.getName()
                                                                            .equals(
                                                                                    "submitInstanceSpotSend"))) {
                                                        submissions.incrementAndGet();
                                                        return CompletableFuture.failedFuture(
                                                                new systems.zlink.contracts.errors
                                                                        .ZlinkSubmitException(
                                                                        systems.zlink.contracts
                                                                                .sockets
                                                                                .SubmitResult
                                                                                .NOT_CONNECTED));
                                                    }
                                                    if (method.getName()
                                                            .equals("requestInstanceSpot")) {
                                                        int submitted =
                                                                submissions.incrementAndGet();
                                                        if (submitted > 1
                                                                && args[0]
                                                                        instanceof
                                                                        systems.zlink.framework
                                                                                        .runtime
                                                                                        .internal
                                                                                        .service
                                                                                        .ZLinkServiceM6BWireCodec
                                                                                        .InstanceRouteFence
                                                                                fence) {
                                                            assertEquals("new", fence.ownerId());
                                                            if (!missing && submitted == 2) {
                                                                assertSame(
                                                                        requestParts.get(),
                                                                        args[4]);
                                                                assertTrue(
                                                                        ((Duration) args[5])
                                                                                        .compareTo(
                                                                                                firstTimeout
                                                                                                        .get())
                                                                                <= 0);
                                                            }
                                                            accepted.incrementAndGet();
                                                            return CompletableFuture
                                                                    .completedFuture(
                                                                            List.<Message>of());
                                                        }
                                                        if (submitted == 1) {
                                                            assertEquals(
                                                                    "InstanceRouteFence",
                                                                    args[0].getClass()
                                                                            .getSimpleName());
                                                            requestParts.set(args[4]);
                                                            firstTimeout.set((Duration) args[5]);
                                                            current.set(
                                                                    creating
                                                                            ? snapshot(
                                                                                    "new", 3, true)
                                                                            : missing
                                                                                    ? new ZLinkAuthorityMissing(
                                                                                            Instant
                                                                                                    .now())
                                                                                    : snapshot(
                                                                                            "new",
                                                                                            3));
                                                            return CompletableFuture.failedFuture(
                                                                    terminal);
                                                        }
                                                        assertTrue(
                                                                missing,
                                                                "Missing or Creating delegates to"
                                                                        + " the activation"
                                                                        + " coordinator");
                                                        assertEquals(
                                                                "InstanceColdActivation",
                                                                args[0].getClass().getSimpleName());
                                                        assertSame(
                                                                requestParts.get(),
                                                                args[4],
                                                                "cold admission retains the"
                                                                        + " original payload");
                                                        assertTrue(
                                                                ((Duration) args[5])
                                                                                .compareTo(
                                                                                        firstTimeout
                                                                                                .get())
                                                                        <= 0,
                                                                "cold admission retains the"
                                                                        + " deadline");
                                                        accepted.incrementAndGet();
                                                        current.set(snapshot("new", 3));
                                                        return CompletableFuture.completedFuture(
                                                                List.<Message>of());
                                                    }
                                                    try {
                                                        return method.invoke(originalNode, args);
                                                    } catch (
                                                            java.lang.reflect
                                                                            .InvocationTargetException
                                                                    failure) {
                                                        throw failure.getCause();
                                                    }
                                                }));
        var locationRuntime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(repository),
                        locationOptions.ownerLeaseTtl(),
                        locationOptions.ownerLeaseRenewInterval());
        var construction =
                new ZLinkFrameworkRuntimeTestAccess.Construction()
                        .spotOperationAuthorityStore(repository)
                        .spotLocationStore(repository)
                        .spotLocationRuntime(locationRuntime)
                        .spotLocationResolvers(routes);
        try (var runtime =
                        ZLinkFrameworkRuntimeTestAccess.startHostWithConstruction(
                                options,
                                backendProvider,
                                systems.zlink.framework.runtime.internal.handlers
                                        .ZLinkHandlerActivator.reflection(),
                                null,
                                construction);
                var payload = Message.from(new byte[] {1})) {
            var host =
                    systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess.spots(
                            runtime);
            var ready = resolver.resolve(SPOT).toCompletableFuture().join().orElseThrow();
            assertEquals("old", ready.ownerId());
            if (allocationLevel != null) {
                var bytes =
                        systems.zlink.framework.testutil.PerCallAllocation.minWindow(
                                () ->
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
                                                        null)
                                                .toCompletableFuture()
                                                .join(),
                                1000,
                                2000);
                System.out.printf(
                        "instance-request success level=%s requests=1000 B/op=%.3f%n",
                        allocationLevel, bytes / 1000.0);
                return bytes;
            }
            if (requestFailure != null) {
                CompletionStage<?> failed;
                if (requestFailure)
                    failed =
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
                else
                    failed =
                            host.instanceSpotCalls()
                                    .send(
                                            SPOT,
                                            TYPE,
                                            MESH,
                                            systems.zlink.framework.runtime.internal.service
                                                    .ZLinkFrameworkMultipartView.snapshot(
                                                    new byte[] {1}),
                                            Optional.of("Probe"),
                                            "application/json",
                                            Map.of(),
                                            System.currentTimeMillis() + 5000,
                                            ready);
                assertThrows(
                        ExecutionException.class,
                        () -> failed.toCompletableFuture().get(5, TimeUnit.SECONDS));
                assertEquals(1, submissions.get());
                if (recordedBeforeClose != null) recordedBeforeClose.get(5, TimeUnit.SECONDS);
                return 0;
            }
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
            assertTrue(
                    call.toCompletableFuture()
                            .get(WAIT.toMillis(), TimeUnit.MILLISECONDS)
                            .isEmpty());
            assertEquals(2, submissions.get());
            assertEquals(1, accepted.get());
            assertEquals(1, invalidations.get());
            int beforeNext = reads.get();
            var next = resolver.resolve(SPOT).toCompletableFuture().join().orElseThrow();
            assertEquals(
                    "new", next.ownerId(), "next independent operation uses the current owner");
            assertEquals(3, next.spotGeneration());
            assertEquals(beforeNext + (missing ? 1 : 0), reads.get());
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
            assertEquals(3, submissions.get());
            assertEquals(2, accepted.get());
            assertEquals(
                    1, invalidations.get(), "successful next operation must not invalidate again");
            return 0;
        } finally {
            routes.close();
        }
    }

    private static ZLinkAuthoritySnapshot snapshot(String owner, long generation) {
        return snapshot(owner, generation, false);
    }

    private static ZLinkAuthoritySnapshot snapshot(
            String owner, long generation, boolean creating) {
        var node = RoutingId.from(owner);
        return new ZLinkAuthoritySnapshot(
                owner,
                new ZLinkServiceAuthorityPayloadCodec()
                        .encodeInstance(
                                creating
                                        ? ZLinkServiceAuthorityPayloadCodec.State.CREATING
                                        : ZLinkServiceAuthorityPayloadCodec.State.READY,
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
                        creating
                                ? ZLinkPlacementAllocationState.PENDING
                                : ZLinkPlacementAllocationState.ACTIVE,
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
