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
        if (fixture.store instanceof ZLinkInMemoryLocationStore) {
            var field = ZLinkInMemoryLocationStore.class.getDeclaredField("authority");
            field.setAccessible(true);
            return ((ZLinkInMemoryAuthorityStore) field.get(fixture.store))
                    .kindCapacity(
                            new ZLinkMeshNodeDescriptorKey("mesh", fixture.oldDescriptor.rid()),
                            1,
                            false)[0];
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
                    .path("spots")
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
            if (!enabled) return delegate.scan(request, cancellation);
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
        ZLinkLocationRepository store =
                backend.equals("provider")
                        ? new ZLinkProviderLocationRepository(
                                new ZLinkInMemoryProviderLocationStore())
                        : new ZLinkInMemoryLocationStore();
        return fixture(store, kind, state);
    }

    private static Fixture fixture(
            ZLinkLocationRepository store,
            ZLinkPlacementObjectKind kind,
            ZLinkServiceAuthorityPayloadCodec.State state) {
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
        var oldDescriptor = descriptor(oldOwner, kind);
        var newDescriptor = descriptor(newOwner, kind);
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
            ZLinkLocationOwnerToken owner, ZLinkPlacementObjectKind kind) {
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
                        new ZLinkCapacityUsage(0, 0, 1),
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
