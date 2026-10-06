package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.errors.*;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.channels.ZLinkChannelRuntime;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.*;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddress;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddressResolver;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.time.Instant;
import java.util.*;
import java.util.concurrent.*;

final class ZLinkInstanceSpotActivationDeadlineTest {
    private static final String MESH = "activation-deadline";
    private static final String TYPE = "deadline-instance";
    private static final Duration ACTIVATION_TIMEOUT = Duration.ofMillis(600);
    private static final Duration WAIT = Duration.ofSeconds(5);

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
                "resolveInstanceType",
                null,
                ZLinkFrameworkErrorKind.NOT_FOUND);
    }

    @Test
    void explicitTypeWithoutServingCapabilityIsNotFound() throws Exception {
        assertTypeSelection(
                List.of(descriptor(TYPE + "-other", ZLinkFrameworkRuntimeState.SERVING)),
                "selectInstanceSpotTarget",
                TYPE,
                ZLinkFrameworkErrorKind.NOT_FOUND);
    }

    private static void assertTypeSelection(
            List<ZLinkMeshNodeDescriptor> descriptors,
            String methodName,
            String requestedType,
            ZLinkFrameworkErrorKind expected)
            throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        options.addRouteMesh(MESH).listen("inproc://type-" + UUID.randomUUID()).objects().server();
        try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            var host = (ZLinkSpotRuntime) runtime.spotManager();
            var repository =
                    (ZLinkLocationRepository)
                            Proxy.newProxyInstance(
                                    ZLinkLocationRepository.class.getClassLoader(),
                                    new Class<?>[] {ZLinkLocationRepository.class},
                                    (proxy, method, arguments) -> {
                                        if (method.getName().equals("listMeshNodes"))
                                            return CompletableFuture.completedFuture(
                                                    new ZLinkLocationPage<>(descriptors, null));
                                        throw new UnsupportedOperationException(method.getName());
                                    });
            var method =
                    ZLinkSpotRuntime.class.getDeclaredMethod(
                            methodName, ZLinkLocationRepository.class, String.class, String.class);
            method.setAccessible(true);
            @SuppressWarnings("unchecked")
            var result = (CompletionStage<?>) method.invoke(host, repository, MESH, requestedType);
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
        try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            var host = (ZLinkSpotRuntime) runtime.spotManager();
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
                                                                        List.of(descriptor()),
                                                                        null));
                                                case "reserve" ->
                                                        throw new AssertionError(
                                                                "source must not create a reservation");
                                                default ->
                                                        throw new UnsupportedOperationException(
                                                                method.getName());
                                            });
            var storeField = ZLinkSpotRuntime.class.getDeclaredField("userSpotLocationStore");
            storeField.setAccessible(true);
            var originalStore = storeField.get(host);
            storeField.set(host, repository);
            var nodesField = ZLinkSpotRuntime.class.getDeclaredField("routeMeshNodesByName");
            nodesField.setAccessible(true);
            var nodes =
                    (Map<
                                    String,
                                    systems.zlink.framework.runtime.internal.backend
                                            .ZLinkInternalMeshNode>)
                            nodesField.get(host);
            var originalNode = nodes.get(MESH);
            var wireRoute = new CompletableFuture<Object>();
            var admission = new CompletableFuture<Void>();
            var source =
                    (systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode)
                            Proxy.newProxyInstance(
                                    originalNode.getClass().getClassLoader(),
                                    new Class<?>[] {
                                        systems.zlink.framework.runtime.internal.backend
                                                .ZLinkInternalMeshNode.class
                                    },
                                    (proxy, method, arguments) -> {
                                        if (method.getName().equals("submitInstanceSpotSend")) {
                                            wireRoute.complete(arguments[0]);
                                            return admission;
                                        }
                                        try {
                                            return method.invoke(originalNode, arguments);
                                        } catch (
                                                java.lang.reflect.InvocationTargetException
                                                        failure) {
                                            throw failure.getCause();
                                        }
                                    });
            var substituted = new HashMap<>(nodes);
            substituted.put(MESH, source);
            nodesField.set(host, Map.copyOf(substituted));
            var routeResolve = new CompletableFuture<Optional<SpotTransportAddress>>();
            var resolverField = ZLinkChannelRuntime.class.getDeclaredField("spotAddressResolver");
            resolverField.setAccessible(true);
            var originalResolver = resolverField.get(runtime.route());
            resolverField.set(
                    runtime.route(), (SpotTransportAddressResolver) spotId -> routeResolve);
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
                nodesField.set(host, nodes);
                resolverField.set(runtime.route(), originalResolver);
                storeField.set(host, originalStore);
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
        return new ZLinkMeshNodeDescriptor(
                MESH,
                RoutingId.from("deadline-target"),
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
                1,
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
                "owner",
                1,
                Instant.now());
    }

    public record InitialMessage() {}
}
