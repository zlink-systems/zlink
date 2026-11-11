package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.errors.*;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.locations.ZLinkLocationOptions;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.*;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddress;
import systems.zlink.framework.runtime.locations.ZLinkCandidateLeaseFixture;
import systems.zlink.framework.runtime.locations.ZLinkCandidateLeaseFixture.Lease;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.locations.ZLinkRegisteredLocationStores;
import systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.time.Instant;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkInstanceSpotActivationDeadlineTest {
    private static final String MESH = "activation-deadline";
    private static final String TYPE = "deadline-instance";
    private static final Duration ACTIVATION_TIMEOUT = Duration.ofMillis(600);
    private static final Duration WAIT = Duration.ofSeconds(5);

    @Test
    void instanceSendDuringRuntimeDrainRemainsShutdown() throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        options.addRouteMesh(MESH).listen("inproc://drain-" + UUID.randomUUID()).objects().server();
        try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            var host =
                    systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess.spots(
                            runtime);
            host.beginDrain().toCompletableFuture().get(WAIT.toMillis(), TimeUnit.MILLISECONDS);
            var send =
                    runtime.route()
                            .sendToSpot("draining-room", new InitialMessage())
                            .instanceSpot(TYPE)
                            .inMesh(MESH)
                            .submit();
            var failure =
                    assertThrows(
                            ExecutionException.class,
                            () ->
                                    send.toCompletableFuture()
                                            .get(WAIT.toMillis(), TimeUnit.MILLISECONDS));
            var typed = assertInstanceOf(ZLinkFrameworkException.class, failure.getCause());
            assertEquals(ZLinkFrameworkErrorKind.SHUTTING_DOWN, typed.kind());
            try (var probe =
                            systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetricProbe
                                    .install();
                    var metrics =
                            new systems.zlink.framework.runtime.internal.metrics
                                    .ZLinkInstanceSpotMetrics(MESH, TYPE)) {
                metrics.terminal(System.nanoTime(), typed);
                assertEquals(
                        "shutdown",
                        probe.only("zlink.instance_spot.activations").tags().get("outcome"));
            }
        }
    }

    @Test
    void sourceSendsEnvelopeWithoutReservingAndWaitsOnlyForAdmission() throws Exception {
        run(false, true);
    }

    @Test
    void delayedResolveDoesNotRestartWireActivationDeadline() throws Exception {
        run(true, true);
    }

    @Test
    void unspecifiedMeshDefaultUsesFrameworkRequestTimeout() throws Exception {
        run(false, false);
    }

    @Test
    void inferredTypeIgnoresPreparingDescriptors() throws Exception {
        assertTypeSelection(
                List.of(descriptor(TYPE, ZLinkFrameworkRuntimeState.PREPARING)),
                null,
                ZLinkFrameworkErrorKind.NOT_FOUND);
    }

    @Test
    void explicitTypeWithoutServingCapabilityIsNotFound() throws Exception {
        assertTypeSelection(
                List.of(descriptor(TYPE + "-other", ZLinkFrameworkRuntimeState.SERVING)),
                TYPE,
                ZLinkFrameworkErrorKind.NOT_FOUND);
    }

    @ParameterizedTest
    @EnumSource(
            value = Lease.class,
            names = {"MISSING", "EXPIRED", "REPLACED", "WRONG_OWNER", "LIVE", "CORRUPT", "FAILED"})
    void coldActivationUsesLiveCandidates(Lease lease) throws Exception {
        coldActivationUsesLiveCandidates(lease, false);
    }

    @Test
    void liveZeroWeightProviderMakesStaleOnlyPlacementUnavailable() throws Exception {
        coldActivationUsesLiveCandidates(Lease.EXPIRED, true);
    }

    private void coldActivationUsesLiveCandidates(Lease lease, boolean zeroWeightProvider)
            throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        options.addRouteMesh(MESH).listen("inproc://lease-" + UUID.randomUUID()).objects().client();
        if (zeroWeightProvider) {
            assertEquals(1, descriptor().placementWeight());
            assertEquals(
                    0,
                    descriptor(TYPE, ZLinkFrameworkRuntimeState.SERVING, 0, "live-zero")
                            .placementWeight());
        }
        AtomicInteger sends = new AtomicInteger();
        AtomicInteger reads = new AtomicInteger();
        var repository =
                (ZLinkLocationRepository)
                        Proxy.newProxyInstance(
                                ZLinkLocationRepository.class.getClassLoader(),
                                new Class<?>[] {ZLinkLocationRepository.class},
                                (proxy, method, arguments) ->
                                        switch (method.getName()) {
                                            case "read" ->
                                                    CompletableFuture.completedFuture(
                                                            new ZLinkAuthorityMissing(
                                                                    Instant.now()));
                                            case "listMeshNodes" ->
                                                    CompletableFuture.completedFuture(
                                                            new ZLinkLocationPage<>(
                                                                    zeroWeightProvider
                                                                            ? List.of(
                                                                                    descriptor(),
                                                                                    descriptor(
                                                                                            TYPE,
                                                                                            ZLinkFrameworkRuntimeState
                                                                                                    .SERVING,
                                                                                            0,
                                                                                            "live-zero"))
                                                                            : List.of(descriptor()),
                                                                    null));
                                            case "readOwnerLease" -> {
                                                reads.incrementAndGet();
                                                yield ZLinkCandidateLeaseFixture.read(
                                                        "live-zero".equals(arguments[0])
                                                                ? Lease.LIVE
                                                                : lease,
                                                        (String) arguments[0],
                                                        descriptor().leaseGeneration());
                                            }
                                            default ->
                                                    throw new AssertionError(
                                                            "Unexpected source store call: "
                                                                    + method.getName());
                                        });
        var resolvers =
                new ZLinkStoreLocationResolvers(
                        ZLinkRegisteredLocationStores.fromUnified(repository),
                        new ZLinkLocationOptions());
        var construction =
                new ZLinkFrameworkRuntimeTestAccess.Construction()
                        .spotLocationStore(repository)
                        .spotLocationResolvers(resolvers)
                        .channelSpotAddressResolver(
                                spotId -> CompletableFuture.completedFuture(Optional.empty()));
        var backend =
                ZLinkFrameworkRuntimeTestAccess.withMeshNodeDecorator(
                        new ZLinkJavaBackendAdapterFactory(),
                        original ->
                                (systems.zlink.framework.runtime.internal.backend
                                                .ZLinkInternalMeshNode)
                                        Proxy.newProxyInstance(
                                                original.getClass().getClassLoader(),
                                                new Class<?>[] {
                                                    systems.zlink.framework.runtime.internal.backend
                                                            .ZLinkInternalMeshNode.class
                                                },
                                                (proxy, method, arguments) -> {
                                                    if (method.getName()
                                                            .equals("submitInstanceSpotSend")) {
                                                        sends.incrementAndGet();
                                                        return CompletableFuture.completedFuture(
                                                                null);
                                                    }
                                                    try {
                                                        return method.invoke(original, arguments);
                                                    } catch (
                                                            java.lang.reflect
                                                                            .InvocationTargetException
                                                                    failure) {
                                                        throw failure.getCause();
                                                    }
                                                }));
        try (var runtime =
                ZLinkFrameworkRuntimeTestAccess.startWithConstruction(
                        options, backend, ZLinkHandlerActivator.reflection(), null, construction)) {
            var send =
                    runtime.route()
                            .sendToSpot("new-lease-spot", new InitialMessage())
                            .instanceSpot(TYPE)
                            .inMesh(MESH)
                            .submit()
                            .toCompletableFuture();
            if (lease == Lease.LIVE) {
                send.get(WAIT.toMillis(), TimeUnit.MILLISECONDS);
                assertEquals(1, sends.get());
                assertEquals(
                        TYPE,
                        ZLinkSpotRuntimeTestAccess.resolveInstanceType(
                                        systems.zlink.framework.runtime.host
                                                .ZLinkFrameworkRuntimeTestAccess.spots(runtime),
                                        MESH,
                                        null)
                                .toCompletableFuture()
                                .join());
            } else {
                var failure =
                        assertThrows(
                                ExecutionException.class,
                                () -> send.get(WAIT.toMillis(), TimeUnit.MILLISECONDS));
                if (lease == Lease.FAILED)
                    assertSame(ZLinkCandidateLeaseFixture.READ_FAILURE, failure.getCause());
                else
                    assertEquals(
                            lease == Lease.CORRUPT
                                    ? ZLinkFrameworkErrorKind.INTERNAL_FAILURE
                                    : zeroWeightProvider
                                            ? ZLinkFrameworkErrorKind.UNAVAILABLE
                                            : ZLinkFrameworkErrorKind.NOT_FOUND,
                            assertInstanceOf(ZLinkFrameworkException.class, failure.getCause())
                                    .kind());
                assertEquals(0, sends.get());
                var inferred =
                        ZLinkSpotRuntimeTestAccess.resolveInstanceType(
                                        systems.zlink.framework.runtime.host
                                                .ZLinkFrameworkRuntimeTestAccess.spots(runtime),
                                        MESH,
                                        null)
                                .toCompletableFuture();
                if (zeroWeightProvider) assertEquals(TYPE, inferred.join());
                else {
                    var inferenceFailure =
                            assertThrows(
                                    ExecutionException.class,
                                    () -> inferred.get(WAIT.toMillis(), TimeUnit.MILLISECONDS));
                    if (lease == Lease.FAILED)
                        assertSame(
                                ZLinkCandidateLeaseFixture.READ_FAILURE,
                                inferenceFailure.getCause());
                    else
                        assertEquals(
                                lease == Lease.CORRUPT
                                        ? ZLinkFrameworkErrorKind.INTERNAL_FAILURE
                                        : ZLinkFrameworkErrorKind.NOT_FOUND,
                                assertInstanceOf(
                                                ZLinkFrameworkException.class,
                                                inferenceFailure.getCause())
                                        .kind());
                }
            }
            assertTrue(reads.get() > 0, "candidate lease must be read before selection");
        }
    }

    private static void assertTypeSelection(
            List<ZLinkMeshNodeDescriptor> descriptors,
            String requestedType,
            ZLinkFrameworkErrorKind expected)
            throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        options.addRouteMesh(MESH).listen("inproc://type-" + UUID.randomUUID()).objects().server();
        var repository =
                (ZLinkLocationRepository)
                        Proxy.newProxyInstance(
                                ZLinkLocationRepository.class.getClassLoader(),
                                new Class<?>[] {ZLinkLocationRepository.class},
                                (proxy, method, arguments) -> {
                                    if (method.getName().equals("readOwnerLease"))
                                        return ZLinkCandidateLeaseFixture.read(
                                                Lease.LIVE, (String) arguments[0], 1);
                                    if (method.getName().equals("listMeshNodes"))
                                        return CompletableFuture.completedFuture(
                                                new ZLinkLocationPage<>(descriptors, null));
                                    throw new UnsupportedOperationException(method.getName());
                                });
        var construction =
                new ZLinkFrameworkRuntimeTestAccess.Construction()
                        .spotLocationStore(repository)
                        .spotLocationResolvers(
                                new ZLinkStoreLocationResolvers(
                                        ZLinkRegisteredLocationStores.fromUnified(repository),
                                        new ZLinkLocationOptions()));
        try (var runtime =
                ZLinkFrameworkRuntimeTestAccess.startWithConstruction(
                        options,
                        new ZLinkJavaBackendAdapterFactory(),
                        ZLinkHandlerActivator.reflection(),
                        null,
                        construction)) {
            var host =
                    systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess.spots(
                            runtime);
            var result =
                    requestedType == null
                            ? ZLinkSpotRuntimeTestAccess.resolveInstanceType(host, MESH, null)
                            : ZLinkSpotRuntimeTestAccess.selectInstanceSpotTarget(
                                    host, MESH, requestedType);
            var failure =
                    assertThrows(
                            ExecutionException.class,
                            () ->
                                    result.toCompletableFuture()
                                            .get(WAIT.toMillis(), TimeUnit.MILLISECONDS));
            assertEquals(
                    expected,
                    assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
        }
    }

    @SuppressWarnings("unchecked")
    private static void run(boolean delayResolve, boolean meshOverride) throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(meshOverride ? WAIT : ACTIVATION_TIMEOUT);
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        var mesh = options.addRouteMesh(MESH).listen("inproc://deadline-" + UUID.randomUUID());
        mesh.objects().server();
        if (meshOverride) mesh.setDefaultRequestTimeout(ACTIVATION_TIMEOUT);
        var routeResolve = new CompletableFuture<Optional<SpotTransportAddress>>();
        var wireRoute = new CompletableFuture<Object>();
        var admission = new CompletableFuture<Void>();
        var repository =
                (ZLinkLocationRepository)
                        Proxy.newProxyInstance(
                                ZLinkLocationRepository.class.getClassLoader(),
                                new Class<?>[] {ZLinkLocationRepository.class},
                                (proxy, method, arguments) ->
                                        switch (method.getName()) {
                                            case "read" ->
                                                    CompletableFuture.completedFuture(
                                                            new ZLinkAuthorityMissing(
                                                                    Instant.now()));
                                            case "listMeshNodes" ->
                                                    CompletableFuture.completedFuture(
                                                            new ZLinkLocationPage<>(
                                                                    List.of(descriptor()), null));
                                            case "readOwnerLease" ->
                                                    ZLinkCandidateLeaseFixture.read(
                                                            Lease.LIVE,
                                                            (String) arguments[0],
                                                            descriptor().leaseGeneration());
                                            case "reserve" ->
                                                    throw new AssertionError(
                                                            "source must not create a reservation");
                                            default ->
                                                    throw new UnsupportedOperationException(
                                                            method.getName());
                                        });
        var construction =
                new ZLinkFrameworkRuntimeTestAccess.Construction()
                        .spotLocationStore(repository)
                        .spotLocationResolvers(
                                new ZLinkStoreLocationResolvers(
                                        ZLinkRegisteredLocationStores.fromUnified(repository),
                                        new ZLinkLocationOptions()))
                        .channelSpotAddressResolver(spotId -> routeResolve);
        var backendProvider =
                ZLinkFrameworkRuntimeTestAccess.withMeshNodeDecorator(
                        new ZLinkJavaBackendAdapterFactory(),
                        originalNode ->
                                (systems.zlink.framework.runtime.internal.backend
                                                .ZLinkInternalMeshNode)
                                        Proxy.newProxyInstance(
                                                originalNode.getClass().getClassLoader(),
                                                new Class<?>[] {
                                                    systems.zlink.framework.runtime.internal.backend
                                                            .ZLinkInternalMeshNode.class
                                                },
                                                (proxy, method, arguments) -> {
                                                    if (method.getName()
                                                            .equals("submitInstanceSpotSend")) {
                                                        wireRoute.complete(arguments[0]);
                                                        return admission;
                                                    }
                                                    try {
                                                        return method.invoke(
                                                                originalNode, arguments);
                                                    } catch (
                                                            java.lang.reflect
                                                                            .InvocationTargetException
                                                                    failure) {
                                                        throw failure.getCause();
                                                    }
                                                }));
        try (var runtime =
                ZLinkFrameworkRuntimeTestAccess.startWithConstruction(
                        options,
                        backendProvider,
                        ZLinkHandlerActivator.reflection(),
                        null,
                        construction)) {
            try {
                long started = System.currentTimeMillis();
                var send =
                        runtime.route()
                                .sendToSpot("deadline-spot", new InitialMessage())
                                .instanceSpot(TYPE)
                                .inMesh(MESH)
                                .submit()
                                .toCompletableFuture();
                long submitted = System.currentTimeMillis();
                send.whenComplete(
                        (done, failure) -> {
                            if (failure != null) wireRoute.completeExceptionally(failure);
                        });
                if (delayResolve) waitUntil(submitted + ACTIVATION_TIMEOUT.toMillis() + 100);
                routeResolve.complete(Optional.empty());
                Object route = wireRoute.get(WAIT.toMillis(), TimeUnit.MILLISECONDS);
                assertEquals("InstanceColdActivation", route.getClass().getSimpleName());
                long deadline = (long) route.getClass().getMethod("deadlineUnixMs").invoke(route);
                assertTrue(deadline >= started + ACTIVATION_TIMEOUT.toMillis());
                assertTrue(deadline <= submitted + ACTIVATION_TIMEOUT.toMillis());
                waitUntil(submitted + ACTIVATION_TIMEOUT.toMillis() + 100);
                assertFalse(
                        send.isDone(),
                        "target activation deadline must not terminate pending outbound admission");
                admission.complete(null);
                send.get(WAIT.toMillis(), TimeUnit.MILLISECONDS);
            } finally {
                admission.complete(null);
                routeResolve.complete(Optional.empty());
            }
        }
    }

    private static void waitUntil(long deadline) throws Exception {
        CompletableFuture.runAsync(
                        () -> {},
                        CompletableFuture.delayedExecutor(
                                Math.max(0, deadline - System.currentTimeMillis()),
                                TimeUnit.MILLISECONDS))
                .get(WAIT.toMillis(), TimeUnit.MILLISECONDS);
    }

    private static ZLinkMeshNodeDescriptor descriptor() {
        return descriptor(TYPE, ZLinkFrameworkRuntimeState.SERVING);
    }

    private static ZLinkMeshNodeDescriptor descriptor(
            String type, ZLinkFrameworkRuntimeState state) {
        return descriptor(type, state, 1, "owner");
    }

    private static ZLinkMeshNodeDescriptor descriptor(
            String type, ZLinkFrameworkRuntimeState state, int weight, String ownerId) {
        return new ZLinkMeshNodeDescriptor(
                MESH,
                RoutingId.from("owner".equals(ownerId) ? "deadline-target" : "zero-target"),
                1,
                1,
                "tcp://127.0.0.1:1",
                Map.of(),
                1,
                List.of(
                        new ZLinkObjectCapability(
                                ZLinkPlacementObjectKind.INSTANCE_SPOT,
                                type,
                                ZLinkObjectMaintenancePolicyKind.SNAPSHOT,
                                true,
                                0)),
                ZLinkMeshNodeObjectRole.SERVER,
                Optional.empty(),
                weight,
                new ZLinkPlacementCapacity(
                        new ZLinkCapacityUsage(0, 0, 0),
                        new ZLinkCapacityUsage(0, 0, 8),
                        List.of(
                                new ZLinkSpotTypeCapacity(
                                        ZLinkPlacementObjectKind.INSTANCE_SPOT,
                                        type,
                                        new ZLinkCapacityUsage(0, 0, 8)))),
                new ZLinkActivationConcurrency(0, 8),
                Optional.empty(),
                state,
                "security",
                ownerId,
                1,
                Instant.now());
    }

    public record InitialMessage() {}
}
