package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
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
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;

final class ZLinkSpotRuntimeTargetSelectionTest {
    private static final RoutingId CONNECTED =
        RoutingId.from("connected-node");
    private static final RoutingId DISCONNECTED =
        RoutingId.from("disconnected-node");
    private static final Instant NOW = Instant.parse(
        "2026-01-01T00:00:00Z");

    @Test
    void connectedCandidatesArePreferredWhenTheLocationStoreListsBoth() {
        ZLinkMeshNodeDescriptor disconnected = descriptor(DISCONNECTED, 100);
        ZLinkMeshNodeDescriptor connected = descriptor(CONNECTED, 1);

        List<ZLinkMeshNodeDescriptor> selected =
            ZLinkSpotRuntime.preferConnectedInstanceTargets(
                List.of(disconnected, connected), Set.of(CONNECTED));

        assertEquals(List.of(connected), selected);
    }

    @Test
    void allCandidatesRemainAvailableUntilAConnectedCandidateIsKnown() {
        ZLinkMeshNodeDescriptor first = descriptor(DISCONNECTED, 100);
        ZLinkMeshNodeDescriptor second = descriptor(
            RoutingId.from("other-node"), 1);
        List<ZLinkMeshNodeDescriptor> candidates = List.of(first, second);

        List<ZLinkMeshNodeDescriptor> selected =
            ZLinkSpotRuntime.preferConnectedInstanceTargets(
                candidates, Set.of());

        assertEquals(candidates, selected);
    }

    private static ZLinkMeshNodeDescriptor descriptor(
        RoutingId rid,
        int placementWeight) {
        return new ZLinkMeshNodeDescriptor(
            "mesh",
            rid,
            1,
            1,
            "tcp://127.0.0.1:1",
            Map.of(),
            1,
            List.of(new ZLinkObjectCapability(
                ZLinkPlacementObjectKind.INSTANCE_SPOT,
                "player-quest",
                ZLinkObjectMaintenancePolicyKind.SNAPSHOT,
                true,
                0)),
            ZLinkMeshNodeObjectRole.SERVER,
            Optional.of("entry-00000000-0000-4000-8000-000000000001"),
            placementWeight,
            new ZLinkPlacementCapacity(
                new ZLinkCapacityUsage(0, 0, 0),
                new ZLinkCapacityUsage(0, 0, 8),
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
