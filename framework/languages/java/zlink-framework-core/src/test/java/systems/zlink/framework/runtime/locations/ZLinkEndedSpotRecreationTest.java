package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.configuration.ZLinkObjectFactoryRegistration.RelocationPolicy;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.spots.ZLinkSpotKind;

import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletionException;

final class ZLinkEndedSpotRecreationTest {
    @ParameterizedTest
    @CsvSource({"false", "true"})
    void nonMembershipAuthorityCommitKeepsGenerationOnlyOwnerFence(boolean missingExpiry) {
        var provider = new LeaseExpiryViewProvider(missingExpiry);
        var fixture =
                fixture(
                        new ZLinkProviderLocationRepository(provider),
                        ZLinkPlacementObjectKind.INSTANCE_SPOT,
                        ZLinkServiceAuthorityPayloadCodec.State.READY);
        var before =
                (ZLinkAuthoritySnapshot)
                        fixture.store.read(fixture.key, () -> false).toCompletableFuture().join();
        provider.expiredOwner = fixture.oldOwner.ownerId();
        assertInstanceOf(
                ZLinkAuthorityStored.class,
                fixture.store
                        .compareExchange(
                                fixture.key,
                                new ZLinkAuthorityExpectFound(before.storeVersion()),
                                new ZLinkAuthorityPut(before.payload()),
                                () -> false)
                        .toCompletableFuture()
                        .join());
    }

    @ParameterizedTest
    @CsvSource({"false", "true"})
    void membershipCommitRejectsMatchingSpotLeaseWithInvalidExpiry(boolean missingExpiry) {
        var provider = new LeaseExpiryViewProvider(missingExpiry);
        var fixture =
                fixture(
                        new ZLinkProviderLocationRepository(provider),
                        ZLinkPlacementObjectKind.USER_SPOT,
                        ZLinkServiceAuthorityPayloadCodec.State.READY);
        var actor = member(fixture, "expiry-member", false);
        var before =
                (ZLinkAuthoritySnapshot)
                        fixture.store
                                .read(actor.authorityKey(), () -> false)
                                .toCompletableFuture()
                                .join();
        provider.expiredOwner = fixture.oldOwner.ownerId();
        assertInstanceOf(
                ZLinkAuthorityConflict.class,
                fixture.store
                        .compareExchange(
                                actor.authorityKey(),
                                new ZLinkAuthorityExpectFound(before.storeVersion()),
                                new ZLinkAuthorityPut(
                                        memberPayload(fixture, "expiry-member", true)),
                                () -> false)
                        .toCompletableFuture()
                        .join());
        assertEquals(
                before.storeVersion(),
                ((ZLinkAuthoritySnapshot)
                                fixture.store
                                        .read(actor.authorityKey(), () -> false)
                                        .toCompletableFuture()
                                        .join())
                        .storeVersion());
    }

    @ParameterizedTest
    @CsvSource({"false", "true"})
    void nonMembershipCreationCommitKeepsGenerationOnlyOwnerFence(boolean missingExpiry) {
        var provider = new LeaseExpiryViewProvider(missingExpiry);
        var fixture =
                fixture(
                        new ZLinkProviderLocationRepository(provider),
                        ZLinkPlacementObjectKind.INSTANCE_SPOT,
                        ZLinkServiceAuthorityPayloadCodec.State.READY);
        var reservation =
                assertInstanceOf(
                                ZLinkObjectReserved.class,
                                fixture.store
                                        .reserve(
                                                new ZLinkObjectReservationRequest(
                                                        ZLinkPlacementObjectKind.INSTANCE_SPOT,
                                                        ZLinkAuthorityKeyCodec.spot(
                                                                "non-membership-creation"),
                                                        "room",
                                                        "intent",
                                                        new byte[32],
                                                        1,
                                                        new ZLinkMeshNodeDescriptorKey(
                                                                "mesh",
                                                                fixture.newDescriptor.rid()),
                                                        1,
                                                        fixture.newOwner,
                                                        new byte[] {1},
                                                        ZLinkPlacementCapacityBundle.spot(
                                                                ZLinkPlacementObjectKind
                                                                        .INSTANCE_SPOT,
                                                                "room",
                                                                1),
                                                        new RelocationPolicy.Disabled()),
                                                () -> false)
                                        .toCompletableFuture()
                                        .join())
                        .reservation();
        provider.expiredOwner = fixture.newOwner.ownerId();
        assertEquals(
                ZLinkObjectCommitResult.COMMITTED,
                fixture.store
                        .commit(reservation, new byte[] {2}, () -> false)
                        .toCompletableFuture()
                        .join());
    }

    private static final class LeaseExpiryViewProvider implements ZLinkLocationStore {
        private final ZLinkLocationStore delegate = new ZLinkInMemoryProviderLocationStore();
        private final boolean missingExpiry;
        private String expiredOwner;

        private LeaseExpiryViewProvider(boolean missingExpiry) {
            this.missingExpiry = missingExpiry;
        }

        public java.util.concurrent.CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            return delegate.read(key, cancellation)
                    .thenApply(
                            read -> {
                                if (expiredOwner == null
                                        || !(read instanceof ZLinkStoreReadFound found))
                                    return read;
                                var value = found.value();
                                try {
                                    var record =
                                            new com.fasterxml.jackson.databind.ObjectMapper()
                                                    .readTree(value.bytes());
                                    if (!record.has("leaseGeneration")
                                            || !record.path("ownerId")
                                                    .asText()
                                                    .equals(expiredOwner)) return read;
                                } catch (java.io.IOException failure) {
                                    throw new AssertionError(failure);
                                }
                                return new ZLinkStoreReadFound(
                                        new ZLinkStoreValue(
                                                value.bytes(),
                                                value.version(),
                                                missingExpiry ? null : value.storeNow(),
                                                value.storeNow()));
                            });
        }

        public java.util.concurrent.CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            return delegate.write(request, cancellation);
        }

        public java.util.concurrent.CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return delegate.scan(request, cancellation);
        }
    }

    @ParameterizedTest
    @CsvSource({
        "provider, USER_SPOT",
        "provider, INSTANCE_SPOT",
        "memory, USER_SPOT",
        "memory, INSTANCE_SPOT"
    })
    void disabledEmptySteadyReadySpotCreatesNewGeneration(
            String backend, ZLinkPlacementObjectKind kind) throws ReflectiveOperationException {
        var fixture = fixture(backend, kind, ZLinkServiceAuthorityPayloadCodec.State.READY);
        assertEquals(1, oldSpotSlots(fixture));
        fixture.store.releaseOwnerLease(fixture.oldOwner).toCompletableFuture().join();
        var next =
                assertInstanceOf(
                                ZLinkObjectReserved.class,
                                fixture.store
                                        .reserve(
                                                request(
                                                        fixture,
                                                        fixture.newOwner,
                                                        fixture.newDescriptor,
                                                        new RelocationPolicy.Disabled()),
                                                () -> false)
                                        .toCompletableFuture()
                                        .join())
                        .reservation();
        assertTrue(next.objectGeneration() > fixture.reservation.objectGeneration());
        assertEquals(0, oldSpotSlots(fixture));
        assertInstanceOf(
                ZLinkObjectConflict.class,
                fixture.store
                        .reserve(
                                request(
                                        fixture,
                                        fixture.newOwner,
                                        fixture.newDescriptor,
                                        new RelocationPolicy.Disabled()),
                                () -> false)
                        .toCompletableFuture()
                        .join());
    }

    private static long oldSpotSlots(Fixture fixture) throws ReflectiveOperationException {
        return oldSlots(fixture, false);
    }

    private static long oldSlots(Fixture fixture, boolean actors)
            throws ReflectiveOperationException {
        if (fixture.store instanceof ZLinkInMemoryLocationStore) {
            var field = ZLinkInMemoryLocationStore.class.getDeclaredField("authority");
            field.setAccessible(true);
            return ((ZLinkInMemoryAuthorityStore) field.get(fixture.store))
                    .kindCapacity(
                            new ZLinkMeshNodeDescriptorKey("mesh", fixture.oldDescriptor.rid()),
                            1,
                            actors)[0];
        }
        var field = ZLinkProviderLocationRepository.class.getDeclaredField("provider");
        field.setAccessible(true);
        var provider = (ZLinkLocationStore) field.get(fixture.store);
        var read =
                (ZLinkStoreReadFound)
                        provider.read(new ZLinkStoreKey("zlink:v11:capacity:mesh:old"), () -> false)
                                .toCompletableFuture()
                                .join();
        try {
            return new com.fasterxml.jackson.databind.ObjectMapper()
                    .readTree(read.value().bytes())
                    .path("active")
                    .path(actors ? "actors" : "spots")
                    .asLong();
        } catch (java.io.IOException failure) {
            throw new AssertionError(failure);
        }
    }

    @ParameterizedTest
    @CsvSource({
        "provider, USER_SPOT, true",
        "provider, USER_SPOT, false",
        "provider, INSTANCE_SPOT, true",
        "provider, INSTANCE_SPOT, false",
        "memory, USER_SPOT, true",
        "memory, USER_SPOT, false",
        "memory, INSTANCE_SPOT, true",
        "memory, INSTANCE_SPOT, false"
    })
    void endedReadyKindOrTypeMismatchLeavesAuthorityUnchanged(
            String backend, ZLinkPlacementObjectKind kind, boolean mismatchKind) {
        var fixture = fixture(backend, kind, ZLinkServiceAuthorityPayloadCodec.State.READY);
        fixture.store.releaseOwnerLease(fixture.oldOwner).toCompletableFuture().join();
        var before =
                (ZLinkAuthoritySnapshot)
                        fixture.store.read(fixture.key, () -> false).toCompletableFuture().join();
        var original =
                request(
                        fixture,
                        fixture.newOwner,
                        fixture.newDescriptor,
                        new RelocationPolicy.Disabled());
        var requestedKind =
                mismatchKind
                        ? (kind == ZLinkPlacementObjectKind.USER_SPOT
                                ? ZLinkPlacementObjectKind.INSTANCE_SPOT
                                : ZLinkPlacementObjectKind.USER_SPOT)
                        : kind;
        var requestedType = mismatchKind ? "room" : "other";
        var request =
                new ZLinkObjectReservationRequest(
                        requestedKind,
                        fixture.key,
                        requestedType,
                        original.creationIntentReference(),
                        original.creationIntentHash(),
                        original.creationIntentEncodedSize(),
                        original.targetDescriptor(),
                        original.targetDescriptorLifecycleGeneration(),
                        original.targetOwner(),
                        original.creatingPayload(),
                        ZLinkPlacementCapacityBundle.spot(requestedKind, requestedType, 1),
                        original.relocationPolicy());
        assertInstanceOf(
                ZLinkObjectTypeMismatch.class,
                fixture.store.reserve(request, () -> false).toCompletableFuture().join());
        var after =
                (ZLinkAuthoritySnapshot)
                        fixture.store.read(fixture.key, () -> false).toCompletableFuture().join();
        assertEquals(before.storeVersion(), after.storeVersion());
        assertArrayEquals(before.payload(), after.payload());
    }

    @ParameterizedTest
    @CsvSource({
        "provider, USER_SPOT",
        "provider, INSTANCE_SPOT",
        "memory, USER_SPOT",
        "memory, INSTANCE_SPOT"
    })
    void unregisteredTypeDoesNotReleaseEndedReadySpot(
            String backend, ZLinkPlacementObjectKind kind) {
        var fixture = fixture(backend, kind, ZLinkServiceAuthorityPayloadCodec.State.READY);
        fixture.store.releaseOwnerLease(fixture.oldOwner).toCompletableFuture().join();
        assertUnavailableAndUnchanged(fixture, null);
    }

    @ParameterizedTest
    @CsvSource({
        "provider, USER_SPOT",
        "provider, INSTANCE_SPOT",
        "memory, USER_SPOT",
        "memory, INSTANCE_SPOT"
    })
    void closingSpotDoesNotBecomeNewGeneration(String backend, ZLinkPlacementObjectKind kind) {
        var fixture = fixture(backend, kind, ZLinkServiceAuthorityPayloadCodec.State.CLOSING);
        fixture.store.releaseOwnerLease(fixture.oldOwner).toCompletableFuture().join();
        assertUnavailableAndUnchanged(fixture, new RelocationPolicy.Disabled());
    }

    @ParameterizedTest
    @CsvSource({
        "provider, USER_SPOT",
        "provider, INSTANCE_SPOT",
        "memory, USER_SPOT",
        "memory, INSTANCE_SPOT"
    })
    void liveLeaseDoesNotReleaseReadySpot(String backend, ZLinkPlacementObjectKind kind)
            throws ReflectiveOperationException {
        var fixture = fixture(backend, kind, ZLinkServiceAuthorityPayloadCodec.State.READY);
        var before =
                (ZLinkAuthoritySnapshot)
                        fixture.store.read(fixture.key, () -> false).toCompletableFuture().join();
        assertFalse(
                fixture.store
                        .releaseEndedReservation(
                                request(
                                        fixture,
                                        fixture.newOwner,
                                        fixture.newDescriptor,
                                        new RelocationPolicy.Disabled()),
                                before.storeVersion(),
                                () -> false)
                        .toCompletableFuture()
                        .join());
        assertEquals(
                before.storeVersion(),
                ((ZLinkAuthoritySnapshot)
                                fixture.store
                                        .read(fixture.key, () -> false)
                                        .toCompletableFuture()
                                        .join())
                        .storeVersion());
        assertEquals(1, oldSpotSlots(fixture));
        assertInstanceOf(
                ZLinkObjectAlreadyExists.class,
                fixture.store
                        .reserve(
                                request(
                                        fixture,
                                        fixture.newOwner,
                                        fixture.newDescriptor,
                                        new RelocationPolicy.Disabled()),
                                () -> false)
                        .toCompletableFuture()
                        .join());
        assertEquals(
                fixture.reservation.objectGeneration(),
                ((ZLinkAuthoritySnapshot)
                                fixture.store
                                        .read(fixture.key, () -> false)
                                        .toCompletableFuture()
                                        .join())
                        .objectGeneration());
    }

    @ParameterizedTest
    @CsvSource({
        "provider, USER_SPOT",
        "provider, INSTANCE_SPOT",
        "memory, USER_SPOT",
        "memory, INSTANCE_SPOT"
    })
    void nonDisabledTypeDoesNotReleaseEndedReadySpot(
            String backend, ZLinkPlacementObjectKind kind) {
        var fixture = fixture(backend, kind, ZLinkServiceAuthorityPayloadCodec.State.READY);
        fixture.store.releaseOwnerLease(fixture.oldOwner).toCompletableFuture().join();
        assertUnavailableAndUnchanged(fixture, new RelocationPolicy.Recreate());
    }

    @ParameterizedTest
    @CsvSource({"provider", "memory"})
    void userSpotWithMemberDoesNotReleaseEndedReadySpot(String backend) {
        var fixture =
                fixture(
                        backend,
                        ZLinkPlacementObjectKind.USER_SPOT,
                        ZLinkServiceAuthorityPayloadCodec.State.READY);
        member(fixture, "member", true);
        fixture.store.releaseOwnerLease(fixture.oldOwner).toCompletableFuture().join();
        assertUnavailableAndUnchanged(fixture, new RelocationPolicy.Disabled());
    }

    @ParameterizedTest
    @CsvSource({"provider", "memory"})
    void lateJoinCannotCommitMembershipAfterTargetLeaseEnds(String backend) {
        var fixture =
                fixture(
                        backend,
                        ZLinkPlacementObjectKind.USER_SPOT,
                        ZLinkServiceAuthorityPayloadCodec.State.READY);
        var actor = member(fixture, "late", false);
        fixture.store.releaseOwnerLease(fixture.oldOwner).toCompletableFuture().join();
        var before =
                (ZLinkAuthoritySnapshot)
                        fixture.store
                                .read(actor.authorityKey(), () -> false)
                                .toCompletableFuture()
                                .join();
        assertInstanceOf(
                ZLinkAuthorityConflict.class,
                fixture.store
                        .compareExchange(
                                actor.authorityKey(),
                                new ZLinkAuthorityExpectFound(before.storeVersion()),
                                new ZLinkAuthorityPut(memberPayload(fixture, "late", true)),
                                () -> false)
                        .toCompletableFuture()
                        .join());
        assertEquals(
                before.storeVersion(),
                ((ZLinkAuthoritySnapshot)
                                fixture.store
                                        .read(actor.authorityKey(), () -> false)
                                        .toCompletableFuture()
                                        .join())
                        .storeVersion());
    }

    @ParameterizedTest
    @CsvSource({"provider", "memory"})
    void liveTargetAllowsMembershipCommit(String backend) {
        var fixture =
                fixture(
                        backend,
                        ZLinkPlacementObjectKind.USER_SPOT,
                        ZLinkServiceAuthorityPayloadCodec.State.READY);
        var actor = member(fixture, "live", false);
        var before =
                (ZLinkAuthoritySnapshot)
                        fixture.store
                                .read(actor.authorityKey(), () -> false)
                                .toCompletableFuture()
                                .join();
        assertInstanceOf(
                ZLinkAuthorityStored.class,
                fixture.store
                        .compareExchange(
                                actor.authorityKey(),
                                new ZLinkAuthorityExpectFound(before.storeVersion()),
                                new ZLinkAuthorityPut(memberPayload(fixture, "live", true)),
                                () -> false)
                        .toCompletableFuture()
                        .join());
    }

    @ParameterizedTest
    @CsvSource({"provider", "memory"})
    void instanceRecoveryPointerDoesNotPermitRecreation(String backend) {
        var fixture =
                fixture(
                        backend,
                        ZLinkPlacementObjectKind.INSTANCE_SPOT,
                        ZLinkServiceAuthorityPayloadCodec.State.READY);
        var before =
                (ZLinkAuthoritySnapshot)
                        fixture.store.read(fixture.key, () -> false).toCompletableFuture().join();
        byte[] payload =
                new ZLinkServiceAuthorityPayloadCodec()
                        .encodeInstance(
                                ZLinkServiceAuthorityPayloadCodec.State.READY,
                                "room",
                                "ended-spot",
                                fixture.oldOwner.ownerId(),
                                fixture.oldOwner.leaseGeneration(),
                                "mesh",
                                fixture.oldDescriptor.rid(),
                                1,
                                Optional.of(
                                        new ZLinkServiceAuthorityPayloadCodec
                                                .ActivationRecoveryState(
                                                "root", new byte[32], 1, 1, 0)));
        assertInstanceOf(
                ZLinkAuthorityStored.class,
                fixture.store
                        .compareExchange(
                                fixture.key,
                                new ZLinkAuthorityExpectFound(before.storeVersion()),
                                new ZLinkAuthorityPut(payload),
                                () -> false)
                        .toCompletableFuture()
                        .join());
        fixture.store.releaseOwnerLease(fixture.oldOwner).toCompletableFuture().join();
        assertUnavailableAndUnchanged(fixture, new RelocationPolicy.Disabled());
    }

    @ParameterizedTest
    @CsvSource({
        "provider, USER_SPOT",
        "provider, INSTANCE_SPOT",
        "memory, USER_SPOT",
        "memory, INSTANCE_SPOT"
    })
    void endedSpotRecreationRaceHasOneReservationWinner(
            String backend, ZLinkPlacementObjectKind kind) {
        var fixture = fixture(backend, kind, ZLinkServiceAuthorityPayloadCodec.State.READY);
        fixture.store.releaseOwnerLease(fixture.oldOwner).toCompletableFuture().join();
        var request =
                request(
                        fixture,
                        fixture.newOwner,
                        fixture.newDescriptor,
                        new RelocationPolicy.Disabled());
        var first =
                java.util.concurrent.CompletableFuture.supplyAsync(
                        () ->
                                fixture.store
                                        .reserve(request, () -> false)
                                        .toCompletableFuture()
                                        .join());
        var second =
                java.util.concurrent.CompletableFuture.supplyAsync(
                        () ->
                                fixture.store
                                        .reserve(request, () -> false)
                                        .toCompletableFuture()
                                        .join());
        assertEquals(
                1,
                java.util.stream.Stream.of(first.join(), second.join())
                        .filter(ZLinkObjectReserved.class::isInstance)
                        .count());
    }

    @ParameterizedTest
    @CsvSource({
        "provider, USER_SPOT",
        "provider, INSTANCE_SPOT",
        "memory, USER_SPOT",
        "memory, INSTANCE_SPOT"
    })
    void authorityReadAndFindDoNotReclaimEndedSpot(String backend, ZLinkPlacementObjectKind kind) {
        var fixture = fixture(backend, kind, ZLinkServiceAuthorityPayloadCodec.State.READY);
        var before =
                (ZLinkAuthoritySnapshot)
                        fixture.store.read(fixture.key, () -> false).toCompletableFuture().join();
        fixture.store.releaseOwnerLease(fixture.oldOwner).toCompletableFuture().join();
        var after =
                (ZLinkAuthoritySnapshot)
                        fixture.store.read(fixture.key, () -> false).toCompletableFuture().join();
        var options = new ZLinkLocationOptions();
        options.setRouteCacheMaxAge(Duration.ZERO);
        var resolvers =
                new ZLinkStoreLocationResolvers(
                        ZLinkRegisteredLocationStores.fromUnified(fixture.store), options);
        assertThrows(
                CompletionException.class,
                () -> resolvers.resolveSpot("ended-spot").toCompletableFuture().join());
        assertEquals(before.storeVersion(), after.storeVersion());
        assertEquals(before.objectGeneration(), after.objectGeneration());
        assertArrayEquals(before.payload(), after.payload());
    }

    @Test
    void memberAfterFirstPagePreventsReclamation() {
        var provider = new ScanBoundaryProvider(false, false);
        var fixture =
                fixture(
                        new ZLinkProviderLocationRepository(provider),
                        ZLinkPlacementObjectKind.USER_SPOT,
                        ZLinkServiceAuthorityPayloadCodec.State.READY);
        member(fixture, "member", true);
        fixture.store.releaseOwnerLease(fixture.oldOwner).toCompletableFuture().join();
        provider.enabled = true;
        assertUnavailableAndUnchanged(fixture, new RelocationPolicy.Disabled());
        assertEquals(2, provider.scans);
    }

    @Test
    void expiredSnapshotRestartsBeforeDeclaringMembershipEmpty() {
        var provider = new ScanBoundaryProvider(true, false);
        var fixture =
                fixture(
                        new ZLinkProviderLocationRepository(provider),
                        ZLinkPlacementObjectKind.USER_SPOT,
                        ZLinkServiceAuthorityPayloadCodec.State.READY);
        member(fixture, "member", true);
        fixture.store.releaseOwnerLease(fixture.oldOwner).toCompletableFuture().join();
        provider.enabled = true;
        assertUnavailableAndUnchanged(fixture, new RelocationPolicy.Disabled());
        assertEquals(3, provider.scans);
    }

    @Test
    void storeScanFailureNeverMeansEmptyMembership() {
        var provider = new ScanBoundaryProvider(false, true);
        var fixture =
                fixture(
                        new ZLinkProviderLocationRepository(provider),
                        ZLinkPlacementObjectKind.USER_SPOT,
                        ZLinkServiceAuthorityPayloadCodec.State.READY);
        fixture.store.releaseOwnerLease(fixture.oldOwner).toCompletableFuture().join();
        var before =
                (ZLinkAuthoritySnapshot)
                        fixture.store.read(fixture.key, () -> false).toCompletableFuture().join();
        provider.enabled = true;
        var failure =
                assertThrows(
                        CompletionException.class,
                        () ->
                                fixture.store
                                        .reserve(
                                                request(
                                                        fixture,
                                                        fixture.newOwner,
                                                        fixture.newDescriptor,
                                                        new RelocationPolicy.Disabled()),
                                                () -> false)
                                        .toCompletableFuture()
                                        .join());
        var unavailable = assertInstanceOf(ZLinkFrameworkException.class, failure.getCause());
        assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE, unavailable.kind());
        assertSame(provider.failure, unavailable.getCause());
        assertEquals(
                before.storeVersion(),
                ((ZLinkAuthoritySnapshot)
                                fixture.store
                                        .read(fixture.key, () -> false)
                                        .toCompletableFuture()
                                        .join())
                        .storeVersion());
    }

    private static final class ScanBoundaryProvider implements ZLinkLocationStore {
        private final ZLinkInMemoryProviderLocationStore delegate =
                new ZLinkInMemoryProviderLocationStore();
        private final boolean expire;
        private final boolean fail;
        private final IllegalStateException failure =
                new IllegalStateException("injected Store scan failure");
        private boolean enabled;
        private int scans;
        private Runnable afterSnapshot;

        private ScanBoundaryProvider(boolean expire, boolean fail) {
            this.expire = expire;
            this.fail = fail;
        }

        public java.util.concurrent.CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key,
                systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
            return delegate.read(key, cancellation);
        }

        public java.util.concurrent.CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request,
                systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
            return delegate.write(request, cancellation);
        }

        public java.util.concurrent.CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request,
                systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
            if (!enabled)
                return delegate.scan(request, cancellation)
                        .thenApply(
                                result -> {
                                    if (afterSnapshot != null) {
                                        var action = afterSnapshot;
                                        afterSnapshot = null;
                                        action.run();
                                    }
                                    return result;
                                });
            scans++;
            if (fail) return java.util.concurrent.CompletableFuture.failedFuture(failure);
            if (scans == 1)
                return java.util.concurrent.CompletableFuture.completedFuture(
                        new ZLinkStoreScanPageResult(
                                new ZLinkStoreScanPage(
                                        List.of(),
                                        new ZLinkStoreScanCursor("next-page"),
                                        Instant.now())));
            if (expire && scans == 2)
                return java.util.concurrent.CompletableFuture.completedFuture(
                        new ZLinkStoreScanExpired());
            return delegate.scan(
                    new ZLinkStoreScanRequest(request.prefix(), null, request.limit()),
                    cancellation);
        }
    }

    @ParameterizedTest
    @CsvSource({"provider", "memory"})
    void endedOwnerMembersAreReleasedWithTheSpot(String backend)
            throws ReflectiveOperationException {
        var fixture =
                fixture(
                        backend,
                        ZLinkPlacementObjectKind.USER_SPOT,
                        ZLinkServiceAuthorityPayloadCodec.State.READY);
        var dead = endedMember(fixture, "dead-member", true);
        assertEquals(1, oldSlots(fixture, true));
        fixture.store.releaseOwnerLease(fixture.oldOwner).toCompletableFuture().join();
        var next = reserveNewSpot(fixture);
        assertTrue(next.objectGeneration() > fixture.reservation.objectGeneration());
        assertInstanceOf(
                ZLinkAuthorityMissing.class,
                fixture.store.read(dead.authorityKey(), () -> false).toCompletableFuture().join());
        assertEquals(0, oldSlots(fixture, true));
        var recreated =
                assertInstanceOf(
                                ZLinkObjectReserved.class,
                                fixture.store
                                        .reserve(
                                                actorRequest(
                                                        "dead-member",
                                                        fixture.newDescriptor,
                                                        fixture.newOwner,
                                                        new byte[] {1}),
                                                () -> false)
                                        .toCompletableFuture()
                                        .join())
                        .reservation();
        assertTrue(recreated.objectGeneration() > dead.objectGeneration());
    }

    @Test
    void independentMemberRecreationDuringSpotReleasePreservesNewMember() throws Exception {
        var provider = new ScanBoundaryProvider(false, false);
        var fixture =
                fixture(
                        new ZLinkProviderLocationRepository(provider),
                        ZLinkPlacementObjectKind.USER_SPOT,
                        ZLinkServiceAuthorityPayloadCodec.State.READY);
        var oldMember = endedMember(fixture, "independent-member", true);
        fixture.store.releaseOwnerLease(fixture.oldOwner).toCompletableFuture().join();
        var recreated = new java.util.concurrent.atomic.AtomicReference<ZLinkObjectReservation>();
        provider.afterSnapshot =
                () ->
                        recreated.set(
                                assertInstanceOf(
                                                ZLinkObjectReserved.class,
                                                fixture.store
                                                        .reserve(
                                                                actorRequest(
                                                                        "independent-member",
                                                                        fixture.newDescriptor,
                                                                        fixture.newOwner,
                                                                        new byte[] {1}),
                                                                () -> false)
                                                        .toCompletableFuture()
                                                        .join())
                                        .reservation());

        var spot = reserveNewSpot(fixture);

        assertNotNull(recreated.get());
        assertTrue(spot.objectGeneration() > fixture.reservation.objectGeneration());
        var member = read(fixture, oldMember.authorityKey());
        assertTrue(member.objectGeneration() > oldMember.objectGeneration());
        assertEquals(recreated.get().objectGeneration(), member.objectGeneration());
        assertEquals(recreated.get().storeVersion(), member.storeVersion());
        assertEquals(fixture.newOwner.ownerId(), member.ownerId());
        assertEquals(fixture.newOwner.leaseGeneration(), member.ownerLeaseGeneration());
        assertEquals(0, oldSlots(fixture, true));
        assertEquals(0, oldSpotSlots(fixture));
    }

    @Test
    void unchangedMemberWithInconsistentCapacityKeepsAccountingError() throws Exception {
        var provider = new ScanBoundaryProvider(false, false);
        var fixture =
                fixture(
                        new ZLinkProviderLocationRepository(provider),
                        ZLinkPlacementObjectKind.USER_SPOT,
                        ZLinkServiceAuthorityPayloadCodec.State.READY);
        var memberOwner =
                assertInstanceOf(
                                ZLinkOwnerLeaseClaimed.class,
                                fixture.store
                                        .claimOwnerLease("member", Duration.ofMinutes(5))
                                        .toCompletableFuture()
                                        .join())
                        .token();
        var memberDescriptor = descriptor(memberOwner, fixture.kind, 1);
        fixture.store
                .updateMeshNode(memberDescriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture()
                .join();
        var memberFixture =
                new Fixture(
                        fixture.store,
                        fixture.kind,
                        fixture.key,
                        memberOwner,
                        fixture.newOwner,
                        memberDescriptor,
                        fixture.newDescriptor,
                        fixture.reservation);
        var member = endedMember(memberFixture, "inconsistent-member", true);
        var spotVersion = read(fixture, fixture.key).storeVersion();
        var memberVersion = read(fixture, member.authorityKey()).storeVersion();
        var mapper = new com.fasterxml.jackson.databind.ObjectMapper();
        var page =
                assertInstanceOf(
                                ZLinkStoreScanPageResult.class,
                                provider.delegate
                                        .scan(
                                                new ZLinkStoreScanRequest("", null, 1000),
                                                () -> false)
                                        .toCompletableFuture()
                                        .join())
                        .value();
        boolean changed = false;
        for (var item : page.items()) {
            var record = mapper.readTree(item.value().bytes());
            if (record.path("active").path("actors").asLong() != 1
                    || record.path("active").path("spots").asLong() != 0) continue;
            ((com.fasterxml.jackson.databind.node.ObjectNode) record.path("active"))
                    .put("actors", 0);
            assertInstanceOf(
                    ZLinkStoreWriteApplied.class,
                    provider.delegate
                            .write(
                                    new ZLinkStoreWriteRequest(
                                            List.of(
                                                    new ZLinkStoreVersionCondition(
                                                            item.key(), item.value().version())),
                                            List.of(
                                                    new ZLinkStorePut(
                                                            item.key(),
                                                            mapper.writeValueAsBytes(record),
                                                            null))),
                                    () -> false)
                            .toCompletableFuture()
                            .join());
            changed = true;
        }
        assertTrue(changed);
        fixture.store.releaseOwnerLease(fixture.oldOwner).toCompletableFuture().join();
        fixture.store.releaseOwnerLease(memberOwner).toCompletableFuture().join();

        var failure = assertThrows(CompletionException.class, () -> reserveNewSpot(fixture));

        var accounting = assertInstanceOf(IllegalStateException.class, failure.getCause());
        assertEquals("Location Store capacity record is inconsistent", accounting.getMessage());
        assertEquals(spotVersion, read(fixture, fixture.key).storeVersion());
        assertEquals(memberVersion, read(fixture, member.authorityKey()).storeVersion());
    }

    @ParameterizedTest
    @CsvSource({"false", "true"})
    void endedOwnerMemberAfterFirstPageOrExpiredSnapshotIsReleased(boolean expire) {
        var provider = new ScanBoundaryProvider(expire, false);
        var fixture =
                fixture(
                        new ZLinkProviderLocationRepository(provider),
                        ZLinkPlacementObjectKind.USER_SPOT,
                        ZLinkServiceAuthorityPayloadCodec.State.READY,
                        1);
        var dead = endedMember(fixture, "dead-member", true);
        fixture.store.releaseOwnerLease(fixture.oldOwner).toCompletableFuture().join();
        provider.enabled = true;
        reserveNewSpot(fixture);
        assertEquals(expire ? 3 : 2, provider.scans);
        assertInstanceOf(
                ZLinkAuthorityMissing.class,
                fixture.store.read(dead.authorityKey(), () -> false).toCompletableFuture().join());
    }

    @ParameterizedTest
    @CsvSource({
        "provider, moving",
        "memory, moving",
        "provider, aggregate",
        "memory, aggregate",
        "provider, unactivated",
        "memory, unactivated"
    })
    void movingOrUnactivatedMemberKeepsEndedSpot(String backend, String scenario) throws Exception {
        var fixture =
                fixture(
                        backend,
                        ZLinkPlacementObjectKind.USER_SPOT,
                        ZLinkServiceAuthorityPayloadCodec.State.READY,
                        2);
        var settled = endedMember(fixture, "settled-member", true);
        String memberKey;
        switch (scenario) {
            case "moving" -> {
                var moving = endedMember(fixture, "moving-member", true);
                memberKey = moving.authorityKey();
                var before = read(fixture, memberKey);
                var relocation =
                        new ZLinkAggregateRelocationCoordinator.Request(
                                new java.util.UUID(0, 9),
                                1,
                                2,
                                List.of(
                                        new ZLinkAggregateRelocationCoordinator.Participant(
                                                memberKey,
                                                ZLinkPlacementObjectKind.ACTOR,
                                                before.objectGeneration(),
                                                before.authorityOwnerGeneration(),
                                                before.storeVersion(),
                                                ZLinkAuthorityGenerationTransition.PRESERVE,
                                                before.payload(),
                                                new byte[0])),
                                relocationRoot(),
                                new ZLinkMeshNodeDescriptorKey("mesh", fixture.oldDescriptor.rid()),
                                1,
                                ZLinkPlacementCapacityBundle.actor(0),
                                fixture.oldOwner,
                                "1");
                var publish =
                        ZLinkCanonicalRelocationAuthorityStateCodec.class.getDeclaredMethod(
                                "publish",
                                byte[].class,
                                ZLinkAggregateRelocationCoordinator.Request.class,
                                ZLinkAuthorityGenerationTransition.class);
                publish.setAccessible(true);
                assertInstanceOf(
                        ZLinkAuthorityStored.class,
                        fixture.store
                                .compareExchange(
                                        memberKey,
                                        new ZLinkAuthorityExpectFound(before.storeVersion()),
                                        new ZLinkAuthorityPut(
                                                (byte[])
                                                        publish.invoke(
                                                                null,
                                                                before.payload(),
                                                                relocation,
                                                                ZLinkAuthorityGenerationTransition
                                                                        .PRESERVE)),
                                        () -> false)
                                .toCompletableFuture()
                                .join());
            }
            case "aggregate" -> {
                memberKey = settled.authorityKey();
                var before = read(fixture, memberKey);
                assertInstanceOf(
                        ZLinkAggregatePrepared.class,
                        fixture.store
                                .prepareAggregate(
                                        new ZLinkAggregatePrepareRequest(
                                                new java.util.UUID(0, 22),
                                                1,
                                                List.of(
                                                        new ZLinkAggregateParticipant(
                                                                memberKey,
                                                                before.objectGeneration(),
                                                                before.authorityOwnerGeneration(),
                                                                before.storeVersion(),
                                                                ZLinkAuthorityGenerationTransition
                                                                        .PRESERVE,
                                                                before.payload(),
                                                                new byte[0])),
                                                new byte[32],
                                                new ZLinkMeshNodeDescriptorKey(
                                                        "mesh", fixture.newDescriptor.rid()),
                                                1,
                                                ZLinkPlacementCapacityBundle.actor(0),
                                                fixture.newOwner),
                                        () -> false)
                                .toCompletableFuture()
                                .join());
            }
            default -> memberKey = endedMember(fixture, "unactivated-member", false).authorityKey();
        }
        var memberBefore = read(fixture, memberKey).storeVersion();
        var settledBefore = read(fixture, settled.authorityKey()).storeVersion();
        fixture.store.releaseOwnerLease(fixture.oldOwner).toCompletableFuture().join();
        assertUnavailableAndUnchanged(fixture, new RelocationPolicy.Disabled());
        assertEquals(memberBefore, read(fixture, memberKey).storeVersion());
        assertEquals(settledBefore, read(fixture, settled.authorityKey()).storeVersion());
    }

    @ParameterizedTest
    @CsvSource({"provider, 2044", "memory, 2044", "provider, 2045", "memory, 2045"})
    void endedMemberReleaseIsBoundedByOneProviderBatch(String backend, int members) {
        // Batch keys: Spot authority, owner lease, descriptor and capacity plus one authority
        // per member (all members share the Spot's owner and node). 2,044 members fill the
        // 2,048-key bound exactly.
        var fixture =
                fixture(
                        backend,
                        ZLinkPlacementObjectKind.USER_SPOT,
                        ZLinkServiceAuthorityPayloadCodec.State.READY,
                        members);
        List<String> keys = new java.util.ArrayList<>();
        for (int index = 0; index < members; index++)
            keys.add(endedMember(fixture, "bulk-" + index, true).authorityKey());
        fixture.store.releaseOwnerLease(fixture.oldOwner).toCompletableFuture().join();
        if (members <= 2044) {
            reserveNewSpot(fixture);
            for (String key : keys)
                assertInstanceOf(
                        ZLinkAuthorityMissing.class,
                        fixture.store.read(key, () -> false).toCompletableFuture().join());
            return;
        }
        var first = read(fixture, keys.get(0)).storeVersion();
        assertUnavailableAndUnchanged(fixture, new RelocationPolicy.Disabled());
        assertEquals(first, read(fixture, keys.get(0)).storeVersion());
    }

    private static ZLinkAuthoritySnapshot read(Fixture fixture, String key) {
        return (ZLinkAuthoritySnapshot)
                fixture.store.read(key, () -> false).toCompletableFuture().join();
    }

    private static byte[] relocationRoot() throws java.io.IOException {
        var fixtureJson =
                new com.fasterxml.jackson.databind.ObjectMapper()
                        .readTree(
                                java.nio.file.Files.readString(
                                        java.nio.file.Path.of(
                                                "../../../runtime/protocol/golden/relocation-envelope-v1.json")));
        return java.util.HexFormat.of().parseHex(fixtureJson.findValue("logicalHex").asText());
    }

    private static ZLinkObjectReservation reserveNewSpot(Fixture fixture) {
        return assertInstanceOf(
                        ZLinkObjectReserved.class,
                        fixture.store
                                .reserve(
                                        request(
                                                fixture,
                                                fixture.newOwner,
                                                fixture.newDescriptor,
                                                new RelocationPolicy.Disabled()),
                                        () -> false)
                                .toCompletableFuture()
                                .join())
                .reservation();
    }

    private static ZLinkObjectReservationRequest actorRequest(
            String id,
            ZLinkMeshNodeDescriptor descriptor,
            ZLinkLocationOwnerToken owner,
            byte[] creatingPayload) {
        return new ZLinkObjectReservationRequest(
                ZLinkPlacementObjectKind.ACTOR,
                ZLinkAuthorityKeyCodec.actor(id),
                "player",
                "intent",
                new byte[32],
                1,
                new ZLinkMeshNodeDescriptorKey("mesh", descriptor.rid()),
                1,
                owner,
                creatingPayload,
                ZLinkPlacementCapacityBundle.actor(1),
                new RelocationPolicy.Disabled());
    }

    /** A member of the ended Spot owned by the Spot's (later ended) owner on the old node. */
    private static ZLinkObjectReservation endedMember(
            Fixture fixture, String id, boolean activated) {
        var actor =
                ((ZLinkObjectReserved)
                                fixture.store
                                        .reserve(
                                                actorRequest(
                                                        id,
                                                        fixture.oldDescriptor,
                                                        fixture.oldOwner,
                                                        activated
                                                                ? new byte[] {1}
                                                                : endedMemberPayload(
                                                                        fixture,
                                                                        id,
                                                                        ZLinkActorAuthorityPayloadCodec
                                                                                .State.CREATING)),
                                                () -> false)
                                        .toCompletableFuture()
                                        .join())
                        .reservation();
        if (activated)
            assertEquals(
                    ZLinkObjectCommitResult.COMMITTED,
                    fixture.store
                            .commit(
                                    actor,
                                    endedMemberPayload(
                                            fixture,
                                            id,
                                            ZLinkActorAuthorityPayloadCodec.State.READY),
                                    () -> false)
                            .toCompletableFuture()
                            .join());
        return actor;
    }

    private static byte[] endedMemberPayload(
            Fixture fixture, String id, ZLinkActorAuthorityPayloadCodec.State state) {
        return new ZLinkActorAuthorityPayloadCodec()
                .encode(
                        state,
                        "player",
                        id,
                        "ended-spot",
                        fixture.reservation.objectGeneration(),
                        ZLinkSpotKind.USER.value(),
                        fixture.oldOwner.ownerId(),
                        fixture.oldOwner.leaseGeneration(),
                        "mesh",
                        fixture.oldDescriptor.rid(),
                        1);
    }

    private static ZLinkObjectReservation member(Fixture fixture, String id, boolean joined) {
        var actor =
                ((ZLinkObjectReserved)
                                fixture.store
                                        .reserve(
                                                new ZLinkObjectReservationRequest(
                                                        ZLinkPlacementObjectKind.ACTOR,
                                                        ZLinkAuthorityKeyCodec.actor(id),
                                                        "player",
                                                        "intent",
                                                        new byte[32],
                                                        1,
                                                        new ZLinkMeshNodeDescriptorKey(
                                                                "mesh",
                                                                fixture.newDescriptor.rid()),
                                                        1,
                                                        fixture.newOwner,
                                                        new byte[] {1},
                                                        ZLinkPlacementCapacityBundle.actor(1),
                                                        new RelocationPolicy.Disabled()),
                                                () -> false)
                                        .toCompletableFuture()
                                        .join())
                        .reservation();
        assertEquals(
                ZLinkObjectCommitResult.COMMITTED,
                fixture.store
                        .commit(actor, memberPayload(fixture, id, joined), () -> false)
                        .toCompletableFuture()
                        .join());
        return actor;
    }

    @ParameterizedTest
    @CsvSource({"provider, true", "memory, true", "provider, false", "memory, false"})
    void relocationJoinCommitChecksTargetSpotLease(String backend, boolean live) {
        var fixture =
                fixture(
                        backend,
                        ZLinkPlacementObjectKind.USER_SPOT,
                        ZLinkServiceAuthorityPayloadCodec.State.READY);
        var actor = member(fixture, "relocation", false);
        var snapshot =
                (ZLinkAuthoritySnapshot)
                        fixture.store
                                .read(actor.authorityKey(), () -> false)
                                .toCompletableFuture()
                                .join();
        var request =
                new ZLinkAggregatePrepareRequest(
                        new java.util.UUID(0, 12),
                        1,
                        List.of(
                                new ZLinkAggregateParticipant(
                                        actor.authorityKey(),
                                        snapshot.objectGeneration(),
                                        snapshot.authorityOwnerGeneration(),
                                        snapshot.storeVersion(),
                                        ZLinkAuthorityGenerationTransition.PRESERVE,
                                        memberPayload(fixture, "relocation", true),
                                        new byte[0])),
                        new byte[32],
                        new ZLinkMeshNodeDescriptorKey("mesh", fixture.newDescriptor.rid()),
                        1,
                        ZLinkPlacementCapacityBundle.actor(0),
                        fixture.newOwner);
        var prepared =
                assertInstanceOf(
                        ZLinkAggregatePrepared.class,
                        fixture.store
                                .prepareAggregate(request, () -> false)
                                .toCompletableFuture()
                                .join());
        if (!live) fixture.store.releaseOwnerLease(fixture.oldOwner).toCompletableFuture().join();
        assertEquals(
                live ? ZLinkAggregateCommitResult.COMMITTED : ZLinkAggregateCommitResult.STALE,
                fixture.store
                        .commitAggregate(prepared.fence(), () -> false)
                        .toCompletableFuture()
                        .join());
    }

    @ParameterizedTest
    @CsvSource({
        "provider, USER_SPOT",
        "provider, INSTANCE_SPOT",
        "memory, USER_SPOT",
        "memory, INSTANCE_SPOT"
    })
    void aggregateSpotDoesNotPermitEndedOwnerRecreation(
            String backend, ZLinkPlacementObjectKind kind) {
        var fixture = fixture(backend, kind, ZLinkServiceAuthorityPayloadCodec.State.READY);
        var before =
                (ZLinkAuthoritySnapshot)
                        fixture.store.read(fixture.key, () -> false).toCompletableFuture().join();
        var request =
                new ZLinkAggregatePrepareRequest(
                        new java.util.UUID(0, 13),
                        1,
                        List.of(
                                new ZLinkAggregateParticipant(
                                        fixture.key,
                                        before.objectGeneration(),
                                        before.authorityOwnerGeneration(),
                                        before.storeVersion(),
                                        ZLinkAuthorityGenerationTransition.PRESERVE,
                                        before.payload(),
                                        new byte[0])),
                        new byte[32],
                        new ZLinkMeshNodeDescriptorKey("mesh", fixture.newDescriptor.rid()),
                        1,
                        ZLinkPlacementCapacityBundle.actor(0),
                        fixture.newOwner);
        assertInstanceOf(
                ZLinkAggregatePrepared.class,
                fixture.store.prepareAggregate(request, () -> false).toCompletableFuture().join());
        fixture.store.releaseOwnerLease(fixture.oldOwner).toCompletableFuture().join();
        assertUnavailableAndUnchanged(fixture, new RelocationPolicy.Disabled());
    }

    @ParameterizedTest
    @CsvSource({
        "provider, USER_SPOT",
        "provider, INSTANCE_SPOT",
        "memory, USER_SPOT",
        "memory, INSTANCE_SPOT"
    })
    void committedAggregateUsesCurrentOwnerForReservationAndMembership(
            String backend, ZLinkPlacementObjectKind kind) {
        var fixture = fixture(backend, kind, ZLinkServiceAuthorityPayloadCodec.State.READY);
        var before =
                (ZLinkAuthoritySnapshot)
                        fixture.store.read(fixture.key, () -> false).toCompletableFuture().join();
        var codec = new ZLinkServiceAuthorityPayloadCodec();
        byte[] payload =
                kind == ZLinkPlacementObjectKind.USER_SPOT
                        ? codec.encodeUser(
                                ZLinkServiceAuthorityPayloadCodec.State.READY,
                                "room",
                                "ended-spot",
                                fixture.newOwner.ownerId(),
                                fixture.newOwner.leaseGeneration(),
                                "mesh",
                                fixture.newDescriptor.rid(),
                                1)
                        : codec.encodeInstance(
                                ZLinkServiceAuthorityPayloadCodec.State.READY,
                                "room",
                                "ended-spot",
                                fixture.newOwner.ownerId(),
                                fixture.newOwner.leaseGeneration(),
                                "mesh",
                                fixture.newDescriptor.rid(),
                                1);
        var request =
                new ZLinkAggregatePrepareRequest(
                        new java.util.UUID(0, 14),
                        1,
                        List.of(
                                new ZLinkAggregateParticipant(
                                        fixture.key,
                                        before.objectGeneration(),
                                        before.authorityOwnerGeneration(),
                                        before.storeVersion(),
                                        ZLinkAuthorityGenerationTransition.NEW_OWNER,
                                        payload,
                                        new byte[0])),
                        new byte[32],
                        new ZLinkMeshNodeDescriptorKey("mesh", fixture.newDescriptor.rid()),
                        1,
                        ZLinkPlacementCapacityBundle.spot(kind, "room", 1),
                        fixture.newOwner);
        var prepared =
                assertInstanceOf(
                        ZLinkAggregatePrepared.class,
                        fixture.store
                                .prepareAggregate(request, () -> false)
                                .toCompletableFuture()
                                .join());
        assertEquals(
                ZLinkAggregateCommitResult.COMMITTED,
                fixture.store
                        .commitAggregate(prepared.fence(), () -> false)
                        .toCompletableFuture()
                        .join());
        fixture.store.releaseOwnerLease(fixture.oldOwner).toCompletableFuture().join();
        assertInstanceOf(
                ZLinkObjectAlreadyExists.class,
                fixture.store
                        .reserve(
                                request(
                                        fixture,
                                        fixture.newOwner,
                                        fixture.newDescriptor,
                                        new RelocationPolicy.Disabled()),
                                () -> false)
                        .toCompletableFuture()
                        .join());
        if (kind == ZLinkPlacementObjectKind.USER_SPOT) member(fixture, "committed-target", true);
        assertEquals(
                before.objectGeneration(),
                ((ZLinkAuthoritySnapshot)
                                fixture.store
                                        .read(fixture.key, () -> false)
                                        .toCompletableFuture()
                                        .join())
                        .objectGeneration());
    }

    @ParameterizedTest
    @CsvSource({
        "provider, USER_SPOT",
        "provider, INSTANCE_SPOT",
        "memory, USER_SPOT",
        "memory, INSTANCE_SPOT"
    })
    void relocationSpotDoesNotPermitEndedOwnerRecreation(
            String backend, ZLinkPlacementObjectKind kind) throws Exception {
        var fixture = fixture(backend, kind, ZLinkServiceAuthorityPayloadCodec.State.READY);
        var before =
                (ZLinkAuthoritySnapshot)
                        fixture.store.read(fixture.key, () -> false).toCompletableFuture().join();
        var fixtureJson =
                new com.fasterxml.jackson.databind.ObjectMapper()
                        .readTree(
                                java.nio.file.Files.readString(
                                        java.nio.file.Path.of(
                                                "../../../runtime/protocol/golden/relocation-envelope-v1.json")));
        byte[] root =
                java.util.HexFormat.of().parseHex(fixtureJson.findValue("logicalHex").asText());
        var relocation =
                new ZLinkAggregateRelocationCoordinator.Request(
                        new java.util.UUID(0, 9),
                        1,
                        2,
                        List.of(
                                new ZLinkAggregateRelocationCoordinator.Participant(
                                        fixture.key,
                                        kind,
                                        before.objectGeneration(),
                                        before.authorityOwnerGeneration(),
                                        before.storeVersion(),
                                        ZLinkAuthorityGenerationTransition.PRESERVE,
                                        before.payload(),
                                        new byte[0])),
                        root,
                        new ZLinkMeshNodeDescriptorKey("mesh", fixture.oldDescriptor.rid()),
                        1,
                        ZLinkPlacementCapacityBundle.actor(0),
                        fixture.oldOwner,
                        "1");
        var publish =
                ZLinkCanonicalRelocationAuthorityStateCodec.class.getDeclaredMethod(
                        "publish",
                        byte[].class,
                        ZLinkAggregateRelocationCoordinator.Request.class,
                        ZLinkAuthorityGenerationTransition.class);
        publish.setAccessible(true);
        byte[] payload =
                (byte[])
                        publish.invoke(
                                null,
                                before.payload(),
                                relocation,
                                ZLinkAuthorityGenerationTransition.PRESERVE);
        assertInstanceOf(
                ZLinkAuthorityStored.class,
                fixture.store
                        .compareExchange(
                                fixture.key,
                                new ZLinkAuthorityExpectFound(before.storeVersion()),
                                new ZLinkAuthorityPut(payload),
                                () -> false)
                        .toCompletableFuture()
                        .join());
        fixture.store.releaseOwnerLease(fixture.oldOwner).toCompletableFuture().join();
        assertUnavailableAndUnchanged(fixture, new RelocationPolicy.Disabled());
    }

    private static byte[] memberPayload(Fixture fixture, String id, boolean joined) {
        return new ZLinkActorAuthorityPayloadCodec()
                .encode(
                        ZLinkActorAuthorityPayloadCodec.State.READY,
                        "player",
                        id,
                        joined ? "ended-spot" : "entry",
                        joined ? fixture.reservation.objectGeneration() : 1,
                        joined ? 2 : 1,
                        fixture.newOwner.ownerId(),
                        fixture.newOwner.leaseGeneration(),
                        "mesh",
                        fixture.newDescriptor.rid(),
                        1);
    }

    private static void assertUnavailableAndUnchanged(Fixture fixture, RelocationPolicy policy) {
        var before =
                (ZLinkAuthoritySnapshot)
                        fixture.store.read(fixture.key, () -> false).toCompletableFuture().join();
        var failure =
                assertThrows(
                        CompletionException.class,
                        () ->
                                java.util.concurrent.CompletableFuture.completedFuture(fixture)
                                        .thenCompose(
                                                ignored ->
                                                        fixture.store.reserve(
                                                                request(
                                                                        fixture,
                                                                        fixture.newOwner,
                                                                        fixture.newDescriptor,
                                                                        policy),
                                                                () -> false))
                                        .join());
        assertEquals(
                ZLinkFrameworkErrorKind.UNAVAILABLE,
                assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
        var after =
                (ZLinkAuthoritySnapshot)
                        fixture.store.read(fixture.key, () -> false).toCompletableFuture().join();
        assertEquals(before.storeVersion(), after.storeVersion());
        assertArrayEquals(before.payload(), after.payload());
    }

    private static Fixture fixture(
            String backend,
            ZLinkPlacementObjectKind kind,
            ZLinkServiceAuthorityPayloadCodec.State state) {
        return fixture(backend, kind, state, 1);
    }

    private static Fixture fixture(
            String backend,
            ZLinkPlacementObjectKind kind,
            ZLinkServiceAuthorityPayloadCodec.State state,
            int actorLimit) {
        ZLinkLocationRepository store =
                backend.equals("provider")
                        ? new ZLinkProviderLocationRepository(
                                new ZLinkInMemoryProviderLocationStore())
                        : new ZLinkInMemoryLocationStore();
        return fixture(store, kind, state, actorLimit);
    }

    private static Fixture fixture(
            ZLinkLocationRepository store,
            ZLinkPlacementObjectKind kind,
            ZLinkServiceAuthorityPayloadCodec.State state) {
        return fixture(store, kind, state, 1);
    }

    private static Fixture fixture(
            ZLinkLocationRepository store,
            ZLinkPlacementObjectKind kind,
            ZLinkServiceAuthorityPayloadCodec.State state,
            int actorLimit) {
        var oldOwner =
                ((ZLinkOwnerLeaseClaimed)
                                store.claimOwnerLease("old", Duration.ofMinutes(5))
                                        .toCompletableFuture()
                                        .join())
                        .token();
        var newOwner =
                ((ZLinkOwnerLeaseClaimed)
                                store.claimOwnerLease("new", Duration.ofMinutes(5))
                                        .toCompletableFuture()
                                        .join())
                        .token();
        var oldDescriptor = descriptor(oldOwner, kind, actorLimit);
        var newDescriptor = descriptor(newOwner, kind, 1);
        store.updateMeshNode(oldDescriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture()
                .join();
        store.updateMeshNode(newDescriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture()
                .join();
        String key = ZLinkAuthorityKeyCodec.spot("ended-spot");
        var initial =
                new Fixture(
                        store, kind, key, oldOwner, newOwner, oldDescriptor, newDescriptor, null);
        var reservation =
                ((ZLinkObjectReserved)
                                store.reserve(
                                                request(
                                                        initial,
                                                        oldOwner,
                                                        oldDescriptor,
                                                        new RelocationPolicy.Disabled()),
                                                () -> false)
                                        .toCompletableFuture()
                                        .join())
                        .reservation();
        var codec = new ZLinkServiceAuthorityPayloadCodec();
        byte[] payload =
                kind == ZLinkPlacementObjectKind.USER_SPOT
                        ? codec.encodeUser(
                                state,
                                "room",
                                "ended-spot",
                                oldOwner.ownerId(),
                                oldOwner.leaseGeneration(),
                                "mesh",
                                oldDescriptor.rid(),
                                1)
                        : codec.encodeInstance(
                                state,
                                "room",
                                "ended-spot",
                                oldOwner.ownerId(),
                                oldOwner.leaseGeneration(),
                                "mesh",
                                oldDescriptor.rid(),
                                1);
        assertEquals(
                ZLinkObjectCommitResult.COMMITTED,
                store.commit(reservation, payload, () -> false).toCompletableFuture().join());
        return new Fixture(
                store, kind, key, oldOwner, newOwner, oldDescriptor, newDescriptor, reservation);
    }

    private static ZLinkObjectReservationRequest request(
            Fixture fixture,
            ZLinkLocationOwnerToken owner,
            ZLinkMeshNodeDescriptor descriptor,
            RelocationPolicy policy) {
        return new ZLinkObjectReservationRequest(
                fixture.kind,
                fixture.key,
                "room",
                "intent",
                new byte[32],
                1,
                new ZLinkMeshNodeDescriptorKey("mesh", descriptor.rid()),
                1,
                owner,
                new byte[] {1},
                ZLinkPlacementCapacityBundle.spot(fixture.kind, "room", 1),
                policy);
    }

    private static ZLinkMeshNodeDescriptor descriptor(
            ZLinkLocationOwnerToken owner, ZLinkPlacementObjectKind kind, int actorLimit) {
        return new ZLinkMeshNodeDescriptor(
                "mesh",
                RoutingId.from(owner.ownerId()),
                1,
                1,
                "tcp://127.0.0.1:7100",
                Map.of(),
                1,
                List.of(
                        new ZLinkObjectCapability(
                                kind, "room", ZLinkObjectMaintenancePolicyKind.DISABLED, false, 1),
                        new ZLinkObjectCapability(
                                ZLinkPlacementObjectKind.ACTOR,
                                "player",
                                ZLinkObjectMaintenancePolicyKind.DISABLED,
                                false,
                                0)),
                ZLinkMeshNodeObjectRole.SERVER,
                Optional.of(owner.ownerId() + "-entry-00000000-0000-4000-8000-000000000001"),
                100,
                new ZLinkPlacementCapacity(
                        new ZLinkCapacityUsage(0, 0, actorLimit),
                        new ZLinkCapacityUsage(0, 0, 1),
                        List.of()),
                new ZLinkActivationConcurrency(0, 64),
                Optional.empty(),
                systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState.SERVING,
                "security",
                owner.ownerId(),
                owner.leaseGeneration(),
                Instant.now());
    }

    private record Fixture(
            ZLinkLocationRepository store,
            ZLinkPlacementObjectKind kind,
            String key,
            ZLinkLocationOwnerToken oldOwner,
            ZLinkLocationOwnerToken newOwner,
            ZLinkMeshNodeDescriptor oldDescriptor,
            ZLinkMeshNodeDescriptor newDescriptor,
            ZLinkObjectReservation reservation) {}
}
