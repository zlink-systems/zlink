package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.function.Consumer;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkObjectCapability;
import systems.zlink.framework.locations.ZLinkObjectMaintenancePolicyKind;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.locations.ZLinkSpotTypeCapacity;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeState;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeStatus;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerEntry;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshDispatchRecord;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;

final class ZLinkSpotRuntimeTargetSelectionTest {
    private static final RoutingId CONNECTED =
        RoutingId.from("connected-node");
    private static final RoutingId DISCONNECTED =
        RoutingId.from("disconnected-node");
    // RecordingMeshNode is the local node and admits no peers, so only this
    // RID resolves to a route-ready target.
    private static final RoutingId LOCAL =
        RoutingId.from("source-node");
    private static final RoutingId REPLACEMENT =
        RoutingId.from("replacement-node");
    private static final String ZONE_TYPE = "zone";
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

    @Test
    void manualObjectPeerReplacementCarriesTheDescriptorFence() {
        ZLinkMeshNodeDescriptor target = descriptor(CONNECTED, 1);
        RecordingMeshNode source = new RecordingMeshNode();

        ZLinkSpotRuntime.ManualObjectPeerIntent intent =
            ZLinkSpotRuntime.ensureManualObjectPeerIntent(
                source,
                target,
                null)
                .orElseThrow();

        assertTrue(source.replaced);
        assertEquals(target.endpoint(), source.endpoint);
        assertEquals(target.rid(), source.routingId);
        assertEquals(target.lifecycleGeneration(), source.lifecycleGeneration);
        assertEquals(target.securityIdentity(), source.securityIdentity);
        assertEquals(source.intentId, intent.connectionIntentId());
    }

    @Test
    void aServingNodeWithRoomIsSelectedDirectly() {
        assertEquals(
            ZLinkSpotRuntime.UserSpotPlacementVerdict.SELECT,
            ZLinkSpotRuntime.userSpotPlacementVerdict(
                List.of(zoneNode(LOCAL, ZLinkFrameworkRuntimeState.SERVING, 1)),
                ZONE_TYPE,
                new RecordingMeshNode(),
                Set.of()));
    }

    @Test
    void everyServingNodeBeingFullIsATerminalCapacityVerdict() {
        assertEquals(
            ZLinkSpotRuntime.UserSpotPlacementVerdict.TERMINAL,
            ZLinkSpotRuntime.userSpotPlacementVerdict(
                List.of(zoneNode(LOCAL, ZLinkFrameworkRuntimeState.SERVING, 2)),
                ZONE_TYPE,
                new RecordingMeshNode(),
                Set.of()));
    }

    @Test
    void aReplacementStillPreparingKeepsTheCreateRetryingInsteadOfFailing() {
        // ZW-G3: the replacement node publishes its descriptor while still
        // Preparing — weight 0, no admitted route — and only then turns
        // Serving. The one Serving node left is full, so a single read finds
        // no candidate. That is the mesh converging, not a capacity verdict.
        assertEquals(
            ZLinkSpotRuntime.UserSpotPlacementVerdict.RETRY,
            ZLinkSpotRuntime.userSpotPlacementVerdict(
                List.of(
                    zoneNode(LOCAL, ZLinkFrameworkRuntimeState.SERVING, 2),
                    preparingZoneNode(REPLACEMENT)),
                ZONE_TYPE,
                new RecordingMeshNode(),
                Set.of()));
    }

    @Test
    void aServingNodeWithRoomWhoseRouteIsNotAdmittedYetKeepsRetrying() {
        assertEquals(
            ZLinkSpotRuntime.UserSpotPlacementVerdict.RETRY,
            ZLinkSpotRuntime.userSpotPlacementVerdict(
                List.of(
                    zoneNode(LOCAL, ZLinkFrameworkRuntimeState.SERVING, 2),
                    zoneNode(
                        REPLACEMENT, ZLinkFrameworkRuntimeState.SERVING, 1)),
                ZONE_TYPE,
                new RecordingMeshNode(),
                Set.of()));
    }

    @Test
    void anExcludedPreparingNodeIsNotATargetTheCreateWaitsFor() {
        assertEquals(
            ZLinkSpotRuntime.UserSpotPlacementVerdict.TERMINAL,
            ZLinkSpotRuntime.userSpotPlacementVerdict(
                List.of(
                    zoneNode(LOCAL, ZLinkFrameworkRuntimeState.SERVING, 2),
                    preparingZoneNode(REPLACEMENT)),
                ZONE_TYPE,
                new RecordingMeshNode(),
                Set.of(new ZLinkMeshNodeDescriptorKey("mesh", REPLACEMENT))));
    }

    @Test
    void anEmptyMeshKeepsRetryingBecauseNoCapacityIsKnownYet() {
        assertEquals(
            ZLinkSpotRuntime.UserSpotPlacementVerdict.RETRY,
            ZLinkSpotRuntime.userSpotPlacementVerdict(
                List.of(),
                ZONE_TYPE,
                new RecordingMeshNode(),
                Set.of()));
    }

    @Test
    void theRefreshBackoffGrowsAndIsCapped() {
        assertEquals(1L, ZLinkSpotRuntime.placementRefreshBackoffMillis(0));
        assertEquals(2L, ZLinkSpotRuntime.placementRefreshBackoffMillis(1));
        assertEquals(64L, ZLinkSpotRuntime.placementRefreshBackoffMillis(6));
        assertEquals(64L, ZLinkSpotRuntime.placementRefreshBackoffMillis(40));
    }

    private static ZLinkMeshNodeDescriptor preparingZoneNode(RoutingId rid) {
        // A node that has not published Serving advertises weight 0.
        return zoneNode(rid, ZLinkFrameworkRuntimeState.PREPARING, 0, 0);
    }

    private static ZLinkMeshNodeDescriptor zoneNode(
        RoutingId rid,
        ZLinkFrameworkRuntimeState state,
        int activeSpotsOfType) {
        return zoneNode(rid, state, activeSpotsOfType, 100);
    }

    private static ZLinkMeshNodeDescriptor zoneNode(
        RoutingId rid,
        ZLinkFrameworkRuntimeState state,
        int activeSpotsOfType,
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
                ZLinkPlacementObjectKind.USER_SPOT,
                ZONE_TYPE,
                ZLinkObjectMaintenancePolicyKind.SNAPSHOT,
                true,
                2)),
            ZLinkMeshNodeObjectRole.SERVER,
            Optional.of("entry-00000000-0000-4000-8000-000000000001"),
            placementWeight,
            new ZLinkPlacementCapacity(
                new ZLinkCapacityUsage(0, 0, 0),
                new ZLinkCapacityUsage(activeSpotsOfType, 0, 0),
                List.of(new ZLinkSpotTypeCapacity(
                    ZLinkPlacementObjectKind.USER_SPOT,
                    ZONE_TYPE,
                    new ZLinkCapacityUsage(activeSpotsOfType, 0, 2)))),
            new ZLinkActivationConcurrency(0, 8),
            Optional.empty(),
            state,
            "security",
            "owner",
            1,
            NOW);
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

    private static final class RecordingMeshNode implements ZLinkInternalMeshNode {
        private final RoutingId localRid = RoutingId.from("source-node");
        private String endpoint;
        private RoutingId routingId;
        private long lifecycleGeneration;
        private String securityIdentity;
        private long intentId;
        private boolean replaced;

        @Override public String name() { return "source"; }
        @Override public void setBind(String value) { }
        @Override public void addChannel(String value) { }
        @Override public void setChannelWeight(String value, int weight) { }
        @Override public void setRoutingId(RoutingId value) { }
        @Override public void start() { }
        @Override public long connectPeer(String value) { return 1L; }
        @Override public long connectPeer(String value, RoutingId expected) {
            return 1L;
        }
        @Override public long replacePeerConnection(
            String value,
            RoutingId expected,
            long generation,
            String security) {
            endpoint = value;
            routingId = expected;
            lifecycleGeneration = generation;
            securityIdentity = security;
            intentId = 11L;
            replaced = true;
            return intentId;
        }
        @Override public MeshNodeStatus status() {
            return new MeshNodeStatus(
                MeshNodeState.READY,
                localRid,
                "mesh",
                "inproc://source",
                1L,
                1L,
                1,
                1,
                1,
                0,
                0L,
                0L,
                0L,
                0,
                0L);
        }
        @Override public List<MeshPeerEntry> peers() { return List.of(); }
        @Override public List<Long> connectionIntentIds() { return List.of(); }
        @Override public void startDispatch(Consumer<ZLinkMeshDispatchRecord> receiver) { }
        @Override public void close() { }
    }
}
