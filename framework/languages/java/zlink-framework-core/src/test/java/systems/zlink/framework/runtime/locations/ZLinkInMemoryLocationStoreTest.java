package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertArrayEquals;

import java.time.Clock;
import java.time.Duration;
import java.time.Instant;
import java.time.ZoneOffset;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityConflict;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityDelete;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityExpectFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityGenerationTransition;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityPut;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityRestore;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityStored;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.runtime.internal.locations.ZLinkFanoutPublisherDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkFanoutPublisherDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.locations.ZLinkLocationRole;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteIntent;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteStatus;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkObjectCapability;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectCommitResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkCreationOperationIdentity;
import systems.zlink.framework.runtime.internal.locations.ZLinkCreationOperationTerminal;
import systems.zlink.framework.runtime.internal.locations.ZLinkCreationTerminalFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkCreationTerminalMissing;
import systems.zlink.framework.runtime.internal.locations.ZLinkCreationTerminalState;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectRejectResult;
import systems.zlink.framework.locations.ZLinkObjectMaintenancePolicyKind;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectReservation;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectReservationRequest;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectReserved;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimConflict;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimed;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseReleaseResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseRenewStale;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseRenewed;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementCapacityBundle;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationCapacityFence;

class ZLinkInMemoryLocationStoreTest {
    private static final Instant NOW = Instant.parse("2026-07-03T00:00:00Z");
    private static final RoutingId NODE_A = RoutingId.from(new byte[] {0x01});

    @Test
    void creationTerminalsAreScopedToExactOperationAndRejectionReopensReservation()
        throws Exception {
        ZLinkInMemoryAuthorityStore store = newAuthorityStore();
        ZLinkLocationOwnerToken owner =
            new ZLinkLocationOwnerToken("owner", 1);
        ZLinkCreationOperationIdentity firstOperation =
            new ZLinkCreationOperationIdentity(NODE_A, 7, 1, 2);
        ZLinkCreationOperationIdentity secondOperation =
            new ZLinkCreationOperationIdentity(NODE_A, 7, 1, 3);

        ZLinkObjectReservation first = reserveActor(
            store,
            "zla1:a:4:mesh:8:actor-a",
            owner);
        byte[] createdEnvelope = new byte[] {1, 2, 3};
        ZLinkCreationOperationTerminal created = terminal(
            firstOperation,
            first,
            ZLinkCreationTerminalState.CREATED,
            createdEnvelope);
        assertEquals(
            ZLinkObjectCommitResult.COMMITTED,
            store.commit(first, new byte[] {9}, created, () -> false)
                .toCompletableFuture().get());
        assertEquals(
            ZLinkObjectCommitResult.ALREADY_COMMITTED,
            store.commit(first, new byte[] {9}, created, () -> false)
                .toCompletableFuture().get());
        assertEquals(
            ZLinkObjectCommitResult.STALE,
            store.commit(
                    first,
                    new byte[] {9},
                    terminal(
                        firstOperation,
                        first,
                        ZLinkCreationTerminalState.CREATED,
                        new byte[] {1, 2, 4}),
                    () -> false)
                .toCompletableFuture().get());
        assertArrayEquals(
            createdEnvelope,
            assertInstanceOf(
                ZLinkCreationTerminalFound.class,
                store.readCreationTerminal(firstOperation, () -> false)
                    .toCompletableFuture().get())
                .terminal()
                .terminalEnvelope());
        assertInstanceOf(
            ZLinkCreationTerminalMissing.class,
            store.readCreationTerminal(secondOperation, () -> false)
                .toCompletableFuture().get());

        ZLinkObjectReservation rejectedReservation = reserveActor(
            store,
            "zla1:a:4:mesh:8:actor-b",
            owner);
        ZLinkCreationOperationTerminal rejected = terminal(
            secondOperation,
            rejectedReservation,
            ZLinkCreationTerminalState.REJECTED,
            new byte[] {4});
        assertEquals(
            ZLinkObjectRejectResult.REJECTED,
            store.reject(
                    rejectedReservation,
                    rejected,
                    () -> false)
                .toCompletableFuture().get());
        assertInstanceOf(
            ZLinkObjectReserved.class,
            store.reserve(
                    reservationRequest(
                        "zla1:a:4:mesh:8:actor-b",
                        owner),
                    () -> false)
                .toCompletableFuture().get());
    }

    @Test
    void fanoutPublisherUsesDedicatedLifecycleAndOwnerFences()
        throws Exception {
        ZLinkInMemoryLocationStore store = new ZLinkInMemoryLocationStore(
            Clock.fixed(NOW, ZoneOffset.UTC));
        ZLinkLocationOwnerToken owner = assertInstanceOf(
            ZLinkOwnerLeaseClaimed.class,
            store.claimOwnerLease("fanout-owner", Duration.ofSeconds(30))
                .toCompletableFuture().get()).token();
        ZLinkFanoutPublisherDescriptor initial =
            fanoutPublisher(owner, 7, 1, "tcp://127.0.0.1:7400");

        assertEquals(
            ZLinkLocationWriteStatus.STORED,
            store.updateFanoutPublisher(
                    initial, ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture().get().status());
        assertEquals(
            ZLinkLocationWriteStatus.IGNORED_STALE,
            store.updateFanoutPublisher(
                    fanoutPublisher(
                        owner, 7, 1, "tcp://127.0.0.1:7401"),
                    ZLinkLocationWriteIntent.RENEW)
                .toCompletableFuture().get().status());
        ZLinkFanoutPublisherDescriptor renewed =
            fanoutPublisher(owner, 7, 2, initial.endpoint());
        assertEquals(
            ZLinkLocationWriteStatus.STORED,
            store.updateFanoutPublisher(
                    renewed, ZLinkLocationWriteIntent.RENEW)
                .toCompletableFuture().get().status());
        assertEquals(
            List.of(renewed),
            store.listFanoutPublishers(
                    "events",
                    systems.zlink.framework.locations.ZLinkPageRequest
                        .firstPage())
                .toCompletableFuture().get().items());
        assertEquals(
            ZLinkLocationWriteStatus.STORED,
            store.removeFanoutPublisher(
                    new ZLinkFanoutPublisherDescriptorKey(
                        "events", NODE_A),
                    owner)
                .toCompletableFuture().get());
    }

    @Test
    void ownerLeaseUsesExactTokenForReadRenewAndRelease() throws Exception {
        ZLinkInMemoryLocationStore store = new ZLinkInMemoryLocationStore(
            Clock.fixed(NOW, ZoneOffset.UTC));
        var claimed = assertInstanceOf(
            ZLinkOwnerLeaseClaimed.class,
            store.claimOwnerLease("owner-a", Duration.ofSeconds(30))
                .toCompletableFuture().get());
        assertInstanceOf(
            ZLinkOwnerLeaseClaimConflict.class,
            store.claimOwnerLease("owner-a", Duration.ofSeconds(30))
                .toCompletableFuture().get());
        assertEquals(
            claimed.token(),
            assertInstanceOf(
                ZLinkOwnerLeaseFound.class,
                store.readOwnerLease("owner-a")
                    .toCompletableFuture().get()).token());
        assertInstanceOf(
            ZLinkOwnerLeaseRenewStale.class,
            store.renewOwnerLease(
                    new ZLinkLocationOwnerToken("owner-a", 99),
                    Duration.ofSeconds(30))
                .toCompletableFuture().get());
        assertInstanceOf(
            ZLinkOwnerLeaseRenewed.class,
            store.renewOwnerLease(
                    claimed.token(),
                    Duration.ofSeconds(30))
                .toCompletableFuture().get());
        assertEquals(
            ZLinkOwnerLeaseReleaseResult.STALE,
            store.releaseOwnerLease(
                    new ZLinkLocationOwnerToken("owner-a", 99))
                .toCompletableFuture().get());
        assertEquals(
            ZLinkOwnerLeaseReleaseResult.RELEASED,
            store.releaseOwnerLease(claimed.token())
                .toCompletableFuture().get());
        var reclaimed = assertInstanceOf(
            ZLinkOwnerLeaseClaimed.class,
            store.claimOwnerLease("owner-a", Duration.ofSeconds(30))
                .toCompletableFuture().get());
        assertEquals(
            claimed.token().leaseGeneration() + 1,
            reclaimed.token().leaseGeneration());
    }

    @Test
    void meshNodeRenewFencesImmutableFieldsAndProjectsAuthorityCapacity()
        throws Exception {
        ZLinkInMemoryLocationStore store = new ZLinkInMemoryLocationStore(
            Clock.fixed(NOW, ZoneOffset.UTC));
        var owner = assertInstanceOf(
            ZLinkOwnerLeaseClaimed.class,
            store.claimOwnerLease("mesh-owner", Duration.ofSeconds(30))
                .toCompletableFuture().get()).token();
        ZLinkMeshNodeDescriptor initial = meshNodeDescriptor(
            owner,
            1,
            "tcp://127.0.0.1:7000");
        assertEquals(
            ZLinkLocationWriteStatus.STORED,
            store.updateMeshNode(
                    initial,
                    ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture().get().status());

        assertEquals(
            ZLinkLocationWriteStatus.IGNORED_STALE,
            store.updateMeshNode(
                    meshNodeDescriptor(
                        owner,
                        2,
                        "tcp://127.0.0.1:7001"),
                    ZLinkLocationWriteIntent.RENEW)
                .toCompletableFuture().get().status());

        String key = "zla1:a:4:mesh:9:projected";
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
                        new ZLinkMeshNodeDescriptorKey("mesh", NODE_A),
                        7,
                        owner,
                        new byte[] {1},
                        ZLinkPlacementCapacityBundle.actor(1)),
                    () -> false)
                .toCompletableFuture().get()).reservation();
        assertEquals(
            actorCapacity(0, 1, 8),
            onlyMeshNode(store).capacity());

        assertEquals(
            ZLinkObjectCommitResult.COMMITTED,
            store.commit(reserved, new byte[] {2}, () -> false)
                .toCompletableFuture().get());
        assertEquals(
            actorCapacity(1, 0, 8),
            onlyMeshNode(store).capacity());

        var active = assertInstanceOf(
            ZLinkAuthoritySnapshot.class,
            store.read(key, () -> false).toCompletableFuture().get());
        assertInstanceOf(
            systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityDeleted.class,
            store.compareExchange(
                    key,
                    new ZLinkAuthorityExpectFound(active.storeVersion()),
                    new ZLinkAuthorityDelete(),
                    () -> false)
                .toCompletableFuture().get());
        assertEquals(
            actorCapacity(0, 0, 8),
            onlyMeshNode(store).capacity());
    }

    @Test
    void entrySpotIdentityIsClaimedGloballyAndReleasedByExactOwner()
        throws Exception {
        ZLinkInMemoryLocationStore store = new ZLinkInMemoryLocationStore(
            Clock.fixed(NOW, ZoneOffset.UTC));
        ZLinkLocationOwnerToken firstOwner = assertInstanceOf(
            ZLinkOwnerLeaseClaimed.class,
            store.claimOwnerLease("entry-owner-a", Duration.ofSeconds(30))
                .toCompletableFuture().get()).token();
        ZLinkLocationOwnerToken secondOwner = assertInstanceOf(
            ZLinkOwnerLeaseClaimed.class,
            store.claimOwnerLease("entry-owner-b", Duration.ofSeconds(30))
                .toCompletableFuture().get()).token();
        String entrySpotId =
            "mesh-entry-00000000-0000-4000-8000-000000000099";
        ZLinkMeshNodeDescriptor first = meshNodeDescriptor(
            firstOwner, NODE_A, 7, 1,
            "tcp://127.0.0.1:7000", entrySpotId);
        RoutingId secondRid = RoutingId.from(new byte[] {0x02});
        ZLinkMeshNodeDescriptor second = meshNodeDescriptor(
            secondOwner, secondRid, 8, 1,
            "tcp://127.0.0.1:7001", entrySpotId);

        assertEquals(
            ZLinkLocationWriteStatus.STORED,
            store.updateMeshNode(first, ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture().get().status());
        assertEquals(
            ZLinkLocationWriteStatus.REJECTED_CONFLICT,
            store.updateMeshNode(second, ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture().get().status());
        assertEquals(
            ZLinkLocationWriteStatus.STORED,
            store.removeMeshNode(
                    new ZLinkMeshNodeDescriptorKey("mesh", NODE_A),
                    firstOwner)
                .toCompletableFuture().get());
        assertEquals(
            ZLinkLocationWriteStatus.STORED,
            store.updateMeshNode(second, ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture().get().status());
        assertEquals(
            ZLinkLocationWriteStatus.IGNORED_STALE,
            store.removeMeshNode(
                    new ZLinkMeshNodeDescriptorKey("mesh", NODE_A),
                    firstOwner)
                .toCompletableFuture().get());
    }

    @Test
    void newOwnerRejectsAnUnreservedCapacityFenceWithoutMutation()
        throws Exception {
        ZLinkInMemoryAuthorityStore store = newAuthorityStore();
        var source = new ZLinkLocationOwnerToken("source", 1);
        var target = new ZLinkLocationOwnerToken("target", 2);
        String key = "zla1:a:4:mesh:4:test";
        var created = createActive(store, key, source);

        assertInstanceOf(
            ZLinkAuthorityConflict.class,
            store.compareExchange(
                    key,
                    new ZLinkAuthorityExpectFound(created.storeVersion()),
                    new ZLinkAuthorityPut(
                        new byte[] {2},
                        ZLinkAuthorityGenerationTransition.NEW_OWNER,
                        Optional.of(target),
                        Optional.of(
                            new ZLinkRelocationCapacityFence("missing"))),
                    () -> false)
                .toCompletableFuture().get());

        var current = assertInstanceOf(
            ZLinkAuthoritySnapshot.class,
            store.read(key, () -> false).toCompletableFuture().get());
        assertEquals(created.storeVersion(), current.storeVersion());
        assertEquals(source.ownerId(), current.ownerId());
        assertEquals(
            source.leaseGeneration(),
            current.ownerLeaseGeneration());
    }

    @Test
    void preservePutCanSealAnActiveAuthorityWithoutChangingItsOwnerFence()
        throws Exception {
        ZLinkInMemoryAuthorityStore store = newAuthorityStore();
        var owner = new ZLinkLocationOwnerToken("source", 1);
        String key = "zla1:a:4:mesh:7:closing";
        var active = createActive(store, key, owner);

        var closing = assertInstanceOf(
            ZLinkAuthorityStored.class,
            store.compareExchange(
                    key,
                    new ZLinkAuthorityExpectFound(active.storeVersion()),
                    new ZLinkAuthorityPut(
                        new byte[] {2},
                        ZLinkAuthorityGenerationTransition.PRESERVE,
                        Optional.empty(),
                        Optional.empty()),
                    () -> false)
                .toCompletableFuture().get());

        assertEquals(active.objectGeneration(), closing.objectGeneration());
        assertEquals(
            active.authorityOwnerGeneration(),
            closing.authorityOwnerGeneration());
        assertEquals(active.ownerId(), closing.ownerId());
        assertEquals(
            active.ownerLeaseGeneration(),
            closing.ownerLeaseGeneration());
        assertArrayEquals(new byte[] {2}, closing.payload());
        assertInstanceOf(
            ZLinkAuthorityConflict.class,
            store.compareExchange(
                    key,
                    new ZLinkAuthorityExpectFound(active.storeVersion()),
                    new ZLinkAuthorityDelete(),
                    () -> false)
                .toCompletableFuture().get());
    }

    @Test
    void concurrentCrossNodeSealCasHasOneWinnerAndLoserCannotDeleteIt()
        throws Exception {
        ZLinkInMemoryAuthorityStore store = newAuthorityStore();
        var owner = new ZLinkLocationOwnerToken("source", 1);
        String key = "zla1:a:4:mesh:12:idle-race";
        var ready = createActive(store, key, owner);
        var expected = new ZLinkAuthorityExpectFound(ready.storeVersion());

        CompletableFuture<systems.zlink.framework.runtime.internal.locations
            .ZLinkAuthorityWriteResult> first = CompletableFuture.supplyAsync(
                () -> store.compareExchange(
                        key,
                        expected,
                        new ZLinkAuthorityPut(
                            new byte[] {2},
                            ZLinkAuthorityGenerationTransition.PRESERVE,
                            Optional.empty(),
                            Optional.empty()),
                        () -> false)
                    .toCompletableFuture()
                    .join());
        CompletableFuture<systems.zlink.framework.runtime.internal.locations
            .ZLinkAuthorityWriteResult> second = CompletableFuture.supplyAsync(
                () -> store.compareExchange(
                        key,
                        expected,
                        new ZLinkAuthorityPut(
                            new byte[] {3},
                            ZLinkAuthorityGenerationTransition.PRESERVE,
                            Optional.empty(),
                            Optional.empty()),
                        () -> false)
                    .toCompletableFuture()
                    .join());

        var results = List.of(first.get(), second.get());
        assertEquals(
            1,
            results.stream()
                .filter(ZLinkAuthorityStored.class::isInstance)
                .count());
        assertEquals(
            1,
            results.stream()
                .filter(systems.zlink.framework.runtime.internal.locations
                    .ZLinkAuthorityConflict.class::isInstance)
                .count());
        var winner = results.stream()
            .filter(ZLinkAuthorityStored.class::isInstance)
            .map(ZLinkAuthorityStored.class::cast)
            .findFirst()
            .orElseThrow();

        // A losing idle-eviction worker still holds the READY version. It
        // must not remove the CLOSING row installed by the winner.
        assertInstanceOf(
            systems.zlink.framework.runtime.internal.locations
                .ZLinkAuthorityConflict.class,
            store.compareExchange(
                    key,
                    expected,
                    new ZLinkAuthorityDelete(),
                    () -> false)
                .toCompletableFuture().get());
        var current = assertInstanceOf(
            ZLinkAuthoritySnapshot.class,
            store.read(key, () -> false).toCompletableFuture().get());
        assertEquals(winner.storeVersion(), current.storeVersion());
        assertEquals(owner.ownerId(), current.ownerId());
        assertEquals(owner.leaseGeneration(), current.ownerLeaseGeneration());
    }

    @Test
    void restoreRequiresExactOwnerButDoesNotRequireALiveLease()
        throws Exception {
        var leaseLive = new java.util.concurrent.atomic.AtomicBoolean(true);
        ZLinkInMemoryAuthorityStore store = new ZLinkInMemoryAuthorityStore(
            Clock.fixed(NOW, ZoneOffset.UTC),
            ignored -> leaseLive.get(),
            (descriptor, generation, owner) -> meshNodeDescriptor(
                owner,
                1,
                "tcp://127.0.0.1:7000"));
        var owner = new ZLinkLocationOwnerToken("source", 1);
        String key = "zla1:a:4:mesh:7:restore";
        var created = createActive(store, key, owner);
        leaseLive.set(false);

        var restored = assertInstanceOf(
            ZLinkAuthorityStored.class,
            store.compareExchange(
                    key,
                    new ZLinkAuthorityExpectFound(created.storeVersion()),
                    new ZLinkAuthorityRestore(new byte[] {9}, owner),
                    () -> false)
                .toCompletableFuture().get());

        assertArrayEquals(new byte[] {9}, restored.payload());
        assertEquals(created.objectGeneration(), restored.objectGeneration());
        assertEquals(
            created.authorityOwnerGeneration(),
            restored.authorityOwnerGeneration());
        assertEquals(owner.ownerId(), restored.ownerId());
        assertInstanceOf(
            ZLinkAuthorityConflict.class,
            store.compareExchange(
                    key,
                    new ZLinkAuthorityExpectFound(restored.storeVersion()),
                    new ZLinkAuthorityRestore(
                        new byte[] {8},
                        new ZLinkLocationOwnerToken("other", 1)),
                    () -> false)
                .toCompletableFuture().get());
    }

    @Test
    void meshNodeChangeStampTracksOnlyStoredMutations() throws Exception {
        ZLinkInMemoryLocationStore store = new ZLinkInMemoryLocationStore(
            Clock.fixed(NOW, ZoneOffset.UTC));
        var owner = assertInstanceOf(
            ZLinkOwnerLeaseClaimed.class,
            store.claimOwnerLease("stamp-owner", Duration.ofSeconds(30))
                .toCompletableFuture().get()).token();
        var descriptor = meshNodeDescriptor(
            owner,
            NODE_A,
            7,
            1,
            "tcp://127.0.0.1:7000",
            "mesh-entry-00000000-0000-4000-8000-000000000077");

        assertEquals(
            java.util.OptionalLong.empty(),
            store.getMeshNodeChangeStamp("mesh")
                .toCompletableFuture().get());
        store.updateMeshNode(descriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
            .toCompletableFuture().get();
        assertEquals(
            java.util.OptionalLong.of(1),
            store.getMeshNodeChangeStamp("mesh")
                .toCompletableFuture().get());
        store.removeMeshNode(
                new ZLinkMeshNodeDescriptorKey("mesh", NODE_A), owner)
            .toCompletableFuture().get();
        assertEquals(
            java.util.OptionalLong.of(2),
            store.getMeshNodeChangeStamp("mesh")
                .toCompletableFuture().get());
    }

    @Test
    void creationReservationFencesTargetDescriptorLifecycleAndOwner()
        throws Exception {
        ZLinkInMemoryAuthorityStore store = newAuthorityStore();
        var owner = new ZLinkLocationOwnerToken("target", 1);
        var request = new ZLinkObjectReservationRequest(
            ZLinkPlacementObjectKind.ACTOR,
            "zla1:a:4:mesh:7:created",
            "player",
            "creation-root",
            new byte[32],
            32,
            new ZLinkMeshNodeDescriptorKey("mesh", NODE_A),
            7,
            owner,
            new byte[] {1},
            ZLinkPlacementCapacityBundle.actor(1));
        var reserved = assertInstanceOf(
            ZLinkObjectReserved.class,
            store.reserve(request, () -> false)
                .toCompletableFuture().get()).reservation();
        var wrongLifecycle = new ZLinkObjectReservation(
            reserved.authorityKey(),
            reserved.storeVersion(),
            reserved.objectGeneration(),
            reserved.authorityOwnerGeneration(),
            reserved.reservationVersion(),
            reserved.targetDescriptor(),
            reserved.targetDescriptorLifecycleGeneration() + 1,
            reserved.targetOwner());

        assertEquals(
            ZLinkObjectCommitResult.STALE,
            store.commit(
                    wrongLifecycle,
                    new byte[] {2},
                    () -> false)
                .toCompletableFuture().get());
        assertArrayEquals(
            new byte[] {1},
            assertInstanceOf(
                ZLinkAuthoritySnapshot.class,
                store.read(request.authorityKey(), () -> false)
                    .toCompletableFuture().get()).payload());
        assertEquals(
            ZLinkObjectCommitResult.COMMITTED,
            store.commit(
                    reserved,
                    new byte[] {2},
                    () -> false)
                .toCompletableFuture().get());
        assertArrayEquals(
            new byte[] {2},
            assertInstanceOf(
                ZLinkAuthoritySnapshot.class,
                store.read(request.authorityKey(), () -> false)
                    .toCompletableFuture().get()).payload());
    }

    @Test
    void newClaimRejectsWhenExistingOwnerLeaseIsLive() throws Exception {
        ZLinkInMemoryLocationStore store = new ZLinkInMemoryLocationStore(Clock.fixed(NOW, ZoneOffset.UTC));
        var ownerA = assertInstanceOf(
            ZLinkOwnerLeaseClaimed.class,
            store.claimOwnerLease("owner-a", Duration.ofSeconds(30))
                .toCompletableFuture().get()).token();
        var first = store.updateMeshNode(
                meshNodeDescriptor(ownerA, 1, "tcp://127.0.0.1:6000"),
                ZLinkLocationWriteIntent.NEW_CLAIM)
            .toCompletableFuture()
            .get();
        var ownerB = assertInstanceOf(
            ZLinkOwnerLeaseClaimed.class,
            store.claimOwnerLease("owner-b", Duration.ofSeconds(30))
                .toCompletableFuture().get()).token();

        var conflict = store.updateMeshNode(
                meshNodeDescriptor(
                    ownerB,
                    1,
                    "tcp://127.0.0.1:6000"),
                ZLinkLocationWriteIntent.NEW_CLAIM)
            .toCompletableFuture()
            .get();

        assertEquals(ZLinkLocationWriteStatus.STORED, first.status());
        assertEquals(7, first.generation());
        assertEquals(ZLinkLocationWriteStatus.REJECTED_CONFLICT, conflict.status());
    }

    @Test
    void releasedOwnerLeaseAllowsNewClaimWithNewLifecycle() throws Exception {
        ZLinkInMemoryLocationStore store = new ZLinkInMemoryLocationStore(Clock.fixed(NOW, ZoneOffset.UTC));
        var ownerA = assertInstanceOf(
            ZLinkOwnerLeaseClaimed.class,
            store.claimOwnerLease("owner-a", Duration.ofSeconds(30))
                .toCompletableFuture().get()).token();
        store.updateMeshNode(
                meshNodeDescriptor(ownerA, 1, "tcp://127.0.0.1:6000"),
                ZLinkLocationWriteIntent.NEW_CLAIM)
            .toCompletableFuture()
            .get();
        assertEquals(
            ZLinkOwnerLeaseReleaseResult.RELEASED,
            store.releaseOwnerLease(ownerA).toCompletableFuture().get());
        var ownerB = assertInstanceOf(
            ZLinkOwnerLeaseClaimed.class,
            store.claimOwnerLease("owner-b", Duration.ofSeconds(30))
                .toCompletableFuture().get()).token();

        var replacement = store.updateMeshNode(
                meshNodeDescriptor(
                    ownerB,
                    NODE_A,
                    8,
                    1,
                    "tcp://127.0.0.1:6000",
                    "mesh-entry-00000000-0000-4000-8000-000000000001"),
                ZLinkLocationWriteIntent.NEW_CLAIM)
            .toCompletableFuture()
            .get();

        assertEquals(ZLinkLocationWriteStatus.STORED, replacement.status());
        assertEquals(8, replacement.generation());
    }

    private static ZLinkObjectReservation reserveActor(
        ZLinkInMemoryAuthorityStore store,
        String key,
        ZLinkLocationOwnerToken owner) throws Exception {
        return assertInstanceOf(
            ZLinkObjectReserved.class,
            store.reserve(reservationRequest(key, owner), () -> false)
                .toCompletableFuture().get())
            .reservation();
    }

    private static ZLinkObjectReservationRequest reservationRequest(
        String key,
        ZLinkLocationOwnerToken owner) {
        return new ZLinkObjectReservationRequest(
            ZLinkPlacementObjectKind.ACTOR,
            key,
            "player",
            "creation-root",
            new byte[32],
            32,
            new ZLinkMeshNodeDescriptorKey("mesh", NODE_A),
            7,
            owner,
            new byte[] {1},
            ZLinkPlacementCapacityBundle.actor(1));
    }

    private static ZLinkCreationOperationTerminal terminal(
        ZLinkCreationOperationIdentity operation,
        ZLinkObjectReservation reservation,
        ZLinkCreationTerminalState state,
        byte[] envelope) throws Exception {
        return new ZLinkCreationOperationTerminal(
            operation,
            reservation,
            state,
            envelope,
            java.security.MessageDigest
                .getInstance("SHA-256")
                .digest(envelope),
            NOW.plus(Duration.ofMinutes(5)));
    }

    private static ZLinkInMemoryAuthorityStore newAuthorityStore() {
        return new ZLinkInMemoryAuthorityStore(
            Clock.fixed(NOW, ZoneOffset.UTC),
            ignored -> true,
            (descriptor, generation, owner) ->
                meshNodeDescriptor(
                    owner,
                    1,
                    "tcp://127.0.0.1:7000"));
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
                        new ZLinkMeshNodeDescriptorKey("mesh", NODE_A),
                        7,
                        owner,
                        new byte[] {1},
                        ZLinkPlacementCapacityBundle.actor(1)),
                    () -> false)
                .toCompletableFuture().get()).reservation();
        assertEquals(
            ZLinkObjectCommitResult.COMMITTED,
            store.commit(reserved, new byte[] {1}, () -> false)
                .toCompletableFuture().get());
        return assertInstanceOf(
            ZLinkAuthoritySnapshot.class,
            store.read(key, () -> false).toCompletableFuture().get());
    }

    private static ZLinkMeshNodeDescriptor onlyMeshNode(
        ZLinkInMemoryLocationStore store) throws Exception {
        return store.listMeshNodes(
                "mesh",
                systems.zlink.framework.locations.ZLinkPageRequest
                    .firstPage())
            .toCompletableFuture().get().items().getFirst();
    }

    private static ZLinkFanoutPublisherDescriptor fanoutPublisher(
        ZLinkLocationOwnerToken owner,
        long lifecycleGeneration,
        long revision,
        String endpoint) {
        return new ZLinkFanoutPublisherDescriptor(
            "events",
            NODE_A,
            lifecycleGeneration,
            revision,
            endpoint,
            systems.zlink.framework.runtime.host
                .ZLinkFrameworkRuntimeState.SERVING,
            "security",
            owner.ownerId(),
            owner.leaseGeneration(),
            NOW);
    }

    private static ZLinkMeshNodeDescriptor meshNodeDescriptor(
        ZLinkLocationOwnerToken owner,
        long revision,
        String endpoint) {
        return meshNodeDescriptor(
            owner,
            NODE_A,
            7,
            revision,
            endpoint,
            "mesh-entry-00000000-0000-4000-8000-000000000001");
    }

    private static ZLinkMeshNodeDescriptor meshNodeDescriptor(
        ZLinkLocationOwnerToken owner,
        RoutingId rid,
        long lifecycleGeneration,
        long revision,
        String endpoint,
        String entrySpotId) {
        return new ZLinkMeshNodeDescriptor(
            "mesh",
            rid,
            lifecycleGeneration,
            revision,
            endpoint,
            Map.of("game", 100),
            1,
            List.of(new ZLinkObjectCapability(
                ZLinkPlacementObjectKind.ACTOR,
                "player",
                ZLinkObjectMaintenancePolicyKind.SNAPSHOT,
                true,
                0)),
            ZLinkMeshNodeObjectRole.SERVER,
            Optional.of(entrySpotId),
            100,
            actorCapacity(0, 0, 8),
            new ZLinkActivationConcurrency(0, 128),
            Optional.empty(),
            systems.zlink.framework.runtime.host
                .ZLinkFrameworkRuntimeState.SERVING,
            "security",
            owner.ownerId(),
            owner.leaseGeneration(),
            NOW);
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
}
