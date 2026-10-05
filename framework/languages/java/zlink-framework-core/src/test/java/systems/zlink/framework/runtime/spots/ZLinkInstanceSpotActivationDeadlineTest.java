package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
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
    void pendingReservationEndsAtSourceMeshDefaultRequestDeadline() throws Exception {
        run(false, true);
    }

    @Test
    void delayedResolveDoesNotRestartActivationDeadline() throws Exception {
        run(true, true);
    }

    @Test
    void unspecifiedMeshDefaultUsesFrameworkRequestTimeout() throws Exception {
        run(false, false);
    }

    @Test
    void publicRouteResolveConsumesOriginalActivationDeadline() throws Exception {
        run(true, true, true);
    }

    private static void run(boolean delayResolve, boolean meshOverride) throws Exception {
        run(delayResolve, meshOverride, false);
    }

    private static void run(boolean delayResolve, boolean meshOverride, boolean delayRoute)
            throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(meshOverride ? WAIT : ACTIVATION_TIMEOUT);
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        var mesh = options.addRouteMesh(MESH).listen("inproc://deadline-" + UUID.randomUUID());
        mesh.objects().server();
        if (meshOverride) mesh.setDefaultRequestTimeout(ACTIVATION_TIMEOUT);
        try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            var host = (ZLinkSpotRuntime) runtime.spotManager();
            var descriptor = descriptor();
            var resolve = new CompletableFuture<ZLinkAuthorityReadResult>();
            var cancellation = new CompletableFuture<ZLinkStoreCancellation>();
            var reservation = new CompletableFuture<ZLinkObjectReserveResult>();
            var repository =
                    (ZLinkLocationRepository)
                            Proxy.newProxyInstance(
                                    ZLinkLocationRepository.class.getClassLoader(),
                                    new Class<?>[] {ZLinkLocationRepository.class},
                                    (proxy, method, arguments) ->
                                            switch (method.getName()) {
                                                case "read" -> resolve;
                                                case "listMeshNodes" ->
                                                        CompletableFuture.completedFuture(
                                                                new ZLinkLocationPage<>(
                                                                        List.of(descriptor), null));
                                                case "reserve" -> {
                                                    var token =
                                                            (ZLinkStoreCancellation) arguments[1];
                                                    cancellation.complete(token);
                                                    observeDeadline(token, reservation);
                                                    yield reservation;
                                                }
                                                default ->
                                                        throw new UnsupportedOperationException(
                                                                method.getName());
                                            });
            var store = ZLinkSpotRuntime.class.getDeclaredField("userSpotLocationStore");
            store.setAccessible(true);
            var original = store.get(host);
            store.set(host, repository);
            var routeResolve = new CompletableFuture<Optional<SpotTransportAddress>>();
            var routeResolver = ZLinkChannelRuntime.class.getDeclaredField("spotAddressResolver");
            routeResolver.setAccessible(true);
            var originalResolver = routeResolver.get(runtime.route());
            if (delayRoute) {
                routeResolver.set(
                        runtime.route(), (SpotTransportAddressResolver) spotId -> routeResolve);
            }
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
                if (delayResolve) {
                    // Hold resolution beyond the original deadline. A recomputed deadline
                    // would incorrectly permit a new reservation wait.
                    CompletableFuture.runAsync(
                                    () -> {},
                                    CompletableFuture.delayedExecutor(
                                            ACTIVATION_TIMEOUT.toMillis() + 100,
                                            TimeUnit.MILLISECONDS))
                            .get(WAIT.toMillis(), TimeUnit.MILLISECONDS);
                }
                routeResolve.complete(Optional.empty());
                resolve.complete(new ZLinkAuthorityMissing(Instant.now()));
                var token = cancellation.get(WAIT.toMillis(), TimeUnit.MILLISECONDS);
                if (delayResolve) {
                    assertTrue(
                            token.isCancellationRequested(),
                            "resolve must preserve the submit deadline");
                } else {
                    assertFalse(token.isCancellationRequested());
                    long beforeDeadline = started + ACTIVATION_TIMEOUT.toMillis() - 100;
                    waitUntil(beforeDeadline);
                    assertFalse(
                            token.isCancellationRequested(), "activation must not expire early");
                    assertFalse(send.isDone());
                    waitUntil(submitted + ACTIVATION_TIMEOUT.toMillis() + 100);
                    assertTrue(token.isCancellationRequested(), "pending reservation must expire");
                }
                var failure =
                        assertThrows(
                                ExecutionException.class,
                                () -> send.get(WAIT.toMillis(), TimeUnit.MILLISECONDS));
                assertEquals(
                        ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                        assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
            } finally {
                reservation.completeExceptionally(new IllegalStateException("test cleanup"));
                resolve.complete(new ZLinkAuthorityMissing(Instant.now()));
                routeResolve.complete(Optional.empty());
                routeResolver.set(runtime.route(), originalResolver);
                store.set(host, original);
            }
        }
    }

    private static void observeDeadline(
            ZLinkStoreCancellation token, CompletableFuture<ZLinkObjectReserveResult> reservation) {
        if (reservation.isDone()) return;
        if (token.isCancellationRequested()) {
            reservation.completeExceptionally(
                    new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                            "activation reservation expired"));
            return;
        }
        CompletableFuture.delayedExecutor(5, TimeUnit.MILLISECONDS)
                .execute(() -> observeDeadline(token, reservation));
    }

    private static void waitUntil(long deadline) throws Exception {
        long remaining = Math.max(0, deadline - System.currentTimeMillis());
        CompletableFuture.runAsync(
                        () -> {},
                        CompletableFuture.delayedExecutor(remaining, TimeUnit.MILLISECONDS))
                .get(WAIT.toMillis(), TimeUnit.MILLISECONDS);
    }

    private static ZLinkMeshNodeDescriptor descriptor() {
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
                "owner",
                1,
                Instant.now());
    }

    public record InitialMessage() {}
}
