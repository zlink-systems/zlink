package systems.zlink.framework.runtime.internal.locations;

import static org.junit.jupiter.api.Assertions.*;

import static systems.zlink.framework.runtime.internal.locations.ZLinkRelocationCompletionRecordCodec.*;

import com.fasterxml.jackson.databind.ObjectMapper;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStorePut;
import systems.zlink.framework.locationprovider.ZLinkStoreReadResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreScanResult;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteConflict;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteResult;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryProviderLocationStore;
import systems.zlink.framework.testing.ZLinkLocationStoreTestAdapter;

import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;

final class ZLinkRelocationCompletionStoreTest {
    private static final String ID = ZLinkRelocationCompletionRecordCodecTest.ID;
    private static final String KEY = ZLinkAuthorityKeyCodec.actor("actor-1");
    private static final String DIGEST = "01".repeat(32);
    private static final ZLinkStoreCancellation NEVER = () -> false;
    private static final byte[] REPLY = {1, 2, 3};

    private final List<String> calls = new ArrayList<>();
    private final Authority authority = new Authority();
    private final Blobs blobs = new Blobs();
    private final ZLinkAggregateRelocationCoordinator coordinator =
            new ZLinkAggregateRelocationCoordinator(authority);

    @Test
    void putsVerifiesAndPublishesWithOneCas() {
        var before = authority.row;
        var after = publish(before);
        assertEquals(List.of("put", "get", "put", "get", "cas"), calls);
        assertEquals(after.storeVersion(), authority.row.storeVersion());
        assertNotEquals(before.storeVersion(), after.storeVersion());
        assertArrayEquals(before.payload(), after.payload());
        assertTrue(after.relocation().json().path("completion").path("extension").asBoolean());
        var progress = after.relocation().completion();
        assertEquals(2, progress.acceptedRequests());
        assertEquals(1, progress.completedResults());
        assertEquals(1, progress.pendingDeliveries());
        assertEquals(sha256(encode(record())), progress.sha256());
        var stored = read(after);
        assertArrayEquals(REPLY, stored.entries().getFirst().reply());
        assertArrayEquals(encode(record()), encode(stored));

        var acknowledged =
                new ZLinkRelocationCompletionRecordCodec.Record(
                        ID, List.of(entry(1, "alreadyTerminal")));
        var next =
                coordinator
                        .publishCompletion(
                                KEY,
                                after,
                                blobs,
                                acknowledged,
                                List.of(entry(2, "pending")),
                                NEVER)
                        .toCompletableFuture()
                        .join();
        assertNotEquals(progress.reference(), next.relocation().completion().reference());
        assertEquals(0, next.relocation().completion().pendingDeliveries());
        assertEquals("pending", read(after).entries().getFirst().delivery());
        assertEquals("alreadyTerminal", read(next).entries().getFirst().delivery());
    }

    @ParameterizedTest
    @ValueSource(ints = {1, 2, 3, 4})
    void blobIoFailureCannotPublishAuthority(int failedOperation) {
        var before = authority.row;
        blobs.failOn = failedOperation;
        assertSame(
                blobs.failure,
                assertThrows(CompletionException.class, () -> publish(before)).getCause());
        assertSame(before, authority.row);
        assertEquals(List.of("put", "get", "put", "get").subList(0, failedOperation), calls);
    }

    @ParameterizedTest
    @ValueSource(ints = {2, 4})
    void blobReadBackWithDifferentBytesCannotPublishAuthority(int damagedOperation) {
        var before = authority.row;
        blobs.corruptOn = damagedOperation;
        assertDataLost(() -> publish(before));
        assertSame(before, authority.row);
        assertEquals(List.of("put", "get", "put", "get").subList(0, damagedOperation), calls);
    }

    @Test
    void staleCasCannotReplacePublishedReference() {
        var stale = authority.row;
        var current = publish(stale);
        calls.clear();
        assertInstanceOf(
                ZLinkAggregateRelocationCoordinator.AuthorityConflictException.class,
                assertThrows(CompletionException.class, () -> publish(stale)).getCause());
        assertEquals(current.storeVersion(), authority.row.storeVersion());
        assertEquals(List.of("put", "get", "put", "get", "cas"), calls);
    }

    @Test
    void lostCasResponseIsResolvedByReadingThePublishedReference() {
        authority.dropReply = true;
        assertNull(authority.row.relocation().completion().reference());
        var after = publish(authority.row);
        assertSame(authority.row, after);
        assertEquals(List.of("put", "get", "put", "get", "cas", "read", "get", "get"), calls);
    }

    @Test
    void lostCasWithoutApplicationDoesNotConfirmSuccess() {
        var before = authority.row;
        authority.dropBeforeApply = true;
        assertSame(
                authority.failure,
                assertThrows(CompletionException.class, () -> publish(before)).getCause());
        assertSame(before, authority.row);
        assertEquals(List.of("put", "get", "put", "get", "cas", "read"), calls);
    }

    @Test
    void lostCasAndReadKeepThePublicationUncertain() {
        authority.dropReply = true;
        authority.dropRead = true;
        var failure =
                assertInstanceOf(
                        ZLinkAggregateRelocationCoordinator.CompletionPublicationUnknownException
                                .class,
                        assertThrows(CompletionException.class, () -> publish(authority.row))
                                .getCause());
        assertEquals(authority.row.relocation().completion().reference(), failure.reference());
        assertEquals(List.of("put", "get", "put", "get", "cas", "read"), calls);
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void providerWritesTheAuthorityOnce(boolean loseResponseAfterApply) {
        var delegate = new ZLinkInMemoryProviderLocationStore();
        var owner =
                assertInstanceOf(
                                ZLinkOwnerLeaseClaimed.class,
                                new ZLinkProviderOwnerLeaseRepository(delegate)
                                        .claim("target", Duration.ofMinutes(1))
                                        .toCompletableFuture()
                                        .join())
                        .token();
        var before = initialAuthority();
        var value =
                new ZLinkProviderAuthorityRepository.AuthorityRecord(
                        before.payload(),
                        before.objectGeneration(),
                        before.authorityOwnerGeneration(),
                        owner.ownerId(),
                        owner.leaseGeneration(),
                        before.allocation(),
                        before.pendingCreation(),
                        null,
                        null,
                        before.relocation());
        var rowKey = new ZLinkStoreKey("authority\0actor\0actor-1");
        delegate.write(
                        new ZLinkStoreWriteRequest(
                                List.of(),
                                List.of(
                                        new ZLinkStorePut(
                                                rowKey,
                                                ZLinkProviderAuthorityRepository.encode(value),
                                                null))),
                        NEVER::isCancellationRequested)
                .toCompletableFuture()
                .join();

        var writes = new java.util.concurrent.atomic.AtomicInteger();
        ZLinkLocationStore provider =
                new ZLinkLocationStore() {
                    @Override
                    public CompletionStage<ZLinkStoreReadResult> read(
                            ZLinkStoreKey key,
                            systems.zlink.framework.locationprovider.ZLinkStoreCancellation
                                    cancellation) {
                        return delegate.read(key, cancellation);
                    }

                    @Override
                    public CompletionStage<ZLinkStoreWriteResult> write(
                            ZLinkStoreWriteRequest request,
                            systems.zlink.framework.locationprovider.ZLinkStoreCancellation
                                    cancellation) {
                        if (writes.incrementAndGet() != 1)
                            return delegate.write(request, cancellation);
                        if (!loseResponseAfterApply) {
                            return CompletableFuture.completedFuture(
                                    new ZLinkStoreWriteConflict(Instant.EPOCH));
                        }
                        return delegate.write(request, cancellation)
                                .thenCompose(
                                        result ->
                                                CompletableFuture.failedFuture(authority.failure));
                    }

                    @Override
                    public CompletionStage<ZLinkStoreScanResult> scan(
                            ZLinkStoreScanRequest request,
                            systems.zlink.framework.locationprovider.ZLinkStoreCancellation
                                    cancellation) {
                        return delegate.scan(request, cancellation);
                    }
                };
        var repository = new ZLinkProviderLocationRepository(provider);
        var coordinator = new ZLinkAggregateRelocationCoordinator(repository);
        var expected =
                assertInstanceOf(
                        ZLinkAuthoritySnapshot.class,
                        repository.read(KEY, NEVER).toCompletableFuture().join());
        var publication =
                coordinator
                        .publishCompletion(
                                KEY, expected, blobs, record(), List.of(entry(2, "pending")), NEVER)
                        .toCompletableFuture();
        if (loseResponseAfterApply) {
            assertNotNull(publication.join().relocation().completion().reference());
        } else {
            assertInstanceOf(
                    ZLinkAggregateRelocationCoordinator.AuthorityConflictException.class,
                    assertThrows(CompletionException.class, publication::join).getCause());
            var current =
                    assertInstanceOf(
                            ZLinkAuthoritySnapshot.class,
                            repository.read(KEY, NEVER).toCompletableFuture().join());
            assertEquals(expected.storeVersion(), current.storeVersion());
            assertNull(current.relocation().completion().reference());
        }
        assertEquals(1, writes.get());
    }

    @Test
    void authorityCountsMustMatchTheStoredEntries() {
        var after = publish(authority.row);
        var progress = after.relocation().completion();
        for (var mismatched :
                List.of(
                        new ZLinkAuthorityRelocation.Completion(
                                2, 2, 1, progress.reference(), progress.sha256()),
                        new ZLinkAuthorityRelocation.Completion(
                                2, 1, 0, progress.reference(), progress.sha256()),
                        new ZLinkAuthorityRelocation.Completion(2, 1, 1, null, null))) {
            assertDataLost(
                    () ->
                            read(
                                    withRelocation(
                                            after,
                                            after.relocation().withCompletion(mismatched),
                                            after.storeVersion())));
        }
        var before = authority.row;
        assertDataLost(
                () ->
                        coordinator
                                .publishCompletion(KEY, before, blobs, record(), List.of(), NEVER)
                                .toCompletableFuture()
                                .join());
        assertSame(before, authority.row);
    }

    @Test
    void includesPendingTransitionsAndMissingFailureRepliesAtTheFinalSizeBoundary() {
        var remaining = List.of(entry(2, "pending"));
        var finalRecord =
                new ZLinkRelocationCompletionRecordCodec.Record(
                        ID,
                        List.of(entry(1, "sourceLeaseExpired"), entry(2, "sourceLeaseExpired")));
        long exactSize = encode(finalRecord).length;
        assertEquals(exactSize, finalSize(record(), remaining));
        assertDoesNotThrow(
                () ->
                        ZLinkAggregateRelocationCoordinator.requireCompletionFits(
                                record(), remaining, exactSize));
        assertEquals(
                ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
                assertThrows(
                                ZLinkFrameworkException.class,
                                () ->
                                        ZLinkAggregateRelocationCoordinator.requireCompletionFits(
                                                record(), remaining, exactSize - 1))
                        .kind());
    }

    private ZLinkAuthoritySnapshot publish(ZLinkAuthoritySnapshot expected) {
        return coordinator
                .publishCompletion(
                        KEY, expected, blobs, record(), List.of(entry(2, "pending")), NEVER)
                .toCompletableFuture()
                .join();
    }

    private ZLinkRelocationCompletionRecordCodec.Record read(ZLinkAuthoritySnapshot snapshot) {
        return coordinator.readCompletion(snapshot, blobs, NEVER).toCompletableFuture().join();
    }

    private static void assertDataLost(org.junit.jupiter.api.function.Executable operation) {
        assertInstanceOf(
                ZLinkAggregateRelocationCoordinator.RelocationDataLostException.class,
                assertThrows(CompletionException.class, operation).getCause());
    }

    private static ZLinkRelocationCompletionRecordCodec.Record record() {
        return new ZLinkRelocationCompletionRecordCodec.Record(ID, List.of(entry(1, "pending")));
    }

    private static Entry entry(int id, String delivery) {
        return new Entry(
                KEY,
                new Source("source-😀\u000f", "1", "ff", "2"),
                "%032x".formatted(id),
                Integer.toString(id),
                REPLY,
                delivery);
    }

    private static ZLinkAuthoritySnapshot withRelocation(
            ZLinkAuthoritySnapshot snapshot, ZLinkAuthorityRelocation relocation, String version) {
        return new ZLinkAuthoritySnapshot(
                version,
                snapshot.payload(),
                snapshot.objectGeneration(),
                snapshot.authorityOwnerGeneration(),
                snapshot.ownerId(),
                snapshot.ownerLeaseGeneration(),
                snapshot.allocation(),
                snapshot.pendingCreation(),
                Instant.EPOCH,
                relocation,
                snapshot.aggregateGeneration());
    }

    private static ZLinkAuthoritySnapshot initialAuthority() {
        try {
            var relocation =
                    new ZLinkAuthorityRelocation(
                            new ObjectMapper()
                                    .readTree(
                                            """
                                            {"relocationId":"%s","phase":"committed","applicationVersion":"1",
                                             "sourceInventorySha256":"%s",
                                             "source":{"nodeRid":"01","nodeGeneration":"1","ownerId":"source","ownerLeaseGeneration":"1"},
                                             "target":{"nodeRid":"02","nodeGeneration":"2","ownerId":"target","ownerLeaseGeneration":"2","attemptGeneration":"1"},
                                             "coordinator":{"nodeRid":"01","nodeGeneration":"1","ownerId":"source","ownerLeaseGeneration":"1","expectedStoreVersion":"source"},
                                             "completion":{"acceptedRequests":"2","completedResults":"0","pendingDeliveries":"0","reference":null,"sha256":null,"extension":true}}
                                            """
                                                    .formatted(ID, DIGEST)));
            var allocation =
                    new ZLinkPlacementAllocation(
                            ZLinkPlacementAllocationState.ACTIVE,
                            ZLinkPlacementObjectKind.ACTOR,
                            "actor",
                            new ZLinkMeshNodeDescriptorKey("mesh", RoutingId.from(new byte[] {2})),
                            2,
                            ZLinkPlacementCapacityBundle.actor(1));
            return new ZLinkAuthoritySnapshot(
                    "initial",
                    new byte[] {4, 5},
                    1,
                    2,
                    "target",
                    2,
                    allocation,
                    Optional.empty(),
                    Instant.EPOCH,
                    relocation,
                    1);
        } catch (java.io.IOException failure) {
            throw new AssertionError(failure);
        }
    }

    private final class Authority extends ZLinkLocationStoreTestAdapter {
        private final RuntimeException failure = new RuntimeException("CAS result unavailable");
        private ZLinkAuthoritySnapshot row = initialAuthority();
        private boolean dropReply;
        private boolean dropBeforeApply;
        private boolean dropRead;

        @Override
        public CompletionStage<ZLinkAuthorityReadResult> read(
                String key, ZLinkStoreCancellation cancellation) {
            calls.add("read");
            return dropRead
                    ? CompletableFuture.failedFuture(new RuntimeException("read unavailable"))
                    : CompletableFuture.completedFuture(row);
        }

        @Override
        public CompletionStage<ZLinkAuthorityWriteResult> compareExchange(
                String key,
                ZLinkAuthorityExpectation expectation,
                ZLinkAuthorityMutation mutation,
                ZLinkStoreCancellation cancellation) {
            calls.add("cas");
            assertEquals(KEY, key);
            if (dropBeforeApply) return CompletableFuture.failedFuture(failure);
            if (!row.storeVersion()
                    .equals(
                            assertInstanceOf(ZLinkAuthorityExpectFound.class, expectation)
                                    .storeVersion())) {
                return CompletableFuture.completedFuture(new ZLinkAuthorityConflict(row));
            }
            var put = assertInstanceOf(ZLinkAuthorityPut.class, mutation);
            assertArrayEquals(row.payload(), put.payload());
            row = withRelocation(row, put.relocation(), row.storeVersion() + "-next");
            if (dropReply) return CompletableFuture.failedFuture(failure);
            return CompletableFuture.completedFuture(
                    new ZLinkAuthorityStored(
                            row.storeVersion(),
                            row.payload(),
                            row.objectGeneration(),
                            row.authorityOwnerGeneration(),
                            row.ownerId(),
                            row.ownerLeaseGeneration(),
                            row.allocation(),
                            row.storeNow()));
        }
    }

    private final class Blobs implements ZLinkRelocationStore {
        private final ZLinkRelocationStore memory = new InMemoryRelocationStore();
        private final RuntimeException failure = new RuntimeException("blob operation failed");
        private int operations;
        private int failOn;
        private int corruptOn;

        @Override
        public CompletionStage<ZLinkRelocationStored> put(
                byte[] payload, Duration retention, ZLinkStoreCancellation cancellation) {
            calls.add("put");
            assertEquals(Duration.ofHours(24), retention);
            if (++operations == failOn) return CompletableFuture.failedFuture(failure);
            return memory.put(payload, retention, cancellation);
        }

        @Override
        public CompletionStage<ZLinkRelocationReadResult> get(
                String reference, ZLinkStoreCancellation cancellation) {
            calls.add("get");
            if (++operations == failOn) return CompletableFuture.failedFuture(failure);
            if (operations == corruptOn) {
                return CompletableFuture.completedFuture(new ZLinkRelocationFound(new byte[] {0}));
            }
            return memory.get(reference, cancellation);
        }

        @Override
        public CompletionStage<ZLinkRelocationRenewResult> renew(
                String reference, Duration retention, ZLinkStoreCancellation cancellation) {
            throw new AssertionError("Unexpected renewal");
        }

        @Override
        public CompletionStage<ZLinkRelocationDeleteResult> delete(
                String reference, ZLinkStoreCancellation cancellation) {
            throw new AssertionError("Unexpected deletion");
        }
    }
}
