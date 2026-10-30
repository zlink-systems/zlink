package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.configuration.ZLinkObjectFactoryRegistration.RelocationPolicy;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.locations.ZLinkServiceAuthorityPayloadCodec;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;

import java.time.*;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

final class ZLinkUserSpotCreateCollisionTest {
    private static final AtomicInteger FACTORIES = new AtomicInteger();

    @ParameterizedTest
    @ValueSource(strings = {"expired-ready", "closing"})
    void createTerminatesOnAnActiveGeneratedIdentityCollision(String scenario) throws Exception {
        FACTORIES.set(0);
        var clock = new MutableClock();
        var store = new GeneratedIdentityRead(clock);
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        options.addRouteMesh("create-collision")
                .listen("tcp://127.0.0.1:0")
                .setSpotCapacity(1)
                .objects()
                .server()
                .addSpotFactory("room", Room.class, factory -> factory.disableRelocation());
        try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            var repository = new ZLinkProviderLocationRepository(store);
            var local =
                    repository
                            .listMeshNodes("create-collision", new ZLinkPageRequest(10, null))
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS)
                            .items()
                            .getFirst();
            var owner =
                    assertInstanceOf(
                                    ZLinkOwnerLeaseClaimed.class,
                                    repository
                                            .claimOwnerLease(
                                                    "collision-owner", Duration.ofSeconds(1))
                                            .toCompletableFuture()
                                            .get(5, TimeUnit.SECONDS))
                            .token();
            var target =
                    new ZLinkMeshNodeDescriptor(
                            local.meshName(),
                            RoutingId.from("collision-owner-node"),
                            17,
                            1,
                            "inproc://collision-owner",
                            local.channelWeights(),
                            local.applicationVersion(),
                            local.objectCapabilities(),
                            local.objectRole(),
                            Optional.of("collision-owner-entry"),
                            local.placementWeight(),
                            local.capacity(),
                            local.activationConcurrency(),
                            local.maintenanceWave(),
                            local.state(),
                            local.securityIdentity(),
                            owner.ownerId(),
                            owner.leaseGeneration(),
                            local.updatedAt());
            repository
                    .updateMeshNode(target, ZLinkLocationWriteIntent.NEW_CLAIM)
                    .toCompletableFuture()
                    .get();
            store.armed.set(true);
            var creation = runtime.spotManager().create("room").submit().toCompletableFuture();
            try {
                var captured = store.captured.get(5, TimeUnit.SECONDS);
                String id = captured.value().substring(captured.value().lastIndexOf('\0') + 1);
                assertEquals(id, UUID.fromString(id).toString());
                assertEquals(4, UUID.fromString(id).version());
                String key = ZLinkAuthorityKeyCodec.spot(id);
                var codec = new ZLinkServiceAuthorityPayloadCodec();
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
                                                                        target.meshName(),
                                                                        target.rid()),
                                                                target.lifecycleGeneration(),
                                                                owner,
                                                                codec.encodeUser(
                                                                        ZLinkServiceAuthorityPayloadCodec
                                                                                .State.CREATING,
                                                                        "room",
                                                                        id,
                                                                        owner.ownerId(),
                                                                        owner.leaseGeneration(),
                                                                        target.meshName(),
                                                                        target.rid(),
                                                                        target
                                                                                .lifecycleGeneration()),
                                                                ZLinkPlacementCapacityBundle.spot(
                                                                        ZLinkPlacementObjectKind
                                                                                .USER_SPOT,
                                                                        "room",
                                                                        1),
                                                                new RelocationPolicy.Disabled()),
                                                        () -> false)
                                                .toCompletableFuture()
                                                .get())
                                .reservation();
                var state =
                        scenario.equals("closing")
                                ? ZLinkServiceAuthorityPayloadCodec.State.CLOSING
                                : ZLinkServiceAuthorityPayloadCodec.State.READY;
                repository
                        .commit(
                                reservation,
                                codec.encodeUser(
                                        state,
                                        "room",
                                        id,
                                        owner.ownerId(),
                                        owner.leaseGeneration(),
                                        target.meshName(),
                                        target.rid(),
                                        target.lifecycleGeneration()),
                                null,
                                () -> false)
                        .toCompletableFuture()
                        .get();
                var collision =
                        assertInstanceOf(
                                ZLinkAuthoritySnapshot.class,
                                repository.read(key, () -> false).toCompletableFuture().get());
                if (scenario.equals("expired-ready")) clock.advance(Duration.ofSeconds(2));
                store.authorityWrites.set(0);
                store.release.complete(null);
                store.delivered.get(5, TimeUnit.SECONDS);
                if (scenario.equals("closing")) {
                    // The captured Active collision remains terminal even if Close then completes.
                    assertInstanceOf(
                            ZLinkAuthorityDeleted.class,
                            repository
                                    .compareExchange(
                                            key,
                                            new ZLinkAuthorityExpectFound(collision.storeVersion()),
                                            new ZLinkAuthorityDelete(),
                                            () -> false)
                                    .toCompletableFuture()
                                    .get());
                }
                var failure =
                        assertThrows(
                                ExecutionException.class, () -> creation.get(5, TimeUnit.SECONDS));
                assertEquals(
                        ZLinkFrameworkErrorKind.ALREADY_EXISTS,
                        assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
                assertEquals(0, FACTORIES.get());
                assertEquals(scenario.equals("closing") ? 1 : 0, store.authorityWrites.get());
                if (scenario.equals("expired-ready")) {
                    var retained =
                            assertInstanceOf(
                                    ZLinkAuthoritySnapshot.class,
                                    repository.read(key, () -> false).toCompletableFuture().get());
                    assertEquals(collision.storeVersion(), retained.storeVersion());
                    assertEquals(collision.objectGeneration(), retained.objectGeneration());
                }
            } finally {
                store.release.complete(null);
            }
        }
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

        void advance(Duration delta) {
            now.updateAndGet(value -> value.plus(delta));
        }
    }

    private static final class GeneratedIdentityRead implements ZLinkLocationStore {
        private final ZLinkInMemoryLocationStore inner;
        final AtomicBoolean armed = new AtomicBoolean();
        final CompletableFuture<ZLinkStoreKey> captured = new CompletableFuture<>();
        final CompletableFuture<Void> release = new CompletableFuture<>();
        final CompletableFuture<Void> delivered = new CompletableFuture<>();
        final AtomicInteger authorityWrites = new AtomicInteger();
        private volatile ZLinkStoreKey capturedKey;

        GeneratedIdentityRead(Clock clock) {
            inner = new ZLinkInMemoryLocationStore(clock);
        }

        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            return inner.read(key, cancellation)
                    .thenCompose(
                            result -> {
                                if (key.value().startsWith("authority\0")
                                        && result instanceof ZLinkStoreReadMissing
                                        && armed.compareAndSet(true, false)) {
                                    capturedKey = key;
                                    captured.complete(key);
                                    return release.thenCompose(
                                                    ignored -> inner.read(key, cancellation))
                                            .thenApply(
                                                    current -> {
                                                        delivered.complete(null);
                                                        return current;
                                                    });
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
            if (request.mutations().stream()
                    .anyMatch(
                            m ->
                                    m instanceof ZLinkStorePut put && put.key().equals(capturedKey)
                                            || m instanceof ZLinkStoreDelete delete
                                                    && delete.key().equals(capturedKey))) {
                authorityWrites.incrementAndGet();
            }
            return inner.write(request, cancellation);
        }
    }

    public record Room(ZLinkSpotContext context) implements ZLinkSpot<ZLinkActor> {
        public Room {
            FACTORIES.incrementAndGet();
        }

        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
