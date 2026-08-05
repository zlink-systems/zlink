package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkObjectCapability;
import systems.zlink.framework.locations.ZLinkObjectMaintenancePolicyKind;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeState;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeStatus;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerEntry;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerSource;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerState;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;

final class ZLinkActorCreationCoordinatorTargetSelectionTest {
    private static final Instant NOW = Instant.parse(
        "2026-01-01T00:00:00Z");

    @Test
    void localServingTargetIsPreferredBeforeWeightedRemoteSelection() {
        RoutingId local = RoutingId.from("local-node");
        ZLinkMeshNodeDescriptor remote = descriptor(
            RoutingId.from("remote-node"), 10_000);
        ZLinkMeshNodeDescriptor localDescriptor = descriptor(local, 1);

        Optional<ZLinkMeshNodeDescriptor> selected =
            ZLinkActorCreationCoordinator.localCandidate(
                List.of(remote, localDescriptor), local);

        assertTrue(selected.isPresent());
        assertEquals(local, selected.orElseThrow().rid());
    }

    @Test
    void noLocalTargetLeavesWeightedSelectionAvailable() {
        RoutingId local = RoutingId.from("local-node");

        Optional<ZLinkMeshNodeDescriptor> selected =
            ZLinkActorCreationCoordinator.localCandidate(
                List.of(descriptor(
                    RoutingId.from("remote-node"), 100)), local);

        assertTrue(selected.isEmpty());
    }

    @Test
    void localTargetRequiresReadyStatusAndExactGeneration() {
        RoutingId local = RoutingId.from("local-node");
        ZLinkMeshNodeDescriptor candidate = descriptor(local, 1, 7);
        MeshNodeStatus ready = status(local, MeshNodeState.READY, 7);

        assertTrue(ZLinkActorCreationCoordinator.isExactReadyTarget(
            candidate, ready, List.of()));
        assertFalse(ZLinkActorCreationCoordinator.isExactReadyTarget(
            descriptor(local, 1, 8), ready, List.of()));
        assertFalse(ZLinkActorCreationCoordinator.isExactReadyTarget(
            candidate,
            status(local, MeshNodeState.STARTED, 7),
            List.of()));
    }

    @Test
    void remoteTargetRequiresAdmittedPeerWithExactGeneration() {
        RoutingId local = RoutingId.from("local-node");
        RoutingId remote = RoutingId.from("remote-node");
        ZLinkMeshNodeDescriptor candidate = descriptor(remote, 1, 7);
        MeshNodeStatus localStatus = status(local, MeshNodeState.READY, 3);

        assertTrue(ZLinkActorCreationCoordinator.isExactReadyTarget(
            candidate,
            localStatus,
            List.of(peer(remote, MeshPeerState.ADMITTED, 7))));
        assertFalse(ZLinkActorCreationCoordinator.isExactReadyTarget(
            candidate,
            localStatus,
            List.of(peer(remote, MeshPeerState.CONNECTING, 7))));
        assertFalse(ZLinkActorCreationCoordinator.isExactReadyTarget(
            candidate,
            localStatus,
            List.of(peer(remote, MeshPeerState.ADMITTED, 6))));
    }

    private static MeshNodeStatus status(
        RoutingId routingId,
        MeshNodeState state,
        long lifecycleGeneration) {
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
        RoutingId routingId,
        MeshPeerState state,
        long lifecycleGeneration) {
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

    private static ZLinkMeshNodeDescriptor descriptor(
        RoutingId rid,
        int placementWeight) {
        return descriptor(rid, placementWeight, 1);
    }

    private static ZLinkMeshNodeDescriptor descriptor(
        RoutingId rid,
        int placementWeight,
        long lifecycleGeneration) {
        return new ZLinkMeshNodeDescriptor(
            "mesh",
            rid,
            lifecycleGeneration,
            1,
            "tcp://127.0.0.1:1",
            Map.of(),
            1,
            List.of(new ZLinkObjectCapability(
                ZLinkPlacementObjectKind.ACTOR,
                "player",
                ZLinkObjectMaintenancePolicyKind.SNAPSHOT,
                true,
                0)),
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
