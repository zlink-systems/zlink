package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.RepeatedTest;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkLocationPage;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkObjectCapability;
import systems.zlink.framework.locations.ZLinkObjectMaintenancePolicyKind;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeState;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeStatus;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerEntry;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerSource;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerState;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.runtime.locations.ZLinkCandidateLeaseFixture;
import systems.zlink.framework.runtime.locations.ZLinkCandidateLeaseFixture.Lease;
import systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers;
import systems.zlink.framework.runtime.mesh.ZLinkActivationAdmission;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Function;

final class ZLinkActorCreationCoordinatorTargetSelectionTest {
    private static final Instant NOW = Instant.parse("2026-01-01T00:00:00Z");

    @RepeatedTest(40)
    void emptyStoreRefreshIsImmediatelyUnavailable() throws Exception {
        var reads = new AtomicInteger();
        var fixture =
                fixture(
                        request -> {
                            reads.incrementAndGet();
                            return CompletableFuture.completedFuture(
                                    new ZLinkLocationPage<>(List.of(), null));
                        });
        fixture.discovery()
                .listPeers(
                        systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectType
                                .ROUTE_MESH,
                        "mesh",
                        null)
                .toCompletableFuture()
                .join();
        var selected = creationTarget(fixture, Duration.ofMillis(100)).toCompletableFuture();
        assertTrue(selected.isDone(), "empty discovery must not wait for the deadline");
        var failure = assertThrows(CompletionException.class, selected::join);
        assertEquals(
                ZLinkFrameworkErrorKind.UNAVAILABLE,
                ((ZLinkFrameworkException) failure.getCause()).kind());
        assertEquals(2, reads.get(), "empty discovery permits one Store list before Unavailable");
    }

    @RepeatedTest(40)
    void preparingWaitsForSharedDiscoveryPublication() throws Exception {
        var rows =
                new java.util.concurrent.atomic.AtomicReference<List<ZLinkMeshNodeDescriptor>>(
                        List.of(
                                withState(
                                        descriptor(RoutingId.from("local-node"), 100),
                                        ZLinkFrameworkRuntimeState.PREPARING)));
        var reads = new AtomicInteger();
        var fixture =
                fixture(
                        request -> {
                            reads.incrementAndGet();
                            return CompletableFuture.completedFuture(
                                    new ZLinkLocationPage<>(rows.get(), null));
                        });
        var resolver = fixture.discovery();
        resolver.listPeers(
                        systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectType
                                .ROUTE_MESH,
                        "mesh",
                        null)
                .toCompletableFuture()
                .join();
        var selected = creationTarget(fixture, Duration.ofSeconds(1)).toCompletableFuture();
        assertFalse(selected.isDone());
        assertEquals(1, reads.get(), "waiting requests must not re-list the Store");
        rows.set(List.of(descriptor(RoutingId.from("local-node"), 100)));
        resolver.listPeers(
                        systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectType
                                .ROUTE_MESH,
                        "mesh",
                        null)
                .toCompletableFuture()
                .join();
        assertEquals(RoutingId.from("local-node"), selected.join().rid());
        assertEquals(2, reads.get());
    }

    private static java.util.concurrent.CompletionStage<ZLinkMeshNodeDescriptor> creationTarget(
            Fixture fixture, Duration timeout) {
        return fixture.coordinator()
                .selectTarget(
                        "player",
                        System.currentTimeMillis() + timeout.toMillis(),
                        java.util.Set.of(),
                        true);
    }

    private record Fixture(
            ZLinkActorCreationCoordinator coordinator, ZLinkStoreLocationResolvers discovery) {}

    private static ZLinkMeshNodeDescriptor withState(
            ZLinkMeshNodeDescriptor d, ZLinkFrameworkRuntimeState state) {
        return new ZLinkMeshNodeDescriptor(
                d.meshName(),
                d.rid(),
                d.lifecycleGeneration(),
                d.descriptorRevision(),
                d.endpoint(),
                d.channelWeights(),
                d.applicationVersion(),
                d.objectCapabilities(),
                d.objectRole(),
                d.entrySpotId(),
                d.placementWeight(),
                d.capacity(),
                d.activationConcurrency(),
                d.maintenanceWave(),
                state,
                d.securityIdentity(),
                d.ownerId(),
                d.leaseGeneration(),
                d.updatedAt());
    }

    @Test
    void targetSelectionConsumesEmptyAndTrailingDescriptorPages() throws Exception {
        AtomicInteger pages = new AtomicInteger();
        var fixture =
                fixture(
                        request -> {
                            int page = pages.getAndIncrement();
                            assertEquals(
                                    page == 0 ? null : "page-" + page, request.continuationToken());
                            return CompletableFuture.completedFuture(
                                    new ZLinkLocationPage<>(
                                            page == 1
                                                    ? List.of(
                                                            descriptor(
                                                                    RoutingId.from("local-node"),
                                                                    1))
                                                    : List.of(),
                                            page < 2 ? "page-" + (page + 1) : null));
                        });

        var selected =
                fixture.coordinator()
                        .selectEntrySpotTarget("player", Duration.ofSeconds(1))
                        .toCompletableFuture()
                        .join();

        assertEquals(RoutingId.from("local-node"), selected.nodeRid());
        assertEquals(3, pages.get());
    }

    @Test
    void discoveryPreservesFailureAfterAnEligibleDescriptorPage() throws Exception {
        var failure = new IllegalStateException("descriptor continuation failed");
        var fixture =
                fixture(
                        request ->
                                request.continuationToken() == null
                                        ? CompletableFuture.completedFuture(
                                                new ZLinkLocationPage<>(
                                                        List.of(
                                                                descriptor(
                                                                        RoutingId.from(
                                                                                "local-node"),
                                                                        1)),
                                                        "next"))
                                        : CompletableFuture.failedFuture(failure));

        var thrown =
                assertThrows(
                        CompletionException.class,
                        () ->
                                fixture.discovery()
                                        .listPeers(
                                                systems.zlink.framework.runtime.internal.locations
                                                        .ZLinkAutoConnectType.ROUTE_MESH,
                                                "mesh",
                                                null)
                                        .toCompletableFuture()
                                        .join());

        assertEquals(failure, thrown.getCause());
    }

    @ParameterizedTest
    @EnumSource(
            value = Lease.class,
            names = {"MISSING", "EXPIRED", "REPLACED", "WRONG_OWNER", "LIVE", "CORRUPT", "FAILED"})
    void newActorCandidatesRequireLiveOwner(Lease lease) throws Exception {
        var sideEffects = new AtomicInteger();
        var fixture =
                fixture(
                        request ->
                                CompletableFuture.completedFuture(
                                        new ZLinkLocationPage<>(
                                                List.of(
                                                        descriptor(
                                                                RoutingId.from("local-node"), 1)),
                                                null)),
                        lease,
                        sideEffects);
        var refreshed =
                fixture.discovery()
                        .listPeers(
                                systems.zlink.framework.runtime.internal.locations
                                        .ZLinkAutoConnectType.ROUTE_MESH,
                                "mesh",
                                null)
                        .toCompletableFuture();
        if (lease == Lease.FAILED || lease == Lease.CORRUPT) {
            var failure = assertThrows(CompletionException.class, refreshed::join);
            if (lease == Lease.FAILED)
                org.junit.jupiter.api.Assertions.assertSame(
                        ZLinkCandidateLeaseFixture.READ_FAILURE, failure.getCause());
            else
                assertEquals(
                        ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
                        ((ZLinkFrameworkException) failure.getCause()).kind());
            assertEquals(0, sideEffects.get());
            return;
        }
        refreshed.join();
        var selection = creationTarget(fixture, Duration.ofSeconds(1)).toCompletableFuture();
        if (lease == Lease.LIVE) {
            assertEquals(RoutingId.from("local-node"), selection.join().rid());
        } else {
            var failure = assertThrows(CompletionException.class, selection::join);
            if (lease == Lease.FAILED)
                org.junit.jupiter.api.Assertions.assertSame(
                        ZLinkCandidateLeaseFixture.READ_FAILURE, failure.getCause());
            else
                assertEquals(
                        lease == Lease.CORRUPT
                                ? ZLinkFrameworkErrorKind.INTERNAL_FAILURE
                                : ZLinkFrameworkErrorKind.UNAVAILABLE,
                        ((ZLinkFrameworkException) failure.getCause()).kind());
            var create =
                    fixture.coordinator()
                            .submit(
                                    "new-actor",
                                    "player",
                                    systems.zlink.framework.messaging.ZLinkMessage.of("create"),
                                    false,
                                    Duration.ofSeconds(1))
                            .toCompletableFuture();
            var createFailure = assertThrows(CompletionException.class, create::join);
            if (lease == Lease.FAILED)
                org.junit.jupiter.api.Assertions.assertSame(
                        ZLinkCandidateLeaseFixture.READ_FAILURE, createFailure.getCause());
            else
                assertEquals(
                        lease == Lease.CORRUPT
                                ? ZLinkFrameworkErrorKind.INTERNAL_FAILURE
                                : ZLinkFrameworkErrorKind.UNAVAILABLE,
                        ((ZLinkFrameworkException) createFailure.getCause()).kind());
            assertEquals(
                    0,
                    sideEffects.get(),
                    "excluded candidate must not reserve or activate an Actor");
        }
    }

    private static Fixture fixture(
            Function<
                            ZLinkPageRequest,
                            CompletableFuture<ZLinkLocationPage<ZLinkMeshNodeDescriptor>>>
                    pages) {
        return fixture(pages, Lease.LIVE);
    }

    private static Fixture fixture(
            Function<
                            ZLinkPageRequest,
                            CompletableFuture<ZLinkLocationPage<ZLinkMeshNodeDescriptor>>>
                    pages,
            Lease lease) {
        return fixture(pages, lease, new AtomicInteger());
    }

    private static Fixture fixture(
            Function<
                            ZLinkPageRequest,
                            CompletableFuture<ZLinkLocationPage<ZLinkMeshNodeDescriptor>>>
                    pages,
            Lease lease,
            AtomicInteger sideEffects) {
        var local = RoutingId.from("local-node");
        var node =
                (ZLinkInternalMeshNode)
                        Proxy.newProxyInstance(
                                ZLinkInternalMeshNode.class.getClassLoader(),
                                new Class<?>[] {ZLinkInternalMeshNode.class},
                                (proxy, method, arguments) ->
                                        switch (method.getName()) {
                                            case "status" -> status(local, MeshNodeState.READY, 1);
                                            case "peers" -> List.of();
                                            default -> {
                                                sideEffects.incrementAndGet();
                                                throw new AssertionError(
                                                        "Unexpected mesh call: " + method);
                                            }
                                        });
        var repository =
                (ZLinkLocationRepository)
                        Proxy.newProxyInstance(
                                ZLinkLocationRepository.class.getClassLoader(),
                                new Class<?>[] {ZLinkLocationRepository.class},
                                (proxy, method, arguments) -> {
                                    if (method.getName().equals("readCreationTerminal"))
                                        return CompletableFuture.completedFuture(
                                                new systems.zlink.framework.runtime.internal
                                                        .locations.ZLinkCreationTerminalMissing());
                                    if (method.getName().equals("readOwnerLease"))
                                        return ZLinkCandidateLeaseFixture.read(
                                                lease, (String) arguments[0], 1);
                                    if (!method.getName().equals("listMeshNodes"))
                                        sideEffects.incrementAndGet();
                                    assertEquals("listMeshNodes", method.getName());
                                    assertEquals("mesh", arguments[0]);
                                    return pages.apply((ZLinkPageRequest) arguments[1]);
                                });
        var spot =
                (ZLinkInternalSpotNode)
                        Proxy.newProxyInstance(
                                ZLinkInternalSpotNode.class.getClassLoader(),
                                new Class<?>[] {ZLinkInternalSpotNode.class},
                                (proxy, method, arguments) -> {
                                    if (!method.getName().equals("routingId"))
                                        sideEffects.incrementAndGet();
                                    assertEquals("routingId", method.getName());
                                    return local;
                                });
        var serializer = new ZLinkJsonMessageSerializer();
        var actors = new ZLinkActorRuntime(spot, Map.of(), Duration.ofSeconds(1), serializer);
        var discovery =
                new ZLinkStoreLocationResolvers(
                        systems.zlink.framework.runtime.locations.ZLinkRegisteredLocationStores
                                .fromUnified(repository),
                        new systems.zlink.framework.locations.ZLinkLocationOptions());
        var coordinator =
                new ZLinkActorCreationCoordinator(
                        "mesh",
                        node,
                        repository,
                        discovery,
                        actors,
                        serializer,
                        new ZLinkActivationAdmission(1),
                        ignored -> null,
                        stableType -> true);
        return new Fixture(coordinator, discovery);
    }

    @Test
    void zeroWeightLocalNodeDoesNotOverrideRemotePlacement() {
        var local = RoutingId.from("local-node");
        var remote = RoutingId.from("remote-node");
        var candidates =
                systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers
                        .classifyPlacement(
                                List.of(descriptor(local, 0), descriptor(remote, 100)),
                                ZLinkPlacementObjectKind.ACTOR,
                                "player",
                                true,
                                source(
                                        status(local, MeshNodeState.READY, 1),
                                        List.of(peer(remote, MeshPeerState.ADMITTED, 1))),
                                java.util.Set.of())
                        .candidates();
        assertEquals(List.of(descriptor(remote, 100)), candidates);
    }

    @Test
    void localTargetRequiresReadyStatusAndExactGeneration() {
        RoutingId local = RoutingId.from("local-node");
        ZLinkMeshNodeDescriptor candidate = descriptor(local, 1, 7);
        MeshNodeStatus ready = status(local, MeshNodeState.READY, 7);

        assertTrue(isExactReadyTarget(candidate, ready, List.of()));
        assertFalse(isExactReadyTarget(descriptor(local, 1, 8), ready, List.of()));
        assertFalse(
                isExactReadyTarget(candidate, status(local, MeshNodeState.STARTED, 7), List.of()));
    }

    @Test
    void remoteTargetRequiresAdmittedPeerWithExactGeneration() {
        RoutingId local = RoutingId.from("local-node");
        RoutingId remote = RoutingId.from("remote-node");
        ZLinkMeshNodeDescriptor candidate = descriptor(remote, 1, 7);
        MeshNodeStatus localStatus = status(local, MeshNodeState.READY, 3);

        assertTrue(
                isExactReadyTarget(
                        candidate, localStatus, List.of(peer(remote, MeshPeerState.ADMITTED, 7))));
        assertFalse(
                isExactReadyTarget(
                        candidate,
                        localStatus,
                        List.of(peer(remote, MeshPeerState.CONNECTING, 7))));
        assertFalse(
                isExactReadyTarget(
                        candidate, localStatus, List.of(peer(remote, MeshPeerState.ADMITTED, 6))));
    }

    private static boolean isExactReadyTarget(
            ZLinkMeshNodeDescriptor descriptor, MeshNodeStatus status, List<MeshPeerEntry> peers) {
        return !systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers
                .classifyPlacement(
                        List.of(descriptor),
                        ZLinkPlacementObjectKind.ACTOR,
                        "player",
                        false,
                        source(status, peers),
                        java.util.Set.of())
                .candidates()
                .isEmpty();
    }

    private static ZLinkInternalMeshNode source(MeshNodeStatus status, List<MeshPeerEntry> peers) {
        return (ZLinkInternalMeshNode)
                Proxy.newProxyInstance(
                        ZLinkInternalMeshNode.class.getClassLoader(),
                        new Class<?>[] {ZLinkInternalMeshNode.class},
                        (proxy, method, args) ->
                                switch (method.getName()) {
                                    case "status" -> status;
                                    case "peers" -> peers;
                                    default -> throw new AssertionError(method.getName());
                                });
    }

    private static MeshNodeStatus status(
            RoutingId routingId, MeshNodeState state, long lifecycleGeneration) {
        return new MeshNodeStatus(
                state,
                routingId,
                "mesh",
                "tcp://127.0.0.1:1",
                lifecycleGeneration,
                1,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0);
    }

    private static MeshPeerEntry peer(
            RoutingId routingId, MeshPeerState state, long lifecycleGeneration) {
        return new MeshPeerEntry(
                routingId,
                "tcp://127.0.0.1:2",
                1,
                MeshPeerSource.MANUAL,
                state,
                lifecycleGeneration,
                1,
                0,
                0,
                0);
    }

    private static ZLinkMeshNodeDescriptor descriptor(RoutingId rid, int placementWeight) {
        return descriptor(rid, placementWeight, 1);
    }

    private static ZLinkMeshNodeDescriptor descriptor(
            RoutingId rid, int placementWeight, long lifecycleGeneration) {
        return new ZLinkMeshNodeDescriptor(
                "mesh",
                rid,
                lifecycleGeneration,
                1,
                "tcp://127.0.0.1:1",
                Map.of(),
                1,
                List.of(
                        new ZLinkObjectCapability(
                                ZLinkPlacementObjectKind.ACTOR,
                                "player",
                                ZLinkObjectMaintenancePolicyKind.SNAPSHOT,
                                true,
                                0,
                                true)),
                ZLinkMeshNodeObjectRole.SERVER,
                Optional.of("entry-00000000-0000-4000-8000-000000000001"),
                placementWeight,
                new ZLinkPlacementCapacity(
                        new ZLinkCapacityUsage(0, 0, 8),
                        new ZLinkCapacityUsage(0, 0, 0),
                        List.of()),
                new ZLinkActivationConcurrency(0, 8),
                Optional.empty(),
                ZLinkFrameworkRuntimeState.SERVING,
                "security",
                "owner",
                1,
                NOW);
    }
}
