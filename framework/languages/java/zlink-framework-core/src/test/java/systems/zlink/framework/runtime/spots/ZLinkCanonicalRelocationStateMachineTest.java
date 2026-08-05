package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeState;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeStatus;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateRelocationCoordinator;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateFence;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityEntry;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityMissing;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityPage;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocation;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocationState;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementCapacityBundle;
import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationStartupScanner;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;
import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

final class ZLinkCanonicalRelocationStateMachineTest {
    private static final ZLinkStoreCancellation OPEN = () -> false;

    @Test
    void productionCommandsDriveStagePublishSealAndCompletion() {
        RoutingId sourceRid = RoutingId.from("source-node");
        RoutingId targetRid = RoutingId.from("target-node");
        String actorId = "actor-a";
        String authorityKey = ZLinkAuthorityKeyCodec.actor(actorId);
        var authority = actorAuthority(
            actorId, sourceRid, authorityKey);
        ZLinkLocationRepository locations = repository(
            authorityKey, authority);
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            locations, new InMemoryRelocationStore());
        UUID relocationId = UUID.randomUUID();
        byte[] root = ZLinkCanonicalActorRelocationEnvelope.encode(
            relocationId,
            actorId,
            3,
            5,
            true,
            new byte[] {1},
            List.of());
        var staged = coordinator.stageRoot(
                new ZLinkAggregateRelocationCoordinator.Request(
                    relocationId,
                    1,
                    List.of(new ZLinkAggregateRelocationCoordinator.Participant(
                        authorityKey,
                        ZLinkPlacementObjectKind.ACTOR,
                        3,
                        5,
                        "store-1",
                        systems.zlink.framework.runtime.internal.locations
                            .ZLinkAuthorityGenerationTransition.NEW_OWNER,
                        authority.payload(),
                        new byte[0])),
                    root,
                    new ZLinkMeshNodeDescriptorKey("mesh", targetRid),
                    12,
                    ZLinkPlacementCapacityBundle.actor(1),
                    new ZLinkLocationOwnerToken("target-owner", 8)),
                OPEN)
            .toCompletableFuture().join();

        AtomicReference<ZLinkCanonicalRelocationStateMachine> source =
            new AtomicReference<>();
        AtomicReference<ZLinkCanonicalRelocationStateMachine> target =
            new AtomicReference<>();
        ZLinkInternalMeshNode sourceNode = node(
            sourceRid, 11, targetRid, target);
        ZLinkInternalMeshNode targetNode = node(
            targetRid, 12, sourceRid, source);
        CountingEndpoint endpoint = new CountingEndpoint();
        source.set(new ZLinkCanonicalRelocationStateMachine(
            sourceNode, "mesh", "source-entry", locations, coordinator,
            new CountingEndpoint()));
        target.set(new ZLinkCanonicalRelocationStateMachine(
            targetNode, "mesh", "target-entry", locations, coordinator,
            endpoint));
        var request = new ZLinkSpotRetireControl.StageRequest(
            new ZLinkSpotRetireControl.Fence(relocationId, 1),
            sourceRid,
            11,
            "source-owner",
            7,
            targetRid,
            12,
            "target-owner",
            8,
            "mesh",
            "target-entry",
            "actor-type",
            false,
            true,
            staged.stored().reference(),
            staged.stored().checksumCrc32c(),
            List.of(new ZLinkSpotRetireControl.ParticipantFence(
                authorityKey,
                1,
                actorId,
                "actor-type",
                true,
                3,
                5)));

        source.get().stage(
                targetRid, request, Duration.ofSeconds(2))
            .toCompletableFuture().join();
        source.get().publish(
                targetRid, request.fence(), Duration.ofSeconds(2))
            .toCompletableFuture().join();
        source.get().finalizeAfterCompletion(
                targetRid, request.fence(), Duration.ofSeconds(2))
            .toCompletableFuture().join();

        assertEquals(1, endpoint.staged.get());
        assertEquals(1, endpoint.published.get());
        assertEquals(1, endpoint.finalized.get());
    }

    @Test
    void publishedStoreStateRebuildsTargetAfterProcessRestart() {
        RoutingId sourceRid = RoutingId.from("source-node");
        RoutingId targetRid = RoutingId.from("target-node");
        String actorId = "actor-a";
        String authorityKey = ZLinkAuthorityKeyCodec.actor(actorId);
        UUID relocationId = UUID.randomUUID();
        byte[] root = ZLinkCanonicalActorRelocationEnvelope.encode(
            relocationId,
            actorId,
            3,
            5,
            true,
            new byte[] {1},
            List.of());
        ZLinkAuthoritySnapshot targetAuthority =
            targetActorAuthority(actorId, targetRid);
        var candidate = new ZLinkRelocationStartupScanner.Candidate(
            "relocation-root",
            0,
            new ZLinkAggregateFence(relocationId, 1),
            "source-owner",
            7,
            sourceRid,
            11,
            new ZLinkLocationOwnerToken("target-owner", 8),
            targetRid,
            12,
            false,
            new ZLinkAggregateRelocationCoordinator.PublishedRoot(
                "relocation-root",
                0,
                root,
                new byte[] {1},
                Map.of(authorityKey, 6L)),
            List.of(new ZLinkAuthorityEntry(
                authorityKey, targetAuthority)));
        AtomicInteger ackCount = new AtomicInteger();
        CountingEndpoint endpoint = new CountingEndpoint();
        ZLinkInternalMeshNode targetNode = capturingNode(
            targetRid, 12, ackCount);
        var machine = new ZLinkCanonicalRelocationStateMachine(
            targetNode,
            "mesh",
            "target-entry",
            repository(authorityKey, targetAuthority),
            new ZLinkAggregateRelocationCoordinator(
                repository(authorityKey, targetAuthority),
                new InMemoryRelocationStore()),
            endpoint);

        machine.recoverPublished(candidate).toCompletableFuture().join();

        var coordinator =
            new ZLinkCanonicalRelocationProtocol.Coordinator(
                "source-owner", 7, sourceRid, 11, "source-store");
        var source = new ZLinkCanonicalRelocationProtocol.Source(
            sourceRid, 11, "source-owner", 7);
        var object = new ZLinkCanonicalRelocationProtocol.ObjectFence(
            1, actorId, "", 3, 5);
        var control = new ZLinkCanonicalRelocationProtocol.ControlData(
            relocationId,
            1,
            coordinator,
            ZLinkCanonicalRelocationProtocol.SOURCE,
            1,
            1,
            source,
            4,
            object,
            0,
            0);
        machine.apply(
                sourceRid,
                ServiceWireConstants.COMMAND_RELOCATION_DATA,
                ZLinkCanonicalRelocationProtocol.encodeControlData(control))
            .toCompletableFuture().join();
        var seal = new ZLinkCanonicalRelocationProtocol.Seal(
            relocationId,
            1,
            coordinator,
            ZLinkCanonicalRelocationProtocol.SOURCE,
            false,
            List.of(new ZLinkCanonicalRelocationProtocol.Terminal(1, 1)));
        machine.apply(
                sourceRid,
                ServiceWireConstants.COMMAND_RELOCATION_SEAL,
                ZLinkCanonicalRelocationProtocol.encodeSeal(seal))
            .toCompletableFuture().join();
        var complete = new ZLinkCanonicalRelocationProtocol.Complete(
            relocationId,
            1,
            coordinator,
            ZLinkCanonicalRelocationProtocol.SOURCE,
            source,
            1);
        machine.apply(
                sourceRid,
                ServiceWireConstants.COMMAND_RELOCATION_COMPLETE,
                ZLinkCanonicalRelocationProtocol.encodeComplete(complete))
            .toCompletableFuture().join();

        assertEquals(1, endpoint.staged.get());
        assertEquals(1, endpoint.published.get());
        assertEquals(1, endpoint.finalized.get());
        assertTrue(ackCount.get() >= 2,
            "recovered command 31 and seal both send canonical replies");
    }

    private static ZLinkAuthoritySnapshot actorAuthority(
        String actorId,
        RoutingId sourceRid,
        String authorityKey) {
        byte[] payload = new ZLinkActorAuthorityPayloadCodec().encode(
            ZLinkActorAuthorityPayloadCodec.State.READY,
            "actor-type",
            actorId,
            "source-entry",
            1,
            1,
            "source-owner",
            7,
            "mesh",
            sourceRid,
            11);
        return new ZLinkAuthoritySnapshot(
            "store-1",
            payload,
            3,
            5,
            "source-owner",
            7,
            new ZLinkPlacementAllocation(
                ZLinkPlacementAllocationState.ACTIVE,
                ZLinkPlacementObjectKind.ACTOR,
                "actor-type",
                new ZLinkMeshNodeDescriptorKey("mesh", sourceRid),
                11,
                ZLinkPlacementCapacityBundle.actor(1)),
            Instant.now());
    }

    private static ZLinkAuthoritySnapshot targetActorAuthority(
        String actorId,
        RoutingId targetRid) {
        byte[] payload = new ZLinkActorAuthorityPayloadCodec().encode(
            ZLinkActorAuthorityPayloadCodec.State.READY,
            "actor-type",
            actorId,
            "target-entry",
            1,
            1,
            "target-owner",
            8,
            "mesh",
            targetRid,
            12);
        return new ZLinkAuthoritySnapshot(
            "store-2",
            payload,
            3,
            6,
            "target-owner",
            8,
            new ZLinkPlacementAllocation(
                ZLinkPlacementAllocationState.ACTIVE,
                ZLinkPlacementObjectKind.ACTOR,
                "actor-type",
                new ZLinkMeshNodeDescriptorKey("mesh", targetRid),
                12,
                ZLinkPlacementCapacityBundle.actor(1)),
            Instant.now());
    }

    private static ZLinkLocationRepository repository(
        String key,
        ZLinkAuthoritySnapshot snapshot) {
        return (ZLinkLocationRepository) Proxy.newProxyInstance(
            ZLinkLocationRepository.class.getClassLoader(),
            new Class<?>[] {ZLinkLocationRepository.class},
            (proxy, method, args) -> switch (method.getName()) {
                case "read" -> CompletableFuture.completedFuture(
                    key.equals(args[0])
                        ? snapshot : new ZLinkAuthorityMissing(Instant.now()));
                case "list" -> CompletableFuture.completedFuture(
                    new ZLinkAuthorityPage(List.of(), Optional.empty()));
                default -> throw new UnsupportedOperationException(
                    method.getName());
            });
    }

    private static ZLinkInternalMeshNode node(
        RoutingId localRid,
        long generation,
        RoutingId peerRid,
        AtomicReference<ZLinkCanonicalRelocationStateMachine> peer) {
        MeshNodeStatus status = new MeshNodeStatus(
            MeshNodeState.READY,
            localRid,
            "mesh",
            "",
            generation,
            1,
            0,
            1,
            1,
            0,
            0,
            0,
            0,
            0,
            0);
        return (ZLinkInternalMeshNode) Proxy.newProxyInstance(
            ZLinkInternalMeshNode.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalMeshNode.class},
            (proxy, method, args) -> switch (method.getName()) {
                case "status" -> status;
                case "sendCanonicalRelocationControl" ->
                    peer.get().apply(
                        localRid,
                        Byte.toUnsignedInt(((byte[]) args[1])[3]),
                        (byte[]) args[1]);
                default -> throw new UnsupportedOperationException(
                    method.getName());
            });
    }

    private static ZLinkInternalMeshNode capturingNode(
        RoutingId localRid,
        long generation,
        AtomicInteger sends) {
        MeshNodeStatus status = new MeshNodeStatus(
            MeshNodeState.READY,
            localRid,
            "mesh",
            "",
            generation,
            1,
            0,
            1,
            1,
            0,
            0,
            0,
            0,
            0,
            0);
        return (ZLinkInternalMeshNode) Proxy.newProxyInstance(
            ZLinkInternalMeshNode.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalMeshNode.class},
            (proxy, method, args) -> switch (method.getName()) {
                case "status" -> status;
                case "sendCanonicalRelocationControl" -> {
                    sends.incrementAndGet();
                    yield CompletableFuture.completedFuture(null);
                }
                default -> throw new UnsupportedOperationException(
                    method.getName());
            });
    }

    private static final class CountingEndpoint
        implements ZLinkSpotRetireControl.TargetEndpoint {
        private final AtomicInteger staged = new AtomicInteger();
        private final AtomicInteger published = new AtomicInteger();
        private final AtomicInteger finalized = new AtomicInteger();

        @Override
        public CompletionStage<Void> stage(
            ZLinkSpotRetireControl.StageRequest request) {
            staged.incrementAndGet();
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> publish(
            ZLinkSpotRetireControl.StageRequest request) {
            published.incrementAndGet();
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> abort(
            ZLinkSpotRetireControl.StageRequest request) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> finalizeAfterCompletion(
            ZLinkSpotRetireControl.StageRequest request) {
            finalized.incrementAndGet();
            return CompletableFuture.completedFuture(null);
        }
    }
}
