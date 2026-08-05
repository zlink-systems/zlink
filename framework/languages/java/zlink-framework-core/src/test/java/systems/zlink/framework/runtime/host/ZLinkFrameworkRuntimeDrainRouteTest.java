package systems.zlink.framework.runtime.host;

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
                "inproc://manual-events-" + java.util.UUID.randomUUID());
        ZLinkFrameworkRuntime runtime = ZLinkFrameworkRuntimeTestAccess.start(
            options,
            new ZLinkJavaBackendAdapterFactory());
        long readyDeadline = System.nanoTime()
            + java.time.Duration.ofSeconds(1).toNanos();
        while (!runtime.isReady() && System.nanoTime() < readyDeadline) {
            Thread.sleep(1);
        }

        ZLinkFrameworkRelocationResult result = runtime.relocate(
                new ZLinkFrameworkRelocationOptions(
                    ZLinkFrameworkRelocationMode.PLANNED_MAINTENANCE,
                    null,
                    java.time.Duration.ofSeconds(1)))
            .toCompletableFuture()
            .get(2, TimeUnit.SECONDS);

        assertEquals(ZLinkFrameworkRelocationOutcome.BLOCKED, result.outcome());
        assertEquals(
            ZLinkFrameworkRelocationReason.MANUAL_TOPOLOGY_UNSUPPORTED,
            result.reason());
        assertEquals(
            ZLinkFrameworkRuntimeState.SERVING,
            runtime.status().state());
        runtime.shutdown(java.time.Duration.ofSeconds(1))
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
            + java.time.Duration.ofSeconds(1).toNanos();
        while (!runtime.isReady() && System.nanoTime() < deadline) {
            Thread.sleep(1);
        }
        CompletableFuture<ZLinkFrameworkTerminationResult> observed =
            new CompletableFuture<>();
        runtime.observe().subscribe(new java.util.concurrent.Flow.Subscriber<>() {
            @Override
            public void onSubscribe(
                java.util.concurrent.Flow.Subscription subscription) {
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
                java.time.Duration.ofSeconds(1))
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
        systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addRouteMeshChannel(options, "game-spots");
        systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addSpotMesh(options, "game-spots");

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
        var admitted = new systems.zlink.framework.runtime.internal.binding.spot.MeshPeerEntry(
            green,
            "tcp://green:9000",
            1,
            systems.zlink.framework.runtime.internal.binding.spot.MeshPeerSource.DISCOVERY,
            systems.zlink.framework.runtime.internal.binding.spot.MeshPeerState.ADMITTED,
            7,
            1,
            0,
            0,
            0);
        var stale = new systems.zlink.framework.runtime.internal.binding.spot.MeshPeerEntry(
            green,
            "tcp://green:9000",
            1,
            systems.zlink.framework.runtime.internal.binding.spot.MeshPeerSource.DISCOVERY,
            systems.zlink.framework.runtime.internal.binding.spot.MeshPeerState.ADMITTED,
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
