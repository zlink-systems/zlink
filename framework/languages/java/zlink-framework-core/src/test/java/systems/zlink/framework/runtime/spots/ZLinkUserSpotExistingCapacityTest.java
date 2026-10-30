package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.configuration.ZLinkObjectFactoryRegistration.RelocationPolicy;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.internal.locations.ZLinkProviderLocationRepository;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.locations.ZLinkServiceAuthorityPayloadCodec;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotCreateState;

import java.time.Clock;
import java.time.Duration;
import java.time.Instant;
import java.time.ZoneId;
import java.time.ZoneOffset;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkUserSpotExistingCapacityTest {
    private static final AtomicInteger FACTORY_CALLS = new AtomicInteger();
    private static final AtomicReference<CompletableFuture<Void>> INITIALIZE_HOLD =
            new AtomicReference<>();
    private static final AtomicReference<CompletableFuture<Void>> INITIALIZE_ENTERED =
            new AtomicReference<>();

    @Test
    void existingReadySpotDoesNotRequireNewPlacementCapacity() throws Exception {
        FACTORY_CALLS.set(0);
        var store = new ZLinkInMemoryLocationStore();
        var options = options(store);
        try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            var manager = runtime.spotManager();
            var created =
                    manager.getOrCreate("room-one", "room")
                            .submit()
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS);
            assertEquals(ZLinkSpotCreateState.CREATED, created.state());
            var repository = new ZLinkProviderLocationRepository(store);
            var descriptor =
                    repository
                            .listMeshNodes("existing-capacity", new ZLinkPageRequest(10, null))
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS)
                            .items()
                            .getFirst();
            assertEquals(1, descriptor.capacity().spots().active());
            assertEquals(0, descriptor.capacity().spots().reserved());
            assertEquals(1, descriptor.capacity().spots().limit());
            var other = manager.getOrCreate("room-two", "room").submit().toCompletableFuture();
            var failure =
                    assertThrows(ExecutionException.class, () -> other.get(5, TimeUnit.SECONDS));
            assertEquals(
                    ZLinkFrameworkErrorKind.UNAVAILABLE,
                    assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
            var existing =
                    manager.getOrCreate("room-one", "room")
                            .submit()
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS);
            assertEquals(ZLinkSpotCreateState.EXISTING, existing.state());
            assertEquals(created.spot(), existing.spot());
            assertEquals(1, FACTORY_CALLS.get());
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void expiredReadyOwnerIsReleasedBeforeCapacityFailure(boolean capacityConditionConflict)
            throws Exception {
        FACTORY_CALLS.set(0);
        var clock = new MutableClock();
        var store = new ReleaseConflictStore(clock);
        try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options(store))) {
            runtime.spotManager()
                    .getOrCreate("occupied-room", "room")
                    .submit()
                    .toCompletableFuture()
                    .get(5, TimeUnit.SECONDS);
            var repository = new ZLinkProviderLocationRepository(store);
            var source =
                    repository
                            .listMeshNodes("existing-capacity", new ZLinkPageRequest(10, null))
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS)
                            .items()
                            .getFirst();
            assertEquals(1, source.capacity().spots().active());
            var oldOwner =
                    assertInstanceOf(
                                    ZLinkOwnerLeaseClaimed.class,
                                    repository
                                            .claimOwnerLease(
                                                    "expired-room-owner", Duration.ofSeconds(1))
                                            .toCompletableFuture()
                                            .get(5, TimeUnit.SECONDS))
                            .token();
            var oldDescriptor =
                    new ZLinkMeshNodeDescriptor(
                            source.meshName(),
                            RoutingId.from("expired-room-node"),
                            17,
                            1,
                            "inproc://expired-room-node",
                            source.channelWeights(),
                            source.applicationVersion(),
                            source.objectCapabilities(),
                            source.objectRole(),
                            Optional.of("expired-owner-entry"),
                            source.placementWeight(),
                            new ZLinkPlacementCapacity(
                                    new ZLinkCapacityUsage(
                                            0, 0, source.capacity().actors().limit()),
                                    new ZLinkCapacityUsage(0, 0, source.capacity().spots().limit()),
                                    source.capacity().spotTypes().stream()
                                            .map(
                                                    t ->
                                                            new ZLinkSpotTypeCapacity(
                                                                    t.objectKind(),
                                                                    t.stableType(),
                                                                    new ZLinkCapacityUsage(
                                                                            0,
                                                                            0,
                                                                            t.usage().limit())))
                                            .toList()),
                            source.activationConcurrency(),
                            source.maintenanceWave(),
                            source.state(),
                            source.securityIdentity(),
                            oldOwner.ownerId(),
                            oldOwner.leaseGeneration(),
                            source.updatedAt());
            assertEquals(
                    ZLinkLocationWriteStatus.STORED,
                    repository
                            .updateMeshNode(oldDescriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS)
                            .status());
            var codec = new ZLinkServiceAuthorityPayloadCodec();
            String key = ZLinkAuthorityKeyCodec.spot("expired-room");
            var reservation =
                    assertInstanceOf(
                                    ZLinkObjectReserved.class,
                                    repository
                                            .reserve(
                                                    new ZLinkObjectReservationRequest(
                                                            ZLinkPlacementObjectKind.USER_SPOT,
                                                            key,
                                                            "room",
                                                            "inline-v1:test",
                                                            new byte[32],
                                                            4,
                                                            new ZLinkMeshNodeDescriptorKey(
                                                                    oldDescriptor.meshName(),
                                                                    oldDescriptor.rid()),
                                                            oldDescriptor.lifecycleGeneration(),
                                                            oldOwner,
                                                            codec.encodeUser(
                                                                    ZLinkServiceAuthorityPayloadCodec
                                                                            .State.CREATING,
                                                                    "room",
                                                                    "expired-room",
                                                                    oldOwner.ownerId(),
                                                                    oldOwner.leaseGeneration(),
                                                                    oldDescriptor.meshName(),
                                                                    oldDescriptor.rid(),
                                                                    oldDescriptor
                                                                            .lifecycleGeneration()),
                                                            ZLinkPlacementCapacityBundle.spot(
                                                                    ZLinkPlacementObjectKind
                                                                            .USER_SPOT,
                                                                    "room",
                                                                    1),
                                                            new RelocationPolicy.Disabled()),
                                                    () -> false)
                                            .toCompletableFuture()
                                            .get(5, TimeUnit.SECONDS))
                            .reservation();
            assertEquals(
                    ZLinkObjectCommitResult.COMMITTED,
                    repository
                            .commit(
                                    reservation,
                                    codec.encodeUser(
                                            ZLinkServiceAuthorityPayloadCodec.State.READY,
                                            "room",
                                            "expired-room",
                                            oldOwner.ownerId(),
                                            oldOwner.leaseGeneration(),
                                            oldDescriptor.meshName(),
                                            oldDescriptor.rid(),
                                            oldDescriptor.lifecycleGeneration()),
                                    null,
                                    () -> false)
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS));
            clock.advance(Duration.ofSeconds(2));
            store.armed.set(capacityConditionConflict);
            var requested =
                    runtime.spotManager()
                            .getOrCreate("expired-room", "room")
                            .submit()
                            .toCompletableFuture();
            var failure =
                    assertThrows(
                            ExecutionException.class, () -> requested.get(5, TimeUnit.SECONDS));
            assertEquals(
                    ZLinkFrameworkErrorKind.UNAVAILABLE,
                    assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
            assertEquals(capacityConditionConflict ? 1 : 0, store.conflicts.get());
            if (capacityConditionConflict) {
                assertEquals(store.beforeVersion.get(), store.afterVersion.get());
            }
            assertInstanceOf(
                    ZLinkAuthorityMissing.class,
                    repository
                            .read(key, () -> false)
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS));
            var released =
                    repository
                            .listMeshNodes("existing-capacity", new ZLinkPageRequest(10, null))
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS)
                            .items()
                            .stream()
                            .filter(d -> d.rid().equals(oldDescriptor.rid()))
                            .findFirst()
                            .orElseThrow();
            assertEquals(0, released.capacity().spots().active());
            assertEquals(0, released.capacity().spots().reserved());
            assertEquals(1, FACTORY_CALLS.get());
        }
    }

    @ParameterizedTest
    @ValueSource(ints = {1, 2})
    void sameIdWinnerBetweenMissingObservationAndCapacitySelectionReturnsExisting(
            int holdMissingNumber) throws Exception {
        FACTORY_CALLS.set(0);
        var store = new MissingReadBarrier(holdMissingNumber);
        try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options(store))) {
            store.armed.set(true);
            var first =
                    runtime.spotManager()
                            .getOrCreate("raced-room", "room")
                            .submit()
                            .toCompletableFuture();
            try {
                store.observed.get(5, TimeUnit.SECONDS);
                var winner =
                        runtime.spotManager()
                                .getOrCreate("raced-room", "room")
                                .submit()
                                .toCompletableFuture()
                                .get(5, TimeUnit.SECONDS);
                assertEquals(ZLinkSpotCreateState.CREATED, winner.state());
                var descriptor =
                        new ZLinkProviderLocationRepository(store)
                                .listMeshNodes("existing-capacity", new ZLinkPageRequest(10, null))
                                .toCompletableFuture()
                                .get(5, TimeUnit.SECONDS)
                                .items()
                                .getFirst();
                assertEquals(1, descriptor.capacity().spots().active());
                store.release.complete(null);
                var existing = first.get(5, TimeUnit.SECONDS);
                assertEquals(ZLinkSpotCreateState.EXISTING, existing.state());
                assertEquals(winner.spot(), existing.spot());
                assertEquals(1, FACTORY_CALLS.get());
            } finally {
                store.release.complete(null);
            }
        }
    }

    @Test
    void failedCreatingWinnerReleasesCapacityForFreshSelection() throws Exception {
        FACTORY_CALLS.set(0);
        var store = new MissingReadBarrier(2);
        var hold = new CompletableFuture<Void>();
        var entered = new CompletableFuture<Void>();
        INITIALIZE_ENTERED.set(entered);
        INITIALIZE_HOLD.set(hold);
        try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options(store))) {
            store.armed.set(true);
            var first =
                    runtime.spotManager()
                            .getOrCreate("raced-room", "room")
                            .submit()
                            .toCompletableFuture();
            try {
                store.observed.get(5, TimeUnit.SECONDS);
                var winner =
                        runtime.spotManager()
                                .getOrCreate("raced-room", "room")
                                .submit()
                                .toCompletableFuture();
                entered.get(5, TimeUnit.SECONDS);
                store.watchFoundAfterRelease.set(true);
                store.release.complete(null);
                store.foundAfterRelease.get(5, TimeUnit.SECONDS);
                hold.completeExceptionally(
                        new IllegalStateException("controlled winner initialization failure"));
                assertThrows(ExecutionException.class, () -> winner.get(5, TimeUnit.SECONDS));
                var created = first.get(5, TimeUnit.SECONDS);
                assertEquals(ZLinkSpotCreateState.CREATED, created.state());
                assertEquals(2, FACTORY_CALLS.get());
                var descriptor =
                        new ZLinkProviderLocationRepository(store)
                                .listMeshNodes("existing-capacity", new ZLinkPageRequest(10, null))
                                .toCompletableFuture()
                                .get(5, TimeUnit.SECONDS)
                                .items()
                                .getFirst();
                assertEquals(1, descriptor.capacity().spots().active());
                assertEquals(0, descriptor.capacity().spots().reserved());
            } finally {
                store.release.complete(null);
                hold.completeExceptionally(new IllegalStateException("test cleanup"));
            }
        } finally {
            INITIALIZE_HOLD.set(null);
            INITIALIZE_ENTERED.set(null);
        }
    }

    private static DefaultZLinkFrameworkOptions options(ZLinkLocationStore store) {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        options.addRouteMesh("existing-capacity")
                .listen("tcp://127.0.0.1:0")
                .setSpotCapacity(1)
                .objects()
                .server()
                .addSpotFactory("room", Room.class, factory -> factory.disableRelocation());
        return options;
    }

    private static final class MutableClock extends Clock {
        private final AtomicReference<Instant> now = new AtomicReference<>(Instant.now());

        public ZoneId getZone() {
            return ZoneOffset.UTC;
        }

        public Clock withZone(ZoneId zone) {
            return this;
        }

        public Instant instant() {
            return now.get();
        }

        void advance(Duration duration) {
            now.updateAndGet(value -> value.plus(duration));
        }
    }

    private static final class MissingReadBarrier implements ZLinkLocationStore {
        private final ZLinkInMemoryLocationStore inner = new ZLinkInMemoryLocationStore();
        final AtomicBoolean armed = new AtomicBoolean();
        private final AtomicInteger missingReads = new AtomicInteger();
        private final int holdMissingNumber;
        final AtomicBoolean watchFoundAfterRelease = new AtomicBoolean();
        final CompletableFuture<Void> foundAfterRelease = new CompletableFuture<>();

        MissingReadBarrier(int holdMissingNumber) {
            this.holdMissingNumber = holdMissingNumber;
        }

        final CompletableFuture<Void> observed = new CompletableFuture<>();
        final CompletableFuture<Void> release = new CompletableFuture<>();

        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            return inner.read(key, cancellation)
                    .thenCompose(
                            result -> {
                                if (key.value().startsWith("authority\0")
                                        && key.value().endsWith("\0raced-room")
                                        && result instanceof ZLinkStoreReadMissing
                                        && armed.get()
                                        && missingReads.incrementAndGet() == holdMissingNumber
                                        && armed.compareAndSet(true, false)) {
                                    observed.complete(null);
                                    return release.thenApply(ignored -> result);
                                }
                                if (key.value().startsWith("authority\0")
                                        && key.value().endsWith("\0raced-room")
                                        && result instanceof ZLinkStoreReadFound
                                        && watchFoundAfterRelease.get()) {
                                    foundAfterRelease.complete(null);
                                }
                                return CompletableFuture.completedFuture(result);
                            });
        }

        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return inner.scan(request, cancellation);
        }

        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            return inner.write(request, cancellation);
        }
    }

    private static final class ReleaseConflictStore implements ZLinkLocationStore {
        private final ZLinkInMemoryLocationStore inner;
        final AtomicBoolean armed = new AtomicBoolean();
        final AtomicInteger conflicts = new AtomicInteger();
        final AtomicReference<String> beforeVersion = new AtomicReference<>();
        final AtomicReference<String> afterVersion = new AtomicReference<>();

        ReleaseConflictStore(Clock clock) {
            inner = new ZLinkInMemoryLocationStore(clock);
        }

        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            return inner.read(key, cancellation);
        }

        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return inner.scan(request, cancellation);
        }

        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            var deletion =
                    request.mutations().stream()
                            .filter(
                                    m ->
                                            m instanceof ZLinkStoreDelete removed
                                                    && removed.key()
                                                            .value()
                                                            .startsWith("authority\0")
                                                    && removed.key()
                                                            .value()
                                                            .endsWith("\0expired-room"))
                            .map(ZLinkStoreDelete.class::cast)
                            .findFirst();
            if (deletion.isEmpty() || !armed.compareAndSet(true, false))
                return inner.write(request, cancellation);
            var capacity =
                    request.mutations().stream()
                            .filter(
                                    m ->
                                            m instanceof ZLinkStorePut put
                                                    && put.key()
                                                            .value()
                                                            .startsWith("zlink:v11:capacity:"))
                            .map(ZLinkStorePut.class::cast)
                            .findFirst()
                            .orElseThrow();
            return inner.read(deletion.orElseThrow().key(), cancellation)
                    .thenCompose(
                            before -> {
                                beforeVersion.set(
                                        assertInstanceOf(ZLinkStoreReadFound.class, before)
                                                .value()
                                                .version()
                                                .value());
                                return inner.read(capacity.key(), cancellation)
                                        .thenCompose(
                                                read -> {
                                                    var value =
                                                            assertInstanceOf(
                                                                            ZLinkStoreReadFound
                                                                                    .class,
                                                                            read)
                                                                    .value();
                                                    return inner.write(
                                                                    new ZLinkStoreWriteRequest(
                                                                            java.util.List.of(
                                                                                    new ZLinkStoreVersionCondition(
                                                                                            capacity
                                                                                                    .key(),
                                                                                            value
                                                                                                    .version())),
                                                                            java.util.List.of(
                                                                                    new ZLinkStorePut(
                                                                                            capacity
                                                                                                    .key(),
                                                                                            value
                                                                                                    .bytes(),
                                                                                            null))),
                                                                    cancellation)
                                                            .thenCompose(
                                                                    rewritten -> {
                                                                        assertInstanceOf(
                                                                                ZLinkStoreWriteApplied
                                                                                        .class,
                                                                                rewritten);
                                                                        return inner.write(
                                                                                        request,
                                                                                        cancellation)
                                                                                .thenCompose(
                                                                                        result -> {
                                                                                            assertInstanceOf(
                                                                                                    ZLinkStoreWriteConflict
                                                                                                            .class,
                                                                                                    result);
                                                                                            conflicts
                                                                                                    .incrementAndGet();
                                                                                            return inner.read(
                                                                                                            deletion.orElseThrow()
                                                                                                                    .key(),
                                                                                                            cancellation)
                                                                                                    .thenApply(
                                                                                                            after -> {
                                                                                                                afterVersion
                                                                                                                        .set(
                                                                                                                                assertInstanceOf(
                                                                                                                                                ZLinkStoreReadFound
                                                                                                                                                        .class,
                                                                                                                                                after)
                                                                                                                                        .value()
                                                                                                                                        .version()
                                                                                                                                        .value());
                                                                                                                return result;
                                                                                                            });
                                                                                        });
                                                                    });
                                                });
                            });
        }
    }

    public static final class Room implements ZLinkSpot<ZLinkActor> {
        private final ZLinkSpotContext context;

        public Room(ZLinkSpotContext context) {
            this.context = context;
            FACTORY_CALLS.incrementAndGet();
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onInitialize() {
            var hold = INITIALIZE_HOLD.get();
            if (hold != null && FACTORY_CALLS.get() == 1) {
                INITIALIZE_ENTERED.get().complete(null);
                return hold;
            }
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
