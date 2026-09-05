package systems.zlink.framework.runtime.host;
import java.time.Duration;
import java.util.UUID;
import java.util.concurrent.Flow;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerEntry;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerSource;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerState;
import systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;

import org.junit.jupiter.api.Test;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectPeer;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectType;
import systems.zlink.framework.runtime.locations.ZLinkLocationAutoConnectHost;

final class ZLinkFrameworkRuntimeDrainRouteTest {
    @Test
    void retireBlocksStorelessManualPublisherBeforeStateChanges()
        throws Exception {
        DefaultZLinkFrameworkOptions options =
            new DefaultZLinkFrameworkOptions();
        options.addFanoutChannel("manual-events")
            .enablePublisher(
                "inproc://manual-events-" + UUID.randomUUID());
        ZLinkFrameworkRuntime runtime = ZLinkFrameworkRuntimeTestAccess.start(
            options,
            new ZLinkJavaBackendAdapterFactory());
        long readyDeadline = System.nanoTime()
            + Duration.ofSeconds(1).toNanos();
        while (!runtime.isReady() && System.nanoTime() < readyDeadline) {
            Thread.sleep(1);
        }

        ZLinkFrameworkRelocationResult result = runtime.relocate(
                new ZLinkFrameworkRelocationOptions(
                    ZLinkFrameworkRelocationMode.PLANNED_MAINTENANCE,
                    null,
                    Duration.ofSeconds(1)))
            .toCompletableFuture()
            .get(2, TimeUnit.SECONDS);

        assertEquals(ZLinkFrameworkRelocationOutcome.BLOCKED, result.outcome());
        assertEquals(
            ZLinkFrameworkRelocationReason.MANUAL_TOPOLOGY_UNSUPPORTED,
            result.reason());
        assertEquals(
            ZLinkFrameworkRuntimeState.SERVING,
            runtime.status().state());
        runtime.shutdown(Duration.ofSeconds(1))
            .toCompletableFuture()
            .get(2, TimeUnit.SECONDS);
    }

    @Test
    void shutdownPublishesTheHostWideTerminalContract() throws Exception {
        DefaultZLinkFrameworkOptions options =
            new DefaultZLinkFrameworkOptions();
        ZLinkFrameworkRuntime runtime = ZLinkFrameworkRuntimeTestAccess.start(
            options,
            new ZLinkJavaBackendAdapterFactory());
        long deadline = System.nanoTime()
            + Duration.ofSeconds(1).toNanos();
        while (!runtime.isReady() && System.nanoTime() < deadline) {
            Thread.sleep(1);
        }
        CompletableFuture<ZLinkFrameworkTerminationResult> observed =
            new CompletableFuture<>();
        runtime.observe().subscribe(new Flow.Subscriber<>() {
            @Override
            public void onSubscribe(
                Flow.Subscription subscription) {
                subscription.request(Long.MAX_VALUE);
            }

            @Override
            public void onNext(
                systems.zlink.framework.monitoring
                    .ZLinkObservedStatus<systems.zlink.framework.monitoring
                        .ZLinkFrameworkRuntimeStatus> observedStatus) {
                observedStatus.status().terminationResult().ifPresent(observed::complete);
            }

            @Override
            public void onError(Throwable throwable) {
                observed.completeExceptionally(throwable);
            }

            @Override
            public void onComplete() {
            }
        });

        ZLinkFrameworkTerminationResult result = runtime.shutdown(
                Duration.ofSeconds(1))
            .toCompletableFuture()
            .get(2, TimeUnit.SECONDS);

        assertEquals(ZLinkFrameworkTerminationOutcome.STOPPED, result.outcome());
        assertEquals(ZLinkFrameworkTerminationReason.NONE, result.reason());
        assertEquals(
            ZLinkFrameworkRuntimeState.STOPPED,
            runtime.status().state());
        assertEquals(
            result,
            runtime.status().terminationResult().orElseThrow());
        assertEquals(result, observed.get(1, TimeUnit.SECONDS));
    }

    @Test
    void shutdownSealsMeshAdmissionAndPublishesDrainingBeforeAcceptedWorkCompletes()
        throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addRouteMesh("shutdown-mesh")
            .listen("inproc://shutdown-mesh-" + UUID.randomUUID());
        try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(
                 options, new ZLinkJavaBackendAdapterFactory())) {
            long deadline = System.nanoTime() + Duration.ofSeconds(5).toNanos();
            while (!runtime.isReady() && System.nanoTime() < deadline) {
                Thread.sleep(5);
            }
            assertTrue(runtime.isReady());
            var drainsField = ZLinkFrameworkRuntime.class.getDeclaredField("meshDrains");
            drainsField.setAccessible(true);
            var drains = (systems.zlink.framework.runtime.internal.drain
                .ZLinkMeshDrainCoordinator) drainsField.get(runtime);
            var nodesField = ZLinkFrameworkRuntime.class.getDeclaredField("meshNodes");
            nodesField.setAccessible(true);
            var node = ((systems.zlink.framework.runtime.mesh.ZLinkMeshNodesRuntime)
                nodesField.get(runtime)).nodesByName().get("shutdown-mesh");
            var gateField = node.getClass().getDeclaredField("peerAdmissionSealed");
            gateField.setAccessible(true);
            var seal = (java.util.function.BooleanSupplier) gateField.get(node);
            assertFalse(seal.getAsBoolean());
            try (var claim = drains.tryClaim("shutdown-mesh")) {
                assertTrue(claim != null);
                var termination = runtime.shutdown(Duration.ofSeconds(5)).toCompletableFuture();
                assertTrue(seal.getAsBoolean());
                assertEquals(ZLinkFrameworkRuntimeState.DRAINING, runtime.status().state());
                assertEquals(systems.zlink.framework.runtime.internal.binding.spot
                    .MeshNodeState.DRAINING, node.status().state());
                assertFalse(termination.isDone());
                claim.close();
                var result = termination.get(5, TimeUnit.SECONDS);
                assertEquals(ZLinkFrameworkTerminationOutcome.STOPPED, result.outcome());
                assertEquals(ZLinkFrameworkTerminationReason.NONE, result.reason());
            }
        }
    }

    @Test
    void drainWaiterTimeoutDoesNotCompleteSharedDrainState() {
        CompletableFuture<String> shared = new CompletableFuture<>();

        ZLinkFrameworkRuntime.independentWaiter(shared)
            .toCompletableFuture()
            .orTimeout(1, TimeUnit.MILLISECONDS)
            .exceptionally(ignored -> null)
            .join();

        assertFalse(shared.isDone());
        shared.complete("drained");
        assertEquals("drained", shared.join());
    }

    @Test
    void drainTransferUsesSpotMeshRouteChannel() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        ZLinkLegacyTopology.addRouteMeshChannel(options, "game-spots");
        ZLinkLegacyTopology.addSpotMesh(options, "game-spots");

        assertEquals(
            "game-spots",
            ZLinkFrameworkRuntime.transferRouteChannelName(
                options.registration(), "game-spots"));
    }

    @Test
    void drainTransferRequiresActorHostCapabilityAndRejectsLocalNode() {
        RoutingId remote = RoutingId.from("play-b");
        ZLinkAutoConnectPeer capable = peer(remote, List.of("actor:player"));
        ZLinkAutoConnectPeer wrongType = peer(RoutingId.from("enemy-a"), List.of("actor:enemy"));
        ZLinkAutoConnectPeer prefixOnly = peer(RoutingId.from("play-a"), List.of("actor:play"));

        assertTrue(ZLinkFrameworkRuntime.isEligibleActorHandoffTarget(capable, "player", Set.of()));
        assertFalse(ZLinkFrameworkRuntime.isEligibleActorHandoffTarget(wrongType, "player", Set.of()));
        assertFalse(ZLinkFrameworkRuntime.isEligibleActorHandoffTarget(prefixOnly, "player", Set.of()));
        assertFalse(ZLinkFrameworkRuntime.isEligibleActorHandoffTarget(
            capable, "player", Set.of(remote)));
    }

    @Test
    void automaticRetireRequiresNonDrainingExactGenerationAdmittedPeer() {
        RoutingId local = RoutingId.from("blue-a");
        RoutingId green = RoutingId.from("green-b");
        ZLinkAutoConnectPeer descriptor = new ZLinkAutoConnectPeer(
            ZLinkAutoConnectType.ROUTE_MESH,
            "game",
            green,
            ZLinkLocationRole.ROUTER,
            "tcp://green:9000",
            100,
            false,
            7,
            Map.of(),
            List.of(),
            "green-owner",
            7,
            Instant.now());
        var admitted = new MeshPeerEntry(
            green,
            "tcp://green:9000",
            1,
            MeshPeerSource.DISCOVERY,
            MeshPeerState.ADMITTED,
            7,
            1,
            0,
            0,
            0);
        var stale = new MeshPeerEntry(
            green,
            "tcp://green:9000",
            1,
            MeshPeerSource.DISCOVERY,
            MeshPeerState.ADMITTED,
            6,
            1,
            0,
            0,
            0);

        assertFalse(ZLinkFrameworkRuntime.hasExactReadyReplacement(
            List.of(), local, List.of(admitted)));
        assertFalse(ZLinkFrameworkRuntime.hasExactReadyReplacement(
            List.of(descriptor), local, List.of(stale)));
        assertTrue(ZLinkFrameworkRuntime.hasExactReadyReplacement(
            List.of(descriptor), local, List.of(admitted)));
    }

    private static ZLinkAutoConnectPeer peer(RoutingId nodeRid, List<String> capabilities) {
        return new ZLinkAutoConnectPeer(
            ZLinkAutoConnectType.SPOT_MESH, "game-spots", nodeRid,
            ZLinkLocationRole.SPOT, "", 100, false, 0, Map.of(), capabilities,
            "owner", 1, Instant.now());
    }
}
