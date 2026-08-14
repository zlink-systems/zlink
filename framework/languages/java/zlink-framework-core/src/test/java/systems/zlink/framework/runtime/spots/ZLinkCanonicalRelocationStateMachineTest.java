package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;

import java.lang.reflect.Proxy;
import java.lang.reflect.InvocationTargetException;
import java.time.Duration;
import java.time.Instant;
import java.util.Map;
import java.util.List;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkObjectCapability;
import systems.zlink.framework.locations.ZLinkObjectMaintenancePolicyKind;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeState;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeStatus;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateRelocationCoordinator;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteIntent;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectCommitResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectReservationRequest;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectReserved;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimed;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocation;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocationState;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementCapacityBundle;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;
import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

final class ZLinkCanonicalRelocationStateMachineTest {
    private static final ZLinkStoreCancellation OPEN = () -> false;

    @Test
    void prepareReadyAndOneWayCutoverAreTheOnlyAttemptTransitions() {
        RoutingId sourceRid = RoutingId.from("source-node");
        RoutingId targetRid = RoutingId.from("target-node");
        String actorId = "actor-a";
        String authorityKey = ZLinkAuthorityKeyCodec.actor(actorId);
        ZLinkInMemoryLocationStore locations =
            new ZLinkInMemoryLocationStore();
        ZLinkLocationOwnerToken sourceOwner = assertInstanceOf(
            ZLinkOwnerLeaseClaimed.class,
            locations.claimOwnerLease(
                    "source-owner", Duration.ofMinutes(5))
                .toCompletableFuture().join()).token();
        ZLinkLocationOwnerToken targetOwner = assertInstanceOf(
            ZLinkOwnerLeaseClaimed.class,
            locations.claimOwnerLease(
                    "target-owner", Duration.ofMinutes(5))
                .toCompletableFuture().join()).token();
        locations.updateMeshNode(
                descriptor(sourceRid, 11, sourceOwner, "source-entry"),
                ZLinkLocationWriteIntent.NEW_CLAIM)
            .toCompletableFuture().join();
        locations.updateMeshNode(
                descriptor(targetRid, 12, targetOwner, "target-entry"),
                ZLinkLocationWriteIntent.NEW_CLAIM)
            .toCompletableFuture().join();
        byte[] sourceAuthority = actorAuthority(
            actorId, sourceRid, sourceOwner).payload();
        var reservation = assertInstanceOf(
            ZLinkObjectReserved.class,
            locations.reserve(
                    new ZLinkObjectReservationRequest(
                        ZLinkPlacementObjectKind.ACTOR,
                        authorityKey,
                        "actor-type",
                        "creation-root",
                        new byte[32],
                        32,
                        new ZLinkMeshNodeDescriptorKey("mesh", sourceRid),
                        11,
                        sourceOwner,
                        sourceAuthority,
                        ZLinkPlacementCapacityBundle.actor(1)),
                    OPEN)
                .toCompletableFuture().join()).reservation();
        assertEquals(
            ZLinkObjectCommitResult.COMMITTED,
            locations.commit(reservation, sourceAuthority, OPEN)
                .toCompletableFuture().join());
        ZLinkAuthoritySnapshot sourceSnapshot = assertInstanceOf(
            ZLinkAuthoritySnapshot.class,
            locations.read(authorityKey, OPEN).toCompletableFuture().join());
        byte[] targetAuthority = new ZLinkActorAuthorityPayloadCodec().encode(
            ZLinkActorAuthorityPayloadCodec.State.READY,
            "actor-type", actorId, "target-entry",
            12, 1, targetOwner.ownerId(), targetOwner.leaseGeneration(),
            "mesh", targetRid, 12);
        AtomicInteger commitAttempts = new AtomicInteger();
        ZLinkLocationRepository retryingLocations =
            (ZLinkLocationRepository) Proxy.newProxyInstance(
                ZLinkLocationRepository.class.getClassLoader(),
                new Class<?>[] {ZLinkLocationRepository.class},
                (proxy, method, args) -> {
                    if (method.getName().equals("commitAggregate")
                        && commitAttempts.getAndIncrement() == 0) {
                        return CompletableFuture.failedFuture(
                            new IllegalStateException(
                                "transient Location Store failure"));
                    }
                    try {
                        return method.invoke(locations, args);
                    } catch (InvocationTargetException failure) {
                        throw failure.getCause();
                    }
                });
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            retryingLocations, new InMemoryRelocationStore());
        UUID relocationId = UUID.randomUUID();
        byte[] root = ZLinkCanonicalActorRelocationEnvelope.encode(
            relocationId,
            actorId,
            sourceSnapshot.objectGeneration(),
            sourceSnapshot.authorityOwnerGeneration(),
            true,
            new byte[] {1},
            List.of());
        var stagedRoot = coordinator.stageRoot(
                new ZLinkAggregateRelocationCoordinator.Request(
                    relocationId,
                    1,
                    List.of(new ZLinkAggregateRelocationCoordinator.Participant(
                        authorityKey,
                        ZLinkPlacementObjectKind.ACTOR,
                        sourceSnapshot.objectGeneration(),
                        sourceSnapshot.authorityOwnerGeneration(),
                        sourceSnapshot.storeVersion(),
                        systems.zlink.framework.runtime.internal.locations
                            .ZLinkAuthorityGenerationTransition.NEW_OWNER,
                        targetAuthority,
                        new byte[0])),
                    root,
                    new ZLinkMeshNodeDescriptorKey("mesh", targetRid),
                    12,
                    ZLinkPlacementCapacityBundle.actor(1),
                    targetOwner),
                OPEN)
            .toCompletableFuture().join();

        AtomicReference<ZLinkCanonicalRelocationStateMachine> source =
            new AtomicReference<>();
        AtomicReference<ZLinkCanonicalRelocationStateMachine> target =
            new AtomicReference<>();
        var sourceCommands = new CopyOnWriteArrayList<Integer>();
        var targetCommands = new CopyOnWriteArrayList<Integer>();
        CountingEndpoint endpoint = new CountingEndpoint();
        source.set(new ZLinkCanonicalRelocationStateMachine(
            node(sourceRid, 11, target, sourceCommands),
            "mesh", "source-entry", retryingLocations, coordinator,
            new CountingEndpoint()));
        target.set(new ZLinkCanonicalRelocationStateMachine(
            node(targetRid, 12, source, targetCommands),
            "mesh", "target-entry", retryingLocations, coordinator, endpoint));
        var request = new ZLinkSpotRetireControl.StageRequest(
            new ZLinkSpotRetireControl.Fence(relocationId, 1),
            sourceRid, 11, sourceOwner.ownerId(), sourceOwner.leaseGeneration(),
            targetRid, 12, targetOwner.ownerId(), targetOwner.leaseGeneration(),
            "mesh", "target-entry", "actor-type", false, true,
            stagedRoot.stored().reference(),
            stagedRoot.stored().checksumCrc32c(),
            List.of(new ZLinkSpotRetireControl.ParticipantFence(
                authorityKey,
                1,
                actorId,
                "actor-type",
                true,
                sourceSnapshot.objectGeneration(),
                sourceSnapshot.authorityOwnerGeneration())),
            List.of());

        source.get().stage(targetRid, request, Duration.ofSeconds(2))
            .toCompletableFuture().join();
        source.get().relay(
                targetRid,
                request.fence(),
                new byte[] {1, 2, 3},
                Duration.ofSeconds(2))
            .toCompletableFuture().join();
        source.get().publish(targetRid, request.fence(), Duration.ofSeconds(2))
            .toCompletableFuture().join();

        assertEquals(List.of(
            ServiceWireConstants.COMMAND_RELOCATION_PREPARE,
            ServiceWireConstants.COMMAND_RELOCATION_DATA,
            ServiceWireConstants.COMMAND_RELOCATION_CUTOVER), sourceCommands);
        assertEquals(List.of(
            ServiceWireConstants.COMMAND_RELOCATION_READY), targetCommands);
        assertEquals(1, endpoint.staged.get());
        assertEquals(1, endpoint.published.get());
        assertEquals(1, endpoint.relayed.get());
        assertEquals(2, commitAttempts.get(),
            "the target retries the same CAS fence after a retryable failure");
        ZLinkAuthoritySnapshot published = assertInstanceOf(
            ZLinkAuthoritySnapshot.class,
            locations.read(authorityKey, OPEN).toCompletableFuture().join());
        assertEquals(targetOwner.ownerId(), published.ownerId(),
            "only the target command handler performs the owner CAS");
    }

    private static ZLinkAuthoritySnapshot actorAuthority(
        String actorId,
        RoutingId sourceRid,
        ZLinkLocationOwnerToken sourceOwner) {
        byte[] payload = new ZLinkActorAuthorityPayloadCodec().encode(
            ZLinkActorAuthorityPayloadCodec.State.READY,
            "actor-type", actorId, "source-entry",
            1, 1, sourceOwner.ownerId(), sourceOwner.leaseGeneration(),
            "mesh", sourceRid, 11);
        return new ZLinkAuthoritySnapshot(
            "store-1", payload, 3, 5,
            sourceOwner.ownerId(), sourceOwner.leaseGeneration(),
            new ZLinkPlacementAllocation(
                ZLinkPlacementAllocationState.ACTIVE,
                ZLinkPlacementObjectKind.ACTOR,
                "actor-type",
                new ZLinkMeshNodeDescriptorKey("mesh", sourceRid),
                11,
                ZLinkPlacementCapacityBundle.actor(1)),
            Instant.now());
    }

    private static ZLinkMeshNodeDescriptor descriptor(
        RoutingId rid,
        long generation,
        ZLinkLocationOwnerToken owner,
        String entrySpotId) {
        return new ZLinkMeshNodeDescriptor(
            "mesh",
            rid,
            generation,
            1,
            "tcp://127.0.0.1:" + (7000 + generation),
            Map.of(),
            1,
            List.of(new ZLinkObjectCapability(
                ZLinkPlacementObjectKind.ACTOR,
                "actor-type",
                ZLinkObjectMaintenancePolicyKind.SNAPSHOT,
                true,
                0)),
            ZLinkMeshNodeObjectRole.SERVER,
            Optional.of(entrySpotId),
            100,
            new ZLinkPlacementCapacity(
                new ZLinkCapacityUsage(0, 0, 8),
                new ZLinkCapacityUsage(0, 0, 0),
                List.of()),
            new ZLinkActivationConcurrency(0, 32),
            Optional.empty(),
            ZLinkFrameworkRuntimeState.SERVING,
            "security",
            owner.ownerId(),
            owner.leaseGeneration(),
            Instant.now());
    }

    private static ZLinkInternalMeshNode node(
        RoutingId localRid,
        long generation,
        AtomicReference<ZLinkCanonicalRelocationStateMachine> peer,
        List<Integer> commands) {
        MeshNodeStatus status = new MeshNodeStatus(
            MeshNodeState.READY, localRid, "mesh", "", generation,
            1, 0, 1, 1, 0, 0, 0, 0, 0, 0);
        return (ZLinkInternalMeshNode) Proxy.newProxyInstance(
            ZLinkInternalMeshNode.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalMeshNode.class},
            (proxy, method, args) -> switch (method.getName()) {
                case "status" -> status;
                case "sendCanonicalRelocationControl" -> {
                    byte[] encoded = (byte[]) args[1];
                    int command = Byte.toUnsignedInt(encoded[3]);
                    commands.add(command);
                    yield peer.get().apply(localRid, command, encoded);
                }
                default -> throw new UnsupportedOperationException(
                    method.getName());
            });
    }

    private static final class CountingEndpoint
        implements ZLinkSpotRetireControl.TargetEndpoint {
        private final AtomicInteger staged = new AtomicInteger();
        private final AtomicInteger published = new AtomicInteger();
        private final AtomicInteger relayed = new AtomicInteger();

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
        public CompletionStage<Void> stageRelayedRecord(
            ZLinkSpotRetireControl.StageRequest request,
            byte[] frozenRecord) {
            assertArrayEquals(new byte[] {1, 2, 3}, frozenRecord);
            relayed.incrementAndGet();
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> abort(
            ZLinkSpotRetireControl.StageRequest request) {
            return CompletableFuture.completedFuture(null);
        }

    }
}
