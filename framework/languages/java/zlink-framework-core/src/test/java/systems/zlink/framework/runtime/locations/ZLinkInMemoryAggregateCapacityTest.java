package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.time.Clock;
import java.time.Instant;
import java.time.ZoneOffset;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicBoolean;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;

final class ZLinkInMemoryAggregateCapacityTest {
    private static final Instant NOW =
        Instant.parse("2026-07-23T00:00:00Z");
    private static final ZLinkMeshNodeDescriptorKey SOURCE_DESCRIPTOR =
        new ZLinkMeshNodeDescriptorKey(
            "mesh",
            RoutingId.from("source-node"));
    private static final ZLinkMeshNodeDescriptorKey TARGET_DESCRIPTOR =
        new ZLinkMeshNodeDescriptorKey(
            "mesh",
            RoutingId.from("target-node"));

    @Test
    void aggregateReservesCapacityUntilAggregateAbort() throws Exception {
        Fixture fixture = fixture();
        fixture.sourceLive.set(false);

        ZLinkAggregatePrepareRequest request =
            aggregateRequest(
                UUID.randomUUID(),
                fixture.current,
                fixture.target,
                new byte[] {2});
        var prepared = assertInstanceOf(
            ZLinkAggregatePrepared.class,
            fixture.store.prepareAggregate(request, () -> false)
                .toCompletableFuture().get());
        assertEquals(
            1,
            fixture.store.pendingCapacity(TARGET_DESCRIPTOR, 9));

        assertInstanceOf(
            ZLinkAggregateAlreadyPrepared.class,
            fixture.store.prepareAggregate(
                    aggregateRequest(
                        request.aggregateId(),
                        fixture.current,
                        fixture.target,
                        new byte[] {2}),
                    () -> false)
                .toCompletableFuture().get());
        assertInstanceOf(
            ZLinkAggregateConflict.class,
            fixture.store.prepareAggregate(
                    aggregateRequest(
                        request.aggregateId(),
                        fixture.current,
                        fixture.target,
                        new byte[] {9}),
                    () -> false)
                .toCompletableFuture().get());
        assertInstanceOf(
            ZLinkAggregateConflict.class,
            fixture.store.prepareAggregate(
                    aggregateRequest(
                        UUID.randomUUID(),
                        fixture.current,
                        fixture.target,
                        new byte[] {2}),
                    () -> false)
                .toCompletableFuture().get());

        fixture.targetDescriptorLive.set(false);
        assertEquals(
            ZLinkAggregateCommitResult.STALE,
            fixture.store.commitAggregate(
                    prepared.fence(),
                    () -> false)
                .toCompletableFuture().get());
        assertEquals(
            fixture.current.storeVersion(),
            current(fixture).storeVersion());
        assertEquals(
            1,
            fixture.store.activeCapacity(SOURCE_DESCRIPTOR, 7));
        assertEquals(
            1,
            fixture.store.pendingCapacity(TARGET_DESCRIPTOR, 9));
        assertEquals(
            ZLinkAggregateAbortResult.ABORTED,
            fixture.store.abortAggregate(
                    prepared.fence(),
                    () -> false)
                .toCompletableFuture().get());
        assertEquals(
            ZLinkAggregateAbortResult.ALREADY_ABORTED,
            fixture.store.abortAggregate(
                    prepared.fence(),
                    () -> false)
                .toCompletableFuture().get());
        assertEquals(
            fixture.current.storeVersion(),
            current(fixture).storeVersion());
        assertEquals(
            0,
            fixture.store.pendingCapacity(TARGET_DESCRIPTOR, 9));
    }

    @Test
    void aggregateRejectsDuplicateParticipantBeforeBindingCapacity()
        throws Exception {
        Fixture fixture = fixture();
        ZLinkAggregateParticipant participant =
            new ZLinkAggregateParticipant(
                fixture.key,
                fixture.current.objectGeneration(),
                fixture.current.authorityOwnerGeneration(),
                fixture.current.storeVersion(),
                ZLinkAuthorityGenerationTransition.PRESERVE,
                new byte[] {2},
                new byte[] {3});
        assertThrows(
            IllegalArgumentException.class,
            () -> new ZLinkAggregatePrepareRequest(
                UUID.randomUUID(),
                1,
                List.of(participant, participant),
                new byte[32],
                TARGET_DESCRIPTOR,
                9,
                ZLinkPlacementCapacityBundle.actor(1),
                fixture.target));
        assertEquals(
            fixture.current.storeVersion(),
            current(fixture).storeVersion());
    }

    @Test
    void aggregateCommitConsumesReservedBundleAndReplacesActiveAllocation()
        throws Exception {
        Fixture fixture = fixture();
        fixture.sourceLive.set(false);
        var prepared = assertInstanceOf(
            ZLinkAggregatePrepared.class,
            fixture.store.prepareAggregate(
                    aggregateRequest(
                        UUID.randomUUID(),
                        fixture.current,
                        fixture.target,
                        new byte[] {2}),
                    () -> false)
                .toCompletableFuture().get());

        assertEquals(
            ZLinkAggregateCommitResult.COMMITTED,
            fixture.store.commitAggregate(
                    prepared.fence(),
                    () -> false)
                .toCompletableFuture().get());
        assertEquals(
            ZLinkAggregateCommitResult.ALREADY_COMMITTED,
            fixture.store.commitAggregate(
                    prepared.fence(),
                    () -> false)
                .toCompletableFuture().get());

        ZLinkAuthoritySnapshot current = current(fixture);
        assertEquals(fixture.target.ownerId(), current.ownerId());
        assertEquals(
            fixture.target.leaseGeneration(),
            current.ownerLeaseGeneration());
        assertEquals(
            fixture.current.objectGeneration(),
            current.objectGeneration());
        assertEquals(
            fixture.current.authorityOwnerGeneration() + 1,
            current.authorityOwnerGeneration());
        assertEquals(
            ZLinkPlacementAllocationState.ACTIVE,
            current.allocation().state());
        assertEquals(
            TARGET_DESCRIPTOR,
            current.allocation().descriptor());
        assertEquals(
            fixture.capacityRequest
                .targetDescriptorLifecycleGeneration(),
            current.allocation().descriptorLifecycleGeneration());
        assertArrayEquals(new byte[] {2}, current.payload());
        assertArrayEquals(
            new byte[] {3},
            fixture.store.membershipMutation(fixture.key));
        assertEquals(
            0,
            fixture.store.activeCapacity(SOURCE_DESCRIPTOR, 7));
        assertEquals(
            1,
            fixture.store.activeCapacity(TARGET_DESCRIPTOR, 9));
        assertEquals(
            0,
            fixture.store.pendingCapacity(TARGET_DESCRIPTOR, 9));
    }

    @Test
    void aggregateCommitRejectsExpiredExactTargetOwnerWithoutMutation()
        throws Exception {
        Fixture fixture = fixture();
        fixture.sourceLive.set(false);
        var prepared = assertInstanceOf(
            ZLinkAggregatePrepared.class,
            fixture.store.prepareAggregate(
                    aggregateRequest(
                        UUID.randomUUID(),
                        fixture.current,
                        fixture.target,
                        new byte[] {2}),
                    () -> false)
                .toCompletableFuture().get());
        fixture.targetLive.set(false);

        assertEquals(
            ZLinkAggregateCommitResult.STALE,
            fixture.store.commitAggregate(prepared.fence(), () -> false)
                .toCompletableFuture().get());
        ZLinkAuthoritySnapshot current = current(fixture);
        assertEquals(fixture.source.ownerId(), current.ownerId());
        assertEquals(
            fixture.current.storeVersion(),
            current.storeVersion());
        assertEquals(
            1,
            fixture.store.activeCapacity(SOURCE_DESCRIPTOR, 7));
        assertEquals(
            1,
            fixture.store.pendingCapacity(TARGET_DESCRIPTOR, 9));

        assertEquals(
            ZLinkAggregateAbortResult.ABORTED,
            fixture.store.abortAggregate(prepared.fence(), () -> false)
                .toCompletableFuture().get());
        assertEquals(
            0,
            fixture.store.pendingCapacity(TARGET_DESCRIPTOR, 9));
    }

    @Test
    void nextGenerationPreserveAggregateChangesOnlyPayload() throws Exception {
        Fixture fixture = fixture();
        fixture.sourceLive.set(false);
        UUID aggregateId = UUID.randomUUID();
        var activated = assertInstanceOf(
            ZLinkAggregatePrepared.class,
            fixture.store.prepareAggregate(
                    aggregateRequest(
                        aggregateId,
                        fixture.current,
                        fixture.target,
                        new byte[] {2}),
                    () -> false)
                .toCompletableFuture().get());
        assertEquals(
            ZLinkAggregateCommitResult.COMMITTED,
            fixture.store.commitAggregate(activated.fence(), () -> false)
                .toCompletableFuture().get());
        ZLinkAuthoritySnapshot before = current(fixture);

        var completed = assertInstanceOf(
            ZLinkAggregatePrepared.class,
            fixture.store.prepareAggregate(
                    new ZLinkAggregatePrepareRequest(
                        aggregateId,
                        2,
                        List.of(new ZLinkAggregateParticipant(
                            fixture.key,
                            before.objectGeneration(),
                            before.authorityOwnerGeneration(),
                            before.storeVersion(),
                            ZLinkAuthorityGenerationTransition.PRESERVE,
                            new byte[] {8},
                            new byte[0])),
                        new byte[32],
                        TARGET_DESCRIPTOR,
                        9,
                        new ZLinkPlacementCapacityBundle(
                            0, 0, Optional.empty()),
                        fixture.target),
                    () -> false)
                .toCompletableFuture().get());
        assertEquals(
            ZLinkAggregateCommitResult.COMMITTED,
            fixture.store.commitAggregate(completed.fence(), () -> false)
                .toCompletableFuture().get());

        ZLinkAuthoritySnapshot after = current(fixture);
        assertArrayEquals(new byte[] {8}, after.payload());
        assertEquals(before.ownerId(), after.ownerId());
        assertEquals(before.objectGeneration(), after.objectGeneration());
        assertEquals(
            before.authorityOwnerGeneration(),
            after.authorityOwnerGeneration());
        assertEquals(before.allocation(), after.allocation());
        assertEquals(1, fixture.store.activeCapacity(TARGET_DESCRIPTOR, 9));
        assertEquals(0, fixture.store.pendingCapacity(TARGET_DESCRIPTOR, 9));
    }

    @Test
    void deleteRequiresLiveCurrentOwnerAndRemovesActiveDelta()
        throws Exception {
        Fixture fixture = fixture();
        fixture.sourceLive.set(false);
        assertInstanceOf(
            ZLinkAuthorityConflict.class,
            fixture.store.compareExchange(
                    fixture.key,
                    new ZLinkAuthorityExpectFound(
                        fixture.current.storeVersion()),
                    new ZLinkAuthorityDelete(),
                    () -> false)
                .toCompletableFuture().get());
        assertEquals(
            1,
            fixture.store.activeCapacity(SOURCE_DESCRIPTOR, 7));

        fixture.sourceLive.set(true);
        assertInstanceOf(
            ZLinkAuthorityDeleted.class,
            fixture.store.compareExchange(
                    fixture.key,
                    new ZLinkAuthorityExpectFound(
                        fixture.current.storeVersion()),
                    new ZLinkAuthorityDelete(),
                    () -> false)
                .toCompletableFuture().get());
        assertEquals(
            0,
            fixture.store.activeCapacity(SOURCE_DESCRIPTOR, 7));
    }

    private static Fixture fixture() throws Exception {
        AtomicBoolean sourceLive = new AtomicBoolean(true);
        AtomicBoolean targetLive = new AtomicBoolean(true);
        AtomicBoolean targetDescriptorLive = new AtomicBoolean(true);
        ZLinkLocationOwnerToken source =
            new ZLinkLocationOwnerToken("source", 1);
        ZLinkLocationOwnerToken target =
            new ZLinkLocationOwnerToken("target", 2);
        ZLinkInMemoryAuthorityStore store =
            new ZLinkInMemoryAuthorityStore(
                Clock.fixed(NOW, ZoneOffset.UTC),
                owner -> owner.equals(source)
                    ? sourceLive.get()
                    : !owner.equals(target) || targetLive.get(),
                (descriptor, generation, owner) ->
                    descriptor.equals(TARGET_DESCRIPTOR)
                        && !targetDescriptorLive.get()
                            ? null
                            : descriptor(
                                descriptor,
                                generation,
                                owner));
        String key = "zla1:a:4:mesh:7:actor-1";
        ZLinkAuthoritySnapshot current =
            createActive(store, key, source);
        ZLinkRelocationCapacityReservationRequest capacityRequest =
            new ZLinkRelocationCapacityReservationRequest(
                UUID.randomUUID(),
                key,
                current.storeVersion(),
                current.allocation().objectKind(),
                current.allocation().stableType(),
                current.allocation().descriptor(),
                current.allocation().descriptorLifecycleGeneration(),
                source,
                TARGET_DESCRIPTOR,
                9,
                target,
                current.allocation().capacityBundle());
        return new Fixture(
            store,
            key,
            source,
            target,
            current,
            capacityRequest,
            sourceLive,
            targetLive,
            targetDescriptorLive);
    }

    private static ZLinkAuthoritySnapshot createActive(
        ZLinkInMemoryAuthorityStore store,
        String key,
        ZLinkLocationOwnerToken owner) throws Exception {
        var reserved = assertInstanceOf(
            ZLinkObjectReserved.class,
            store.reserve(
                    new ZLinkObjectReservationRequest(
                        ZLinkPlacementObjectKind.ACTOR,
                        key,
                        "player",
                        "creation-root",
                        new byte[32],
                        32,
                        SOURCE_DESCRIPTOR,
                        7,
                        owner,
                        new byte[] {1},
                        ZLinkPlacementCapacityBundle.actor(1)),
                    () -> false)
                .toCompletableFuture().get()).reservation();
        assertEquals(
            ZLinkPlacementAllocationState.PENDING,
            assertInstanceOf(
                ZLinkAuthoritySnapshot.class,
                store.read(key, () -> false)
                    .toCompletableFuture().get())
                .allocation().state());
        assertEquals(
            ZLinkObjectCommitResult.COMMITTED,
            store.commit(reserved, new byte[] {1}, () -> false)
                .toCompletableFuture().get());
        assertEquals(
            0,
            store.pendingCapacity(SOURCE_DESCRIPTOR, 7));
        assertEquals(
            1,
            store.activeCapacity(SOURCE_DESCRIPTOR, 7));
        return assertInstanceOf(
            ZLinkAuthoritySnapshot.class,
            store.read(key, () -> false).toCompletableFuture().get());
    }

    private static ZLinkAggregatePrepareRequest aggregateRequest(
        UUID aggregateId,
        ZLinkAuthoritySnapshot current,
        ZLinkLocationOwnerToken target,
        byte[] payload) {
        return new ZLinkAggregatePrepareRequest(
            aggregateId,
            1,
            List.of(new ZLinkAggregateParticipant(
                "zla1:a:4:mesh:7:actor-1",
                current.objectGeneration(),
                current.authorityOwnerGeneration(),
                current.storeVersion(),
                ZLinkAuthorityGenerationTransition.NEW_OWNER,
                payload,
                new byte[] {3})),
            new byte[32],
            TARGET_DESCRIPTOR,
            9,
            ZLinkPlacementCapacityBundle.actor(1),
            target);
    }

    private static ZLinkMeshNodeDescriptor descriptor(
        ZLinkMeshNodeDescriptorKey key,
        long lifecycleGeneration,
        ZLinkLocationOwnerToken owner) {
        return new ZLinkMeshNodeDescriptor(
            key.meshName(),
            key.rid(),
            lifecycleGeneration,
            1,
            "tcp://127.0.0.1:7000",
            Map.of(),
            1,
            List.of(new ZLinkObjectCapability(
                ZLinkPlacementObjectKind.ACTOR,
                "player",
                ZLinkObjectMaintenancePolicyKind.SNAPSHOT,
                true,
                0)),
            ZLinkMeshNodeObjectRole.SERVER,
            Optional.of("entry-" + key.rid()),
            100,
            new ZLinkPlacementCapacity(
                new ZLinkCapacityUsage(0, 0, 64),
                new ZLinkCapacityUsage(0, 0, 0),
                List.of()),
            new ZLinkActivationConcurrency(0, 128),
            Optional.empty(),
            systems.zlink.framework.runtime.host
                .ZLinkFrameworkRuntimeState.SERVING,
            "security",
            owner.ownerId(),
            owner.leaseGeneration(),
            NOW);
    }

    private static ZLinkAuthoritySnapshot current(Fixture fixture)
        throws Exception {
        return assertInstanceOf(
            ZLinkAuthoritySnapshot.class,
            fixture.store.read(fixture.key, () -> false)
                .toCompletableFuture().get());
    }

    private record Fixture(
        ZLinkInMemoryAuthorityStore store,
        String key,
        ZLinkLocationOwnerToken source,
        ZLinkLocationOwnerToken target,
        ZLinkAuthoritySnapshot current,
        ZLinkRelocationCapacityReservationRequest capacityRequest,
        AtomicBoolean sourceLive,
        AtomicBoolean targetLive,
        AtomicBoolean targetDescriptorLive) {
    }
}
