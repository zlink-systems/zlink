package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.*;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetricProbe;
import systems.zlink.framework.runtime.internal.service.ZLinkInstanceActivationRecoveryCodec;
import systems.zlink.framework.runtime.locations.*;
import systems.zlink.framework.spots.*;

import java.util.*;
import java.util.concurrent.*;

final class ZLinkInstanceTakeoverMetricTest {
    public static final class Instance implements ZLinkInstanceSpot {
        private final ZLinkInstanceSpotContext context;

        public Instance(ZLinkInstanceSpotContext context) {
            this.context = context;
        }

        public ZLinkInstanceSpotContext context() {
            return context;
        }
    }

    @ParameterizedTest
    @ValueSource(
            strings = {
                "lost",
                "release_failed",
                "reserve_failed",
                "claimed",
                "release_store_failed",
                "claim_store_failed"
            })
    @SuppressWarnings("unchecked")
    void takeoverRecordsOneTerminalOnlyAfterTheReclaimAndClaimOperation(String scenario)
            throws Exception {
        String mesh = "takeover-metrics";
        var release = new CompletableFuture<Boolean>();
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        options.addRelocationStore(new systems.zlink.framework.runtime.InMemoryRelocationStore());
        options.addRouteMesh(mesh)
                .listen("inproc://takeover-" + UUID.randomUUID())
                .objects()
                .server()
                .addInstanceSpotFactory(
                        "instance", Instance.class, factory -> factory.disableRelocation());
        try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                    .toCompletableFuture()
                    .get(10, TimeUnit.SECONDS);
            var spots = (ZLinkSpotRuntime) runtime.spotManager();
            var field = ZLinkSpotRuntime.class.getDeclaredField("userSpotLocationStore");
            field.setAccessible(true);
            var original = field.get(spots);
            var nodesField = ZLinkSpotRuntime.class.getDeclaredField("routeMeshNodesByName");
            nodesField.setAccessible(true);
            var node = ((Map<String, ZLinkInternalMeshNode>) nodesField.get(spots)).get(mesh);
            var spi =
                    (systems.zlink.framework.locationprovider.ZLinkLocationStore)
                            java.lang.reflect.Proxy.newProxyInstance(
                                    getClass().getClassLoader(),
                                    new Class<?>[] {
                                        systems.zlink.framework.locationprovider.ZLinkLocationStore
                                                .class
                                    },
                                    (proxy, method, args) ->
                                            CompletableFuture.failedFuture(
                                                    new IllegalStateException("SPI unavailable")));
            var failingStore = new ZLinkProviderLocationRepository(spi);
            var repository =
                    (ZLinkLocationRepository)
                            java.lang.reflect.Proxy.newProxyInstance(
                                    getClass().getClassLoader(),
                                    new Class<?>[] {ZLinkLocationRepository.class},
                                    (proxy, method, args) -> {
                                        if (method.getName().equals("releaseEndedReservation")) {
                                            if (scenario.equals("release_store_failed"))
                                                return release.thenCompose(
                                                        ignored ->
                                                                failingStore
                                                                        .releaseEndedReservation(
                                                                                (ZLinkObjectReservationRequest)
                                                                                        args[0],
                                                                                (String) args[1],
                                                                                (ZLinkStoreCancellation)
                                                                                        args[2]));
                                            return release;
                                        }
                                        if (method.getName().equals("read")) {
                                            if (scenario.equals("claim_store_failed"))
                                                return failingStore.read(
                                                        (String) args[0],
                                                        (ZLinkStoreCancellation) args[1]);
                                            if (!scenario.equals("claimed"))
                                                return CompletableFuture.failedFuture(
                                                        new IllegalStateException(
                                                                "claim backend failed"));
                                        }
                                        try {
                                            return method.invoke(original, args);
                                        } catch (
                                                java.lang.reflect.InvocationTargetException
                                                        failure) {
                                            throw failure.getCause();
                                        }
                                    });
            var snapshot =
                    new ZLinkAuthoritySnapshot(
                            "version",
                            new ZLinkServiceAuthorityPayloadCodec()
                                    .encodeInstance(
                                            ZLinkServiceAuthorityPayloadCodec.State.READY,
                                            "instance",
                                            "spot",
                                            "old-owner",
                                            1,
                                            mesh,
                                            node.routingId(),
                                            node.lifecycleGeneration()),
                            1,
                            1,
                            "old-owner",
                            1,
                            new ZLinkPlacementAllocation(
                                    ZLinkPlacementAllocationState.ACTIVE,
                                    ZLinkPlacementObjectKind.INSTANCE_SPOT,
                                    "instance",
                                    new ZLinkMeshNodeDescriptorKey(mesh, node.routingId()),
                                    node.lifecycleGeneration(),
                                    ZLinkPlacementCapacityBundle.spot(
                                            ZLinkPlacementObjectKind.INSTANCE_SPOT, "instance", 1)),
                            Optional.empty(),
                            java.time.Instant.now());
            byte[] application =
                    new systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec()
                            .encodeApplicationPayload(
                                    new systems.zlink.framework.runtime.internal.service
                                            .ZLinkServiceM6AWireCodec.ApplicationPayload(
                                            "Initial", "application/json", new byte[] {1}));
            var envelope =
                    new ZLinkInstanceActivationRecoveryCodec.RecoveryEnvelope(
                            "spot",
                            "instance",
                            mesh,
                            node.routingId(),
                            node.lifecycleGeneration(),
                            "version",
                            RoutingId.from("source"),
                            1,
                            Optional.empty(),
                            false,
                            1,
                            2,
                            null,
                            System.currentTimeMillis() + 30000,
                            new byte[0],
                            application);
            var method =
                    ZLinkSpotRuntime.class.getDeclaredMethod(
                            "reserveInstanceSpotTarget",
                            ZLinkInstanceActivationRecoveryCodec.RecoveryEnvelope.class,
                            ZLinkAuthorityReadResult.class);
            method.setAccessible(true);
            field.set(spots, repository);
            try (var metrics = ZLinkRuntimeMetricProbe.install()) {
                var terminal = (CompletionStage<?>) method.invoke(spots, envelope, snapshot);
                assertEquals(0, metrics.count("zlink.instance_spot.takeovers"));
                if (scenario.equals("release_failed"))
                    release.completeExceptionally(
                            new IllegalStateException("reclaim backend failed"));
                else release.complete(!scenario.equals("lost"));
                if (scenario.equals("claimed"))
                    assertNotNull(terminal.toCompletableFuture().join());
                else
                    assertThrows(
                            CompletionException.class, () -> terminal.toCompletableFuture().join());
                assertEquals(1, metrics.count("zlink.instance_spot.takeovers"));
                assertEquals(
                        scenario.equals("lost")
                                ? "lost"
                                : scenario.equals("claimed") ? "claimed" : "failed",
                        metrics.only("zlink.instance_spot.takeovers").tags().get("outcome"));
                assertFalse(release.complete(false));
                assertEquals(1, metrics.count("zlink.instance_spot.takeovers"));
            } finally {
                field.set(spots, original);
            }
        }
    }
}
