package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;

import java.time.Clock;
import java.time.Duration;
import java.time.Instant;
import java.time.ZoneOffset;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.UUID;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;

final class ZLinkInMemoryDescriptorAdmissionTest {
    private static final Instant NOW =
        Instant.parse("2026-07-23T00:00:00Z");
    private static final RoutingId SOURCE_RID =
        RoutingId.from("source-node");
    private static final RoutingId TARGET_RID =
        RoutingId.from("target-node");

    @Test
    void creationAdmissionUsesExactCapabilityProfileAndNodeTypeLimits()
        throws Exception {
        ZLinkInMemoryLocationStore store = store();
        ZLinkLocationOwnerToken owner = claim(store, "source-owner");
        ZLinkMeshNodeDescriptor initial = descriptor(
            SOURCE_RID,
            11,
            1,
            owner,
            ZLinkFrameworkRuntimeState.SERVING,
            2,
            4,
            4,
            true);
        publish(store, initial, ZLinkLocationWriteIntent.NEW_CLAIM);

        assertInstanceOf(
            ZLinkObjectConflict.class,
            store.reserve(
                    creation(
                        "unsupported-type",
                        initial,
                        owner,
                        "missing"),
                    () -> false)
                .toCompletableFuture().get());
        assertEquals(
            actorCapacity(0, 0, 2),
            capacity(store, SOURCE_RID));

        ZLinkObjectReservation first = reserve(
            store,
            creation(
                "player-1",
                initial,
                owner,
                "player"));
        assertEquals(
            actorCapacity(0, 1, 2),
            capacity(store, SOURCE_RID));
        assertEquals(
            ZLinkObjectCommitResult.COMMITTED,
            store.commit(first, new byte[] {2}, () -> false)
                .toCompletableFuture().get());

        ZLinkMeshNodeDescriptor changedImmutableLimits = descriptor(
            SOURCE_RID,
            11,
            2,
            owner,
            ZLinkFrameworkRuntimeState.SERVING,
            1,
            4,
            1,
            true);
        assertEquals(
            ZLinkLocationWriteStatus.IGNORED_STALE,
            store.updateMeshNode(
                    changedImmutableLimits,
                    ZLinkLocationWriteIntent.RENEW)
                .toCompletableFuture().get().status());

        ZLinkObjectReservation second = reserve(
            store,
            creation(
                "player-2",
                initial,
                owner,
                "player"));
        ZLinkMeshNodeDescriptor reducedTypeLimit = descriptor(
            SOURCE_RID,
            11,
            4,
            owner,
            ZLinkFrameworkRuntimeState.SERVING,
            1,
            2,
            4,
            true);
        assertEquals(
            ZLinkLocationWriteStatus.IGNORED_STALE,
            store.updateMeshNode(
                    reducedTypeLimit,
                    ZLinkLocationWriteIntent.RENEW)
                .toCompletableFuture().get().status());
        assertEquals(
            ZLinkObjectCommitResult.COMMITTED,
            store.commit(second, new byte[] {3}, () -> false)
                .toCompletableFuture().get());
        assertEquals(
            actorCapacity(2, 0, 2),
            capacity(store, SOURCE_RID));
        assertInstanceOf(
            ZLinkPlacementCapacityExhausted.class,
            store.reserve(
                    creation(
                        "player-active-limit",
                        initial,
                        owner,
                        "player"),
                    () -> false)
                .toCompletableFuture().get());
        assertEquals(
            actorCapacity(2, 0, 2),
            capacity(store, SOURCE_RID));
    }

    @Test
    void relocationAdmissionRevalidatesExactTargetAndCapacityAtCommit()
        throws Exception {
        ZLinkInMemoryLocationStore store = store();
        ZLinkLocationOwnerToken sourceOwner =
            claim(store, "source-owner");
        ZLinkLocationOwnerToken targetOwner =
            claim(store, "target-owner");
        ZLinkMeshNodeDescriptor sourceDescriptor = descriptor(
            SOURCE_RID,
            31,
            1,
            sourceOwner,
            ZLinkFrameworkRuntimeState.SERVING,
            4,
            4,
            4,
            true);
        ZLinkMeshNodeDescriptor unsupportedTarget = descriptor(
            TARGET_RID,
            41,
            1,
            targetOwner,
            ZLinkFrameworkRuntimeState.SERVING,
            1,
            1,
            1,
            false);
        publish(
            store,
            sourceDescriptor,
            ZLinkLocationWriteIntent.NEW_CLAIM);
        publish(
            store,
            unsupportedTarget,
            ZLinkLocationWriteIntent.NEW_CLAIM);
        ZLinkAuthoritySnapshot first = createActive(
            store,
            "relocate-1",
            sourceDescriptor,
            sourceOwner);
        ZLinkAuthoritySnapshot second = createActive(
            store,
            "relocate-2",
            sourceDescriptor,
            sourceOwner);

        assertInstanceOf(
            ZLinkRelocationCapacityTargetUnavailable.class,
            store.reserveRelocationCapacity(
                    relocation(
                        "relocate-1",
                        first,
                        sourceDescriptor,
                        sourceOwner,
                        unsupportedTarget,
                        targetOwner),
                    () -> false)
                .toCompletableFuture().get());
        assertEquals(
            actorCapacity(0, 0, 1),
            capacity(store, TARGET_RID));

        assertEquals(
            ZLinkLocationWriteStatus.STORED,
            store.removeMeshNode(
                    descriptorKey(unsupportedTarget),
                    targetOwner)
                .toCompletableFuture().get());
        ZLinkMeshNodeDescriptor targetDescriptor = descriptor(
            TARGET_RID,
            42,
            1,
            targetOwner,
            ZLinkFrameworkRuntimeState.SERVING,
            1,
            1,
            1,
            true);
        publish(
            store,
            targetDescriptor,
            ZLinkLocationWriteIntent.NEW_CLAIM);
        assertInstanceOf(
            ZLinkRelocationCapacityTargetUnavailable.class,
            store.reserveRelocationCapacity(
                    relocation(
                        "relocate-1",
                        first,
                        sourceDescriptor,
                        sourceOwner,
                        unsupportedTarget,
                        targetOwner),
                    () -> false)
                .toCompletableFuture().get());
        assertInstanceOf(
            ZLinkRelocationCapacityTargetUnavailable.class,
            store.reserveRelocationCapacity(
                    relocation(
                        "relocate-1",
                        first,
                        sourceDescriptor,
                        sourceOwner,
                        targetDescriptor,
                        new ZLinkLocationOwnerToken(
                            targetOwner.ownerId(),
                            targetOwner.leaseGeneration() + 1)),
                    () -> false)
                .toCompletableFuture().get());

        ZLinkRelocationCapacityReserved capacity =
            assertInstanceOf(
                ZLinkRelocationCapacityReserved.class,
                store.reserveRelocationCapacity(
                        relocation(
                            "relocate-1",
                            first,
                            sourceDescriptor,
                            sourceOwner,
                            targetDescriptor,
                            targetOwner),
                        () -> false)
                    .toCompletableFuture().get());
        assertInstanceOf(
            ZLinkRelocationCapacityExhausted.class,
            store.reserveRelocationCapacity(
                    relocation(
                        "relocate-2",
                        second,
                        sourceDescriptor,
                        sourceOwner,
                        targetDescriptor,
                        targetOwner),
                    () -> false)
                .toCompletableFuture().get());
        assertEquals(
            actorCapacity(0, 1, 1),
            capacity(store, TARGET_RID));

        publish(
            store,
            descriptor(
                TARGET_RID,
                42,
                2,
                targetOwner,
                ZLinkFrameworkRuntimeState.RELOCATING,
                1,
                1,
                1,
                true),
            ZLinkLocationWriteIntent.RENEW);
        assertInstanceOf(
            ZLinkAuthorityConflict.class,
            store.compareExchange(
                    "relocate-1",
                    new ZLinkAuthorityExpectFound(
                        first.storeVersion()),
                    new ZLinkAuthorityPut(
                        new byte[] {4},
                        ZLinkAuthorityGenerationTransition.NEW_OWNER,
                        Optional.of(targetOwner),
                        Optional.of(capacity.fence())),
                    () -> false)
                .toCompletableFuture().get());
        assertEquals(
            sourceOwner.ownerId(),
            snapshot(store, "relocate-1").ownerId());
        assertEquals(
            actorCapacity(0, 1, 1),
            capacity(store, TARGET_RID));

        publish(
            store,
            descriptor(
                TARGET_RID,
                42,
                3,
                targetOwner,
                ZLinkFrameworkRuntimeState.SERVING,
                1,
                1,
                1,
                true),
            ZLinkLocationWriteIntent.RENEW);
        assertInstanceOf(
            ZLinkAuthorityStored.class,
            store.compareExchange(
                    "relocate-1",
                    new ZLinkAuthorityExpectFound(
                        first.storeVersion()),
                    new ZLinkAuthorityPut(
                        new byte[] {5},
                        ZLinkAuthorityGenerationTransition.NEW_OWNER,
                        Optional.of(targetOwner),
                        Optional.of(capacity.fence())),
                    () -> false)
                .toCompletableFuture().get());
        assertEquals(
            targetOwner.ownerId(),
            snapshot(store, "relocate-1").ownerId());
        assertEquals(
            actorCapacity(1, 0, 1),
            capacity(store, TARGET_RID));
        assertEquals(
            actorCapacity(1, 0, 4),
            capacity(store, SOURCE_RID));
    }

    @Test
    void userSpotCapacityProjectsPopulationAndStableTypeUsage()
        throws Exception {
        ZLinkInMemoryLocationStore store = store();
        ZLinkLocationOwnerToken owner = claim(store, "spot-owner");
        ZLinkMeshNodeDescriptor descriptor = new ZLinkMeshNodeDescriptor(
            "mesh",
            SOURCE_RID,
            51,
            1,
            "tcp://127.0.0.1:7000",
            Map.of(),
            1,
            List.of(new ZLinkObjectCapability(
                ZLinkPlacementObjectKind.USER_SPOT,
                "lobby",
                ZLinkObjectMaintenancePolicyKind.SNAPSHOT,
                true,
                2)),
            ZLinkMeshNodeObjectRole.SERVER,
            Optional.of("mesh-entry-00000000-0000-4000-8000-000000000051"),
            100,
            new ZLinkPlacementCapacity(
                new ZLinkCapacityUsage(0, 0, 0),
                new ZLinkCapacityUsage(0, 0, 3),
                List.of(new ZLinkSpotTypeCapacity(
                    ZLinkPlacementObjectKind.USER_SPOT,
                    "lobby",
                    new ZLinkCapacityUsage(0, 0, 2)))),
            new ZLinkActivationConcurrency(0, 128),
            Optional.empty(),
            ZLinkFrameworkRuntimeState.SERVING,
            "security",
            owner.ownerId(),
            owner.leaseGeneration(),
            NOW);
        publish(store, descriptor, ZLinkLocationWriteIntent.NEW_CLAIM);

        ZLinkObjectReservation reservation = reserve(
            store,
            new ZLinkObjectReservationRequest(
                ZLinkPlacementObjectKind.USER_SPOT,
                "zla1:s:lobby-1",
                "lobby",
                "creation-root",
                new byte[32],
                32,
                descriptorKey(descriptor),
                descriptor.lifecycleGeneration(),
                owner,
                new byte[] {1},
                ZLinkPlacementCapacityBundle.spot(
                    ZLinkPlacementObjectKind.USER_SPOT,
                    "lobby",
                    1)));
        assertEquals(
            new ZLinkPlacementCapacity(
                new ZLinkCapacityUsage(0, 0, 0),
                new ZLinkCapacityUsage(0, 1, 3),
                List.of(new ZLinkSpotTypeCapacity(
                    ZLinkPlacementObjectKind.USER_SPOT,
                    "lobby",
                    new ZLinkCapacityUsage(0, 1, 2)))),
            capacity(store, SOURCE_RID));

        assertEquals(
            ZLinkObjectCommitResult.COMMITTED,
            store.commit(reservation, new byte[] {2}, () -> false)
                .toCompletableFuture().get());
        assertEquals(
            new ZLinkPlacementCapacity(
                new ZLinkCapacityUsage(0, 0, 0),
                new ZLinkCapacityUsage(1, 0, 3),
                List.of(new ZLinkSpotTypeCapacity(
                    ZLinkPlacementObjectKind.USER_SPOT,
                    "lobby",
                    new ZLinkCapacityUsage(1, 0, 2)))),
            capacity(store, SOURCE_RID));
    }

    private static ZLinkInMemoryLocationStore store() {
        return new ZLinkInMemoryLocationStore(
            Clock.fixed(NOW, ZoneOffset.UTC));
    }

    private static ZLinkLocationOwnerToken claim(
        ZLinkInMemoryLocationStore store,
        String ownerId) throws Exception {
        return assertInstanceOf(
            ZLinkOwnerLeaseClaimed.class,
            store.claimOwnerLease(ownerId, Duration.ofMinutes(1))
                .toCompletableFuture().get()).token();
    }

    private static void publish(
        ZLinkInMemoryLocationStore store,
        ZLinkMeshNodeDescriptor descriptor,
        ZLinkLocationWriteIntent intent) throws Exception {
        assertEquals(
            ZLinkLocationWriteStatus.STORED,
            store.updateMeshNode(descriptor, intent)
                .toCompletableFuture().get().status());
    }

    private static ZLinkObjectReservation reserve(
        ZLinkInMemoryLocationStore store,
        ZLinkObjectReservationRequest request) throws Exception {
        return assertInstanceOf(
            ZLinkObjectReserved.class,
            store.reserve(request, () -> false)
                .toCompletableFuture().get()).reservation();
    }

    private static ZLinkAuthoritySnapshot createActive(
        ZLinkInMemoryLocationStore store,
        String authorityKey,
        ZLinkMeshNodeDescriptor descriptor,
        ZLinkLocationOwnerToken owner) throws Exception {
        ZLinkObjectReservation reservation = reserve(
            store,
            creation(
                authorityKey,
                descriptor,
                owner,
                "player"));
        assertEquals(
            ZLinkObjectCommitResult.COMMITTED,
            store.commit(reservation, new byte[] {1}, () -> false)
                .toCompletableFuture().get());
        return snapshot(store, authorityKey);
    }

    private static ZLinkAuthoritySnapshot snapshot(
        ZLinkInMemoryLocationStore store,
        String authorityKey) throws Exception {
        return assertInstanceOf(
            ZLinkAuthoritySnapshot.class,
            store.read(authorityKey, () -> false)
                .toCompletableFuture().get());
    }

    private static ZLinkObjectReservationRequest creation(
        String authorityKey,
        ZLinkMeshNodeDescriptor descriptor,
        ZLinkLocationOwnerToken owner,
        String stableType) {
        return new ZLinkObjectReservationRequest(
            ZLinkPlacementObjectKind.ACTOR,
            authorityKey,
            stableType,
            "creation-root",
            new byte[32],
            32,
            descriptorKey(descriptor),
            descriptor.lifecycleGeneration(),
            owner,
            new byte[] {1},
            ZLinkPlacementCapacityBundle.actor(1));
    }

    private static ZLinkRelocationCapacityReservationRequest relocation(
        String authorityKey,
        ZLinkAuthoritySnapshot current,
        ZLinkMeshNodeDescriptor source,
        ZLinkLocationOwnerToken sourceOwner,
        ZLinkMeshNodeDescriptor target,
        ZLinkLocationOwnerToken targetOwner) {
        return new ZLinkRelocationCapacityReservationRequest(
            UUID.randomUUID(),
            authorityKey,
            current.storeVersion(),
            current.allocation().objectKind(),
            current.allocation().stableType(),
            descriptorKey(source),
            source.lifecycleGeneration(),
            sourceOwner,
            descriptorKey(target),
            target.lifecycleGeneration(),
            targetOwner,
            current.allocation().capacityBundle());
    }

    private static ZLinkMeshNodeDescriptor descriptor(
        RoutingId rid,
        long lifecycle,
        long revision,
        ZLinkLocationOwnerToken owner,
        ZLinkFrameworkRuntimeState state,
        int playerActiveLimit,
        int nodeActiveLimit,
        int nodePendingLimit,
        boolean supportsPlayer) {
        List<ZLinkObjectCapability> capabilities = supportsPlayer
            ? List.of(
                capability(
                    "player",
                    playerActiveLimit,
                    1),
                capability("npc", 4, 4))
            : List.of(capability("npc", 4, 4));
        return new ZLinkMeshNodeDescriptor(
            "mesh",
            rid,
            lifecycle,
            revision,
            "tcp://127.0.0.1:7000",
            Map.of(),
            1,
            capabilities,
            ZLinkMeshNodeObjectRole.SERVER,
            Optional.of("entry-" + rid),
            100,
            actorCapacity(
                0,
                0,
                Math.min(playerActiveLimit, nodeActiveLimit)),
            new ZLinkActivationConcurrency(0, 128),
            Optional.empty(),
            state,
            "security",
            owner.ownerId(),
            owner.leaseGeneration(),
            NOW);
    }

    private static ZLinkObjectCapability capability(
        String stableType,
        int activeLimit,
        int pendingLimit) {
        return new ZLinkObjectCapability(
            ZLinkPlacementObjectKind.ACTOR,
            stableType,
            ZLinkObjectMaintenancePolicyKind.SNAPSHOT,
            true,
            0);
    }

    private static ZLinkPlacementCapacity actorCapacity(
        int active,
        int reserved,
        int limit) {
        return new ZLinkPlacementCapacity(
            new ZLinkCapacityUsage(active, reserved, limit),
            new ZLinkCapacityUsage(0, 0, 0),
            List.of());
    }

    private static ZLinkMeshNodeDescriptorKey descriptorKey(
        ZLinkMeshNodeDescriptor descriptor) {
        return new ZLinkMeshNodeDescriptorKey(
            descriptor.meshName(),
            descriptor.rid());
    }

    private static ZLinkPlacementCapacity capacity(
        ZLinkInMemoryLocationStore store,
        RoutingId rid) throws Exception {
        return store.listMeshNodes(
                "mesh",
                ZLinkPageRequest.firstPage())
            .toCompletableFuture().get().items().stream()
            .filter(descriptor -> descriptor.rid().equals(rid))
            .findFirst()
            .orElseThrow()
            .capacity();
    }
}
