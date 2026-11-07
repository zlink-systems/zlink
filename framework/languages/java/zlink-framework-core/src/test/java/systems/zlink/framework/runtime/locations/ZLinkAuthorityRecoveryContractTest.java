package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.RepeatedTest;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.locations.*;

import java.time.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicLong;

final class ZLinkAuthorityRecoveryContractTest {
    private static final Duration MARGIN = Duration.ofNanos(10);

    private static final class MutableClock extends Clock {
        Instant now = Instant.parse("2026-01-01T00:00:00Z");
        volatile Runnable beforeRead;

        public ZoneId getZone() {
            return ZoneOffset.UTC;
        }

        public Clock withZone(ZoneId zone) {
            return this;
        }

        public Instant instant() {
            Runnable action = beforeRead;
            if (action != null) action.run();
            return now;
        }
    }

    private static <T> T await(CompletionStage<T> stage) {
        return stage.toCompletableFuture().orTimeout(5, TimeUnit.SECONDS).join();
    }

    private static final class ProbeStore implements ZLinkLocationStore {
        final ZLinkLocationStore inner;
        final List<ZLinkStoreWriteRequest> writes = new ArrayList<>();
        boolean failScan;
        boolean expireContinuation;
        Runnable onExpire;
        Runnable beforeWrite;

        ProbeStore(ZLinkLocationStore inner) {
            this.inner = inner;
        }

        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key,
                systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
            return inner.read(key, cancellation);
        }

        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request,
                systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
            writes.add(request);
            var before = beforeWrite;
            beforeWrite = null;
            if (before != null) before.run();
            return inner.write(request, cancellation);
        }

        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request,
                systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
            if (failScan)
                return CompletableFuture.failedFuture(
                        new IllegalStateException("injected scan failure"));
            if (expireContinuation && request.cursor() != null) {
                expireContinuation = false;
                if (onExpire != null) onExpire.run();
                return CompletableFuture.completedFuture(new ZLinkStoreScanExpired());
            }
            return inner.scan(
                    new ZLinkStoreScanRequest(request.prefix(), request.cursor(), 1), cancellation);
        }
    }

    private static final class Fixture {
        final MutableClock clock = new MutableClock();
        final AtomicLong nanos = new AtomicLong();
        final ZLinkInMemoryProviderLocationStore raw =
                new ZLinkInMemoryProviderLocationStore(clock);
        final ProbeStore probe = new ProbeStore(raw);
        final ZLinkLocationRepository repository;
        ZLinkLocationOwnerToken owner;
        final ZLinkMeshNodeDescriptorKey node =
                new ZLinkMeshNodeDescriptorKey("game", RoutingId.from("node"));

        Fixture(boolean provider) {
            repository =
                    provider
                            ? new ZLinkProviderLocationRepository(probe, nanos::get, MARGIN)
                            : new ZLinkInMemoryLocationStore(clock, nanos::get, MARGIN);
            owner =
                    assertInstanceOf(
                                    ZLinkOwnerLeaseClaimed.class,
                                    await(
                                            repository.claimOwnerLease(
                                                    "owner", Duration.ofSeconds(1))))
                            .token();
            publish(1);
        }

        long publishedLifecycle;
        long descriptorRevision;

        void publish(long lifecycle) {
            boolean renew = lifecycle == publishedLifecycle;
            var d =
                    new ZLinkMeshNodeDescriptor(
                            "game",
                            node.rid(),
                            lifecycle,
                            ++descriptorRevision,
                            "tcp://127.0.0.1:7100",
                            Map.of(),
                            1,
                            List.of(
                                    new ZLinkObjectCapability(
                                            ZLinkPlacementObjectKind.USER_SPOT,
                                            "room",
                                            ZLinkObjectMaintenancePolicyKind.DISABLED,
                                            false,
                                            0),
                                    new ZLinkObjectCapability(
                                            ZLinkPlacementObjectKind.ACTOR,
                                            "player",
                                            ZLinkObjectMaintenancePolicyKind.DISABLED,
                                            false,
                                            0)),
                            ZLinkMeshNodeObjectRole.SERVER,
                            Optional.of("entry"),
                            100,
                            new ZLinkPlacementCapacity(
                                    new ZLinkCapacityUsage(0, 0, 32),
                                    new ZLinkCapacityUsage(0, 0, 32),
                                    List.of()),
                            new ZLinkActivationConcurrency(0, 64),
                            Optional.empty(),
                            ZLinkFrameworkRuntimeState.SERVING,
                            "security",
                            owner.ownerId(),
                            owner.leaseGeneration(),
                            clock.instant());
            assertEquals(
                    ZLinkLocationWriteStatus.STORED,
                    await(
                                    repository.updateMeshNode(
                                            d,
                                            renew
                                                    ? ZLinkLocationWriteIntent.RENEW
                                                    : ZLinkLocationWriteIntent.NEW_CLAIM))
                            .status());
            publishedLifecycle = lifecycle;
        }

        void expireAndClaim() {
            clock.now = clock.now.plus(Duration.ofHours(2));
            owner =
                    assertInstanceOf(
                                    ZLinkOwnerLeaseClaimed.class,
                                    await(repository.claimOwnerLease("owner", Duration.ofHours(1))))
                            .token();
        }

        ZLinkAuthoritySnapshot read(String key) {
            return assertInstanceOf(
                    ZLinkAuthoritySnapshot.class, await(repository.read(key, () -> false)));
        }

        ZLinkObjectReservation reserve(String key, boolean actor, byte[] payload) {
            return assertInstanceOf(
                            ZLinkObjectReserved.class,
                            await(
                                    repository.reserve(
                                            new ZLinkObjectReservationRequest(
                                                    actor
                                                            ? ZLinkPlacementObjectKind.ACTOR
                                                            : ZLinkPlacementObjectKind.USER_SPOT,
                                                    key,
                                                    actor ? "player" : "room",
                                                    "intent-" + key,
                                                    new byte[32],
                                                    0,
                                                    node,
                                                    1,
                                                    owner,
                                                    payload,
                                                    actor
                                                            ? ZLinkPlacementCapacityBundle.actor(1)
                                                            : ZLinkPlacementCapacityBundle.spot(
                                                                    ZLinkPlacementObjectKind
                                                                            .USER_SPOT,
                                                                    "room",
                                                                    1)),
                                            () -> false)))
                    .reservation();
        }

        ZLinkAuthoritySnapshot spot(String id) {
            String key = ZLinkAuthorityKeyCodec.spot(id);
            byte[] payload =
                    new ZLinkServiceAuthorityPayloadCodec()
                            .encodeUser(
                                    ZLinkServiceAuthorityPayloadCodec.State.READY,
                                    "room",
                                    id,
                                    owner.ownerId(),
                                    owner.leaseGeneration(),
                                    "game",
                                    node.rid(),
                                    1);
            var reservation = reserve(key, false, payload);
            assertEquals(
                    ZLinkObjectCommitResult.COMMITTED,
                    await(repository.commit(reservation, payload, () -> false)));
            return read(key);
        }

        ZLinkAuthoritySnapshot actor(String id, String spot, long generation) {
            String key = ZLinkAuthorityKeyCodec.actor(id);
            byte[] payload =
                    new ZLinkActorAuthorityPayloadCodec()
                            .encode(
                                    ZLinkActorAuthorityPayloadCodec.State.READY,
                                    "player",
                                    id,
                                    spot,
                                    generation,
                                    2,
                                    owner.ownerId(),
                                    owner.leaseGeneration(),
                                    "game",
                                    node.rid(),
                                    1);
            var reservation = reserve(key, true, payload);
            assertEquals(
                    ZLinkObjectCommitResult.COMMITTED,
                    await(repository.commit(reservation, payload, () -> false)));
            return read(key);
        }

        ZLinkAuthorityWriteResult preserve(String key, ZLinkAuthoritySnapshot before) {
            return await(
                    repository.compareExchange(
                            key,
                            new ZLinkAuthorityExpectFound(before.storeVersion()),
                            ZLinkAuthorityPut.preserve(before, owner),
                            () -> false));
        }

        boolean cleanup(String key, ZLinkAuthoritySnapshot before) {
            return await(repository.cleanupDiscarded(key, before, owner, () -> false));
        }

        ZLinkPlacementCapacity capacity() {
            return await(repository.listMeshNodes("game", ZLinkPageRequest.firstPage()))
                    .items()
                    .getFirst()
                    .capacity();
        }
    }

    @RepeatedTest(value = 100, name = "{displayName} [{currentRepetition}/{totalRepetitions}]")
    void preserveAndCleanupUseIdenticalInputsForBothRepositories() {
        assertAll(
                () -> preserveAndCleanupUseIdenticalInputsForBothRepositories(false),
                () -> preserveAndCleanupUseIdenticalInputsForBothRepositories(true));
    }

    private void preserveAndCleanupUseIdenticalInputsForBothRepositories(boolean provider) {
        var f = new Fixture(provider);
        String spot = ZLinkAuthorityKeyCodec.spot("room");
        var before = f.spot("room");
        var actor = f.actor("a", "room", before.objectGeneration());
        var other = f.actor("b", "room", before.objectGeneration());
        var capacity = f.capacity();
        f.expireAndClaim();
        // Descriptor publication may still be unconfirmed when Preserve is submitted.
        assertInstanceOf(ZLinkAuthorityStored.class, f.preserve(spot, before));
        assertInstanceOf(
                ZLinkAuthorityStored.class, f.preserve(ZLinkAuthorityKeyCodec.actor("a"), actor));
        var after = f.read(spot);
        assertEquals(before.objectGeneration(), after.objectGeneration());
        assertEquals(before.authorityOwnerGeneration(), after.authorityOwnerGeneration());
        assertEquals(before.allocation(), after.allocation());
        assertNotEquals(before.storeVersion(), after.storeVersion());
        assertEquals(f.owner.leaseGeneration(), after.ownerLeaseGeneration());
        assertEquals(
                f.owner.leaseGeneration(),
                new ZLinkServiceAuthorityPayloadCodec()
                        .decode(after.payload())
                        .orElseThrow()
                        .ownerLeaseGeneration());
        assertEquals(capacity, f.capacity());
        assertInstanceOf(ZLinkAuthorityConflict.class, f.preserve(spot, before));
        assertFalse(f.cleanup(spot, before));
        assertTrue(f.cleanup(ZLinkAuthorityKeyCodec.actor("a"), actor));
        assertEquals(
                other.storeVersion(), f.read(ZLinkAuthorityKeyCodec.actor("b")).storeVersion());
        assertFalse(f.cleanup(spot, before));
        assertFalse(f.cleanup(ZLinkAuthorityKeyCodec.actor("b"), other));
        f.nanos.set(29);
        assertFalse(f.cleanup(ZLinkAuthorityKeyCodec.actor("b"), other));
        f.nanos.set(30);
        assertTrue(f.cleanup(ZLinkAuthorityKeyCodec.actor("b"), other));
        assertTrue(f.cleanup(spot, before));
        assertTrue(f.cleanup(spot, before));
        assertEquals(0, f.capacity().actors().active());
        assertEquals(0, f.capacity().spots().active());
    }

    @RepeatedTest(value = 100, name = "{displayName} [{currentRepetition}/{totalRepetitions}]")
    void preserveRejectsWrongIdentityVersionLifecycleAndReservedWithoutChangingRows() {
        assertAll(
                () ->
                        preserveRejectsWrongIdentityVersionLifecycleAndReservedWithoutChangingRows(
                                false),
                () ->
                        preserveRejectsWrongIdentityVersionLifecycleAndReservedWithoutChangingRows(
                                true));
    }

    private void preserveRejectsWrongIdentityVersionLifecycleAndReservedWithoutChangingRows(
            boolean provider) {
        var f = new Fixture(provider);
        String key = ZLinkAuthorityKeyCodec.spot("room");
        var before = f.spot("room");
        // A live old token is not an expired-token recovery transition.
        assertInstanceOf(ZLinkAuthorityConflict.class, f.preserve(key, before));
        f.expireAndClaim();
        var capacity = f.capacity();
        for (int mismatch = 0; mismatch < 6; mismatch++) {
            var a = before.allocation();
            var fake =
                    new ZLinkAuthoritySnapshot(
                            mismatch == 0 ? "wrong" : before.storeVersion(),
                            before.payload(),
                            before.objectGeneration() + (mismatch == 1 ? 1 : 0),
                            before.authorityOwnerGeneration() + (mismatch == 2 ? 1 : 0),
                            mismatch == 3 ? "other" : before.ownerId(),
                            before.ownerLeaseGeneration() + (mismatch == 4 ? 1 : 0),
                            mismatch == 5
                                    ? new ZLinkPlacementAllocation(
                                            a.state(),
                                            a.objectKind(),
                                            a.stableType(),
                                            a.descriptor(),
                                            2,
                                            a.capacityBundle())
                                    : a,
                            before.storeNow());
            assertInstanceOf(ZLinkAuthorityConflict.class, f.preserve(key, fake));
            assertEquals(before.storeVersion(), f.read(key).storeVersion());
        }
        var foreign =
                assertInstanceOf(
                                ZLinkOwnerLeaseClaimed.class,
                                await(f.repository.claimOwnerLease("foreign", Duration.ofHours(1))))
                        .token();
        assertInstanceOf(
                ZLinkAuthorityConflict.class,
                await(
                        f.repository.compareExchange(
                                key,
                                new ZLinkAuthorityExpectFound(before.storeVersion()),
                                ZLinkAuthorityPut.preserve(before, foreign),
                                () -> false)));
        assertTrue(
                await(
                        f.repository.cleanupDiscarded(
                                key,
                                new ZLinkAuthoritySnapshot(
                                        before.storeVersion(),
                                        before.payload(),
                                        before.objectGeneration() + 1,
                                        before.authorityOwnerGeneration(),
                                        before.ownerId(),
                                        before.ownerLeaseGeneration(),
                                        before.allocation(),
                                        before.storeNow()),
                                f.owner,
                                () -> false)));
        assertTrue(
                await(
                        f.repository.cleanupDiscarded(
                                key,
                                new ZLinkAuthoritySnapshot(
                                        before.storeVersion(),
                                        before.payload(),
                                        before.objectGeneration(),
                                        before.authorityOwnerGeneration(),
                                        foreign.ownerId(),
                                        foreign.leaseGeneration(),
                                        before.allocation(),
                                        before.storeNow()),
                                foreign,
                                () -> false)));
        assertEquals(before.storeVersion(), f.read(key).storeVersion());
        assertEquals(capacity, f.capacity());
        f.publish(1);
        String reserved = ZLinkAuthorityKeyCodec.spot("reserved");
        f.reserve(reserved, false, new byte[] {1});
        var reservation = f.read(reserved);
        assertInstanceOf(ZLinkAuthorityConflict.class, f.preserve(reserved, reservation));
        assertFalse(f.cleanup(reserved, reservation));
        assertEquals(reservation.storeVersion(), f.read(reserved).storeVersion());
    }

    @RepeatedTest(value = 100, name = "{displayName} [{currentRepetition}/{totalRepetitions}]")
    void invalidTokenCleanupWaitsAndDeleteNeverBypassesTheLease() {
        assertAll(() -> invalidCleanup(false), () -> invalidCleanup(true));
    }

    private void invalidCleanup(boolean provider) {
        var f = new Fixture(provider);
        String key = ZLinkAuthorityKeyCodec.spot("discarded");
        var before = f.spot("discarded");
        f.expireAndClaim();
        assertInstanceOf(
                ZLinkAuthorityConflict.class,
                await(
                        f.repository.compareExchange(
                                key,
                                new ZLinkAuthorityExpectFound(before.storeVersion()),
                                new ZLinkAuthorityDelete(),
                                () -> false)));
        assertFalse(f.cleanup(key, before));
        f.nanos.set(29);
        assertFalse(f.cleanup(key, before));
        assertEquals(before.storeVersion(), f.read(key).storeVersion());
        f.nanos.set(30);
        assertTrue(f.cleanup(key, before));
        assertInstanceOf(ZLinkAuthorityMissing.class, await(f.repository.read(key, () -> false)));
        assertEquals(0, f.capacity().spots().active());
    }

    @RepeatedTest(value = 100, name = "{displayName} [{currentRepetition}/{totalRepetitions}]")
    void currentTokenCleanupUsesCurrentVersionAndOnlyOneActorBatch() {
        assertAll(() -> currentCleanup(false), () -> currentCleanup(true));
    }

    private void currentCleanup(boolean provider) {
        var f = new Fixture(provider);
        var spot = f.spot("room");
        var before = f.actor("a", "room", spot.objectGeneration());
        var other = f.actor("b", "room", spot.objectGeneration());
        String key = ZLinkAuthorityKeyCodec.actor("a");
        var updated =
                assertInstanceOf(
                        ZLinkAuthorityStored.class,
                        await(
                                f.repository.compareExchange(
                                        key,
                                        new ZLinkAuthorityExpectFound(before.storeVersion()),
                                        new ZLinkAuthorityPut(before.payload()),
                                        () -> false)));
        assertNotEquals(before.storeVersion(), updated.storeVersion());
        f.probe.writes.clear();
        assertTrue(f.cleanup(key, before));
        assertEquals(
                other.storeVersion(), f.read(ZLinkAuthorityKeyCodec.actor("b")).storeVersion());
        assertEquals(1, f.capacity().actors().active());
        if (provider) {
            assertEquals(1, f.probe.writes.size());
            var batch = f.probe.writes.getFirst();
            assertEquals(4, batch.conditions().size());
            assertEquals(2, batch.mutations().size());
            assertEquals(
                    List.of(
                            new ZLinkStoreDelete(
                                    ZLinkProviderAuthorityRepositoryTestAccess.authorityKey(key))),
                    batch.mutations().stream().filter(ZLinkStoreDelete.class::isInstance).toList());
        }
    }

    @RepeatedTest(value = 100, name = "{displayName} [{currentRepetition}/{totalRepetitions}]")
    void preserveRacesCleanupOnTheSameStoreVersionWithoutCapacityLoss() {
        for (boolean preserveWins : List.of(false, true)) {
            var f = new Fixture(true);
            String key = ZLinkAuthorityKeyCodec.spot("room");
            var before = f.spot("room");
            f.expireAndClaim();
            assertFalse(f.cleanup(key, before));
            f.nanos.set(30);
            if (preserveWins) {
                f.probe.beforeWrite =
                        () -> assertInstanceOf(ZLinkAuthorityStored.class, f.preserve(key, before));
                assertFalse(f.cleanup(key, before));
                assertEquals(f.owner.leaseGeneration(), f.read(key).ownerLeaseGeneration());
                assertEquals(1, f.capacity().spots().active());
                assertTrue(f.cleanup(key, before));
            } else {
                assertTrue(f.cleanup(key, before));
                assertInstanceOf(ZLinkAuthorityConflict.class, f.preserve(key, before));
            }
            assertEquals(0, f.capacity().spots().active());
            assertInstanceOf(
                    ZLinkAuthorityMissing.class, await(f.repository.read(key, () -> false)));
        }
    }

    @RepeatedTest(value = 100, name = "{displayName} [{currentRepetition}/{totalRepetitions}]")
    void expiredOrFailedMembershipScanIsNeverZeroRows() {
        var f = new Fixture(true);
        String key = ZLinkAuthorityKeyCodec.spot("room");
        var before = f.spot("room");
        f.spot("unrelated");
        f.probe.failScan = true;
        assertThrows(CompletionException.class, () -> f.cleanup(key, before));
        assertEquals(before.storeVersion(), f.read(key).storeVersion());
        f.probe.failScan = false;
        f.probe.expireContinuation = true;
        f.probe.onExpire = () -> f.actor("late", "room", before.objectGeneration());
        assertFalse(f.cleanup(key, before));
        assertFalse(f.probe.expireContinuation);
        assertEquals(before.storeVersion(), f.read(key).storeVersion());
        var member = f.read(ZLinkAuthorityKeyCodec.actor("late"));
        assertTrue(f.cleanup(ZLinkAuthorityKeyCodec.actor("late"), member));
        assertTrue(f.cleanup(key, before));
    }

    @RepeatedTest(value = 100, name = "{displayName} [{currentRepetition}/{totalRepetitions}]")
    void closingAndChangedNodeLifecycleCannotBePreserved() {
        assertAll(() -> rejectedLifecycle(false), () -> rejectedLifecycle(true));
    }

    private void rejectedLifecycle(boolean provider) {
        var invalid = new Fixture(provider);
        var invalidSpot = invalid.spot("invalid");
        var member = invalid.actor("member", "invalid", invalidSpot.objectGeneration());
        String invalidKey = ZLinkAuthorityKeyCodec.spot("invalid");
        assertInstanceOf(
                ZLinkAuthorityStored.class,
                await(
                        invalid.repository.compareExchange(
                                invalidKey,
                                new ZLinkAuthorityExpectFound(invalidSpot.storeVersion()),
                                new ZLinkAuthorityPut(new byte[] {1}),
                                () -> false)));
        var undecodable = invalid.read(invalidKey);
        var failure =
                assertThrows(
                        RuntimeException.class, () -> invalid.cleanup(invalidKey, invalidSpot));
        Throwable cause = failure;
        while (cause instanceof CompletionException && cause.getCause() != null)
            cause = cause.getCause();
        assertEquals(
                systems.zlink.framework.errors.ZLinkFrameworkErrorKind.UNAVAILABLE,
                assertInstanceOf(
                                systems.zlink.framework.errors.ZLinkFrameworkException.class, cause)
                        .kind());
        assertEquals(undecodable.storeVersion(), invalid.read(invalidKey).storeVersion());
        assertEquals(
                member.storeVersion(),
                invalid.read(ZLinkAuthorityKeyCodec.actor("member")).storeVersion());
        var f = new Fixture(provider);
        String key = ZLinkAuthorityKeyCodec.spot("room");
        var before = f.spot("room");
        byte[] closing =
                new ZLinkServiceAuthorityPayloadCodec()
                        .encodeUser(
                                ZLinkServiceAuthorityPayloadCodec.State.CLOSING,
                                "room",
                                "room",
                                f.owner.ownerId(),
                                f.owner.leaseGeneration(),
                                "game",
                                f.node.rid(),
                                1);
        assertInstanceOf(
                ZLinkAuthorityStored.class,
                await(
                        f.repository.compareExchange(
                                key,
                                new ZLinkAuthorityExpectFound(before.storeVersion()),
                                new ZLinkAuthorityPut(closing),
                                () -> false)));
        var closed = f.read(key);
        f.expireAndClaim();
        assertInstanceOf(ZLinkAuthorityConflict.class, f.preserve(key, closed));
        assertEquals(closed.storeVersion(), f.read(key).storeVersion());
        f.publish(1);
        var other = f.spot("other");
        f.expireAndClaim();
        f.publish(2);
        assertInstanceOf(
                ZLinkAuthorityConflict.class,
                f.preserve(ZLinkAuthorityKeyCodec.spot("other"), other));
        assertEquals(
                other.storeVersion(), f.read(ZLinkAuthorityKeyCodec.spot("other")).storeVersion());
    }

    @RepeatedTest(value = 100, name = "{displayName} [{currentRepetition}/{totalRepetitions}]")
    void preserveRebuildsOnlyWhileItsOriginalVersionAndReplacementLeaseRemainEligible() {
        var f = new Fixture(true);
        String key = ZLinkAuthorityKeyCodec.spot("room");
        var before = f.spot("room");
        f.expireAndClaim();
        f.probe.beforeWrite = () -> f.publish(1);
        assertInstanceOf(ZLinkAuthorityStored.class, f.preserve(key, before));
        assertEquals(before.objectGeneration(), f.read(key).objectGeneration());
        assertEquals(before.authorityOwnerGeneration(), f.read(key).authorityOwnerGeneration());
        assertEquals(f.owner.leaseGeneration(), f.read(key).ownerLeaseGeneration());
        assertEquals(1, f.capacity().spots().active());

        var changed = new Fixture(true);
        var changedBefore = changed.spot("room");
        changed.expireAndClaim();
        changed.probe.beforeWrite =
                () -> {
                    assertFalse(changed.cleanup(key, changedBefore));
                    changed.nanos.set(30);
                    assertTrue(changed.cleanup(key, changedBefore));
                };
        assertInstanceOf(ZLinkAuthorityConflict.class, changed.preserve(key, changedBefore));
        assertInstanceOf(
                ZLinkAuthorityMissing.class, await(changed.repository.read(key, () -> false)));
        assertEquals(0, changed.capacity().spots().active());
    }

    @RepeatedTest(value = 100, name = "{displayName} [{currentRepetition}/{totalRepetitions}]")
    void repositoryOperationsLeaveTheFrameworkLaneAvailable() {
        assertAll(
                () -> repositoryOperationsLeaveTheFrameworkLaneAvailable(false),
                () -> repositoryOperationsLeaveTheFrameworkLaneAvailable(true));
    }

    private void repositoryOperationsLeaveTheFrameworkLaneAvailable(boolean provider) {
        for (int operation = 0; operation < 5; operation++) {
            var f = new Fixture(provider);
            String key = ZLinkAuthorityKeyCodec.spot("lane");
            var before = f.spot("lane");
            var entered = new CompletableFuture<Void>();
            var release = new CompletableFuture<Void>();
            f.clock.beforeRead =
                    () -> {
                        f.clock.beforeRead = null;
                        entered.complete(null);
                        release.orTimeout(5, TimeUnit.SECONDS).join();
                    };
            var occupied =
                    CompletableFuture.supplyAsync(() -> await(f.repository.read(key, () -> false)));
            var lane = new systems.zlink.framework.runtime.internal.execution.ZLinkStateLane();
            final int selected = operation;
            try {
                entered.orTimeout(5, TimeUnit.SECONDS).join();
                var submitted =
                        lane.runAsync(
                                () ->
                                        switch (selected) {
                                            case 0 -> f.repository.read(key, () -> false);
                                            case 1 ->
                                                    f.repository.compareExchange(
                                                            key,
                                                            new ZLinkAuthorityExpectFound(
                                                                    before.storeVersion()),
                                                            new ZLinkAuthorityPut(before.payload()),
                                                            () -> false);
                                            default ->
                                                    f.repository.cleanupDiscarded(
                                                            key, before, f.owner, () -> false);
                                        });
                var unblocked = lane.runAsync(() -> release.complete(null));
                // Timeout guards detect deadlock; no elapsed-time policy is asserted.
                await(await(submitted));
                await(unblocked);
                await(occupied);
            } finally {
                release.complete(null);
                await(lane.closeAsync());
            }
        }
    }

    @RepeatedTest(value = 100, name = "{displayName} [{currentRepetition}/{totalRepetitions}]")
    void discardedCleanupRebuildsItsOriginalVersionAfterUnrelatedConflict() {
        var f = new Fixture(true);
        String key = ZLinkAuthorityKeyCodec.spot("room");
        var before = f.spot("room");
        f.expireAndClaim();
        assertFalse(f.cleanup(key, before));
        f.nanos.set(30);
        f.probe.beforeWrite = () -> f.publish(1);
        assertTrue(f.cleanup(key, before));
        assertInstanceOf(ZLinkAuthorityMissing.class, await(f.repository.read(key, () -> false)));
        assertEquals(0, f.capacity().spots().active());
    }

    @RepeatedTest(value = 100, name = "{displayName} [{currentRepetition}/{totalRepetitions}]")
    void preserveRejectsAnExpiredReplacementLeaseWithoutChangingTheAuthority() {
        assertAll(() -> expiredReplacement(false), () -> expiredReplacement(true));
    }

    private void expiredReplacement(boolean provider) {
        var f = new Fixture(provider);
        String key = ZLinkAuthorityKeyCodec.spot("room");
        var before = f.spot("room");
        f.expireAndClaim();
        var expired = f.owner;
        f.expireAndClaim();
        assertInstanceOf(
                ZLinkAuthorityConflict.class,
                await(
                        f.repository.compareExchange(
                                key,
                                new ZLinkAuthorityExpectFound(before.storeVersion()),
                                ZLinkAuthorityPut.preserve(before, expired),
                                () -> false)));
        assertEquals(before.storeVersion(), f.read(key).storeVersion());
        assertInstanceOf(ZLinkAuthorityStored.class, f.preserve(key, before));
        assertEquals(1, f.capacity().spots().active());
    }
}
