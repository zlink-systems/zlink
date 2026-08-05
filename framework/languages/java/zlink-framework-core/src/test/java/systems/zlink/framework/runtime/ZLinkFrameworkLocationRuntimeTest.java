package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityMissing;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.locations.ZLinkLocationPage;
import systems.zlink.framework.locations.ZLinkLocationRole;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteIntent;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkProviderLocationRepository;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendDealerSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendPublisherSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSubscriberSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkChannelBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkMonitoringBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkSpotBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkStreamBackendAdapter;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotKind;

class ZLinkFrameworkLocationRuntimeTest {
    private static ZLinkLocationRepository repository(
        ZLinkInMemoryLocationStore store) {
        return new ZLinkProviderLocationRepository(store);
    }

    @Test
    void userSpotGetOrCreateRejectsReservedEntrySpotIdBeforeSubmit() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();

        try (ZLinkFrameworkRuntime runtime =
                 ZLinkFrameworkRuntimeTestAccess.start(options, new MinimalBackend())) {
            assertThrows(
                ZLinkConfigurationException.class,
                () -> runtime.spotManager().getOrCreate(
                    "host-entry-00000000-0000-4000-8000-000000000001",
                    "room"));
        }
    }

    @Test
    void configuredLocationStoreStartsLeaseAndCloseRemovesOwnerRows() throws Exception {
        ZLinkInMemoryLocationStore store = new ZLinkInMemoryLocationStore();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        options.addRelocationStore(new InMemoryRelocationStore());

        ZLinkFrameworkRuntime runtime = ZLinkFrameworkRuntimeTestAccess.start(options, new MinimalBackend());
        runtime.closeAsync().toCompletableFuture().get();

        assertEquals(
            List.of(),
            repository(store).listMeshNodes("unused", ZLinkPageRequest.firstPage())
                .toCompletableFuture().get().items());
    }

    @Test
    void userSpotCreationClaimsLocationRowAndCloseRemovesIt() throws Exception {
        ZLinkInMemoryLocationStore store = new ZLinkInMemoryLocationStore();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        RoutingId nodeRid = RoutingId.from("spot-node");
        String spotId = "room-1";
        var mesh = options.addRouteMesh("location-game")
            .setRoutingIdPrefix(nodeRid.toString())
            .listen("inproc://location-user-spot");
        mesh.channelName("location-game").server();
        mesh.objects().server().addSpotFactory(
            "location-spot",
            LocationSpot.class,
            factory -> factory.disableRelocation());

        try (ZLinkFrameworkRuntime runtime =
                 ZLinkFrameworkRuntimeTestAccess.start(options, new ZLinkJavaBackendAdapterFactory())) {
            var created = runtime.spotManager()
                .getOrCreate(spotId, "location-spot")
                .submit()
                .toCompletableFuture()
                .get();

            assertInstanceOf(
                ZLinkAuthoritySnapshot.class,
                repository(store).read(
                        ZLinkAuthorityKeyCodec.spot(spotId),
                        () -> false)
                    .toCompletableFuture()
                    .get());
            assertTrue(runtime.spotManager().close(created.spot())
                .toCompletableFuture().get());
            assertInstanceOf(
                ZLinkAuthorityMissing.class,
                repository(store).read(
                        ZLinkAuthorityKeyCodec.spot(spotId),
                        () -> false)
                    .toCompletableFuture()
                    .get());
        }
    }

    @Test
    void userSpotCloseRejectsMovingAuthorityBeforeLocalClose() throws Exception {
        ZLinkInMemoryLocationStore store = new ZLinkInMemoryLocationStore();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        RoutingId nodeRid = RoutingId.from("moving-spot-node");
        String spotId = "moving-room";
        var mesh = options.addRouteMesh("moving-game")
            .setRoutingIdPrefix(nodeRid.toString())
            .listen("inproc://moving-user-spot");
        mesh.channelName("moving-game").server();
        mesh.objects().server().addSpotFactory(
            "location-spot",
            LocationSpot.class,
            factory -> factory.disableRelocation());

        try (ZLinkFrameworkRuntime runtime =
                 ZLinkFrameworkRuntimeTestAccess.start(
                     options, new ZLinkJavaBackendAdapterFactory())) {
            var created = runtime.spotManager()
                .getOrCreate(spotId, "location-spot")
                .submit()
                .toCompletableFuture()
                .get();
            String key = ZLinkAuthorityKeyCodec.spot(spotId);
            var snapshot = assertInstanceOf(
                ZLinkAuthoritySnapshot.class,
                repository(store).read(key, () -> false).toCompletableFuture().get());
            var authority = new systems.zlink.framework.runtime.locations
                .ZLinkServiceAuthorityPayloadCodec()
                .decode(snapshot.payload())
                .orElseThrow();
            byte[] closing = new systems.zlink.framework.runtime.locations
                .ZLinkServiceAuthorityPayloadCodec()
                .encodeUser(
                    systems.zlink.framework.runtime.locations
                        .ZLinkServiceAuthorityPayloadCodec.State.CLOSING,
                    authority.stableType(),
                    authority.spotId(),
                    authority.ownerId(),
                    authority.ownerLeaseGeneration(),
                    authority.meshName(),
                    authority.nodeRid(),
                    authority.nodeGeneration());
            repository(store).compareExchange(
                    key,
                    new systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityExpectFound(snapshot.storeVersion()),
                    new systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityPut(
                        closing,
                        systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityGenerationTransition.PRESERVE,
                        Optional.empty(),
                        Optional.empty()),
                    () -> false)
                .toCompletableFuture()
                .get();

            assertThrows(CompletionException.class, () ->
                runtime.spotManager().close(created.spot())
                    .toCompletableFuture()
                    .join());
        }
    }

    @Test
    void durableActorCreationPublishesReadyAuthorityThroughCommand49()
        throws Exception {
        ZLinkInMemoryLocationStore store =
            new ZLinkInMemoryLocationStore();
        DefaultZLinkFrameworkOptions options =
            new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        RoutingId nodeRid = RoutingId.from("durable-actor-node");
        var mesh = options.addRouteMesh("durable-actors")
            .setRoutingIdPrefix(nodeRid.toString())
            .listen("inproc://durable-actor-create");
        mesh.channelName("durable-actors").server();
        mesh.objects().server().addActorFactory(
            "player",
            LocationActor.class,
            LocationActorFactory.class,
            factory -> factory.disableRelocation());

        try (ZLinkFrameworkRuntime runtime =
                 ZLinkFrameworkRuntimeTestAccess.start(
                     options,
                     new ZLinkJavaBackendAdapterFactory())) {
            var created = runtime.actorManager()
                .create("durable-player", "player")
                .submit()
                .toCompletableFuture()
                .get();

            assertInstanceOf(
                systems.zlink.framework.actors
                    .ZLinkActorCreateResult.Created.class,
                created);
            var authority = assertInstanceOf(
                systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot.class,
                repository(store).read(
                        systems.zlink.framework.runtime.locations
                            .ZLinkAuthorityKeyCodec.actor(
                                "durable-player"),
                        () -> false)
                    .toCompletableFuture()
                    .get());
            assertEquals(
                systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocationState.ACTIVE,
                authority.allocation().state());
            assertTrue(authority.pendingCreation().isEmpty());
        }
    }

    @Test
    void durableConcurrentGetOrCreateWaitsForCreatingThenReturnsExisting()
        throws Exception {
        BlockingLocationActorFactory.reset();
        ZLinkInMemoryLocationStore store =
            new ZLinkInMemoryLocationStore();
        DefaultZLinkFrameworkOptions options =
            new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        var mesh = options.addRouteMesh("durable-concurrent")
            .setRoutingIdPrefix("durable-concurrent-node")
            .listen("inproc://durable-actor-concurrent");
        mesh.channelName("durable-concurrent").server();
        mesh.objects().server().addActorFactory(
            "player",
            LocationActor.class,
            BlockingLocationActorFactory.class,
            factory -> factory.disableRelocation());

        try (ZLinkFrameworkRuntime runtime =
                 ZLinkFrameworkRuntimeTestAccess.start(
                     options,
                     new ZLinkJavaBackendAdapterFactory())) {
            var first = runtime.actorManager().getOrCreate(
                "durable-concurrent-player", "player").submit();
            long waitUntil = System.nanoTime()
                + Duration.ofSeconds(5).toNanos();
            while (BlockingLocationActorFactory.invocations.get() == 0
                && System.nanoTime() < waitUntil) {
                Thread.onSpinWait();
            }
            var second = runtime.actorManager().getOrCreate(
                "durable-concurrent-player", "player").submit();

            assertEquals(
                1,
                BlockingLocationActorFactory.invocations.get());
            assertTrue(!second.toCompletableFuture().isDone());
            BlockingLocationActorFactory.release.complete(null);

            assertInstanceOf(
                systems.zlink.framework.actors
                    .ZLinkActorCreateResult.Created.class,
                first.toCompletableFuture().get());
            assertInstanceOf(
                systems.zlink.framework.actors
                    .ZLinkActorCreateResult.Existing.class,
                second.toCompletableFuture().get());
            assertEquals(
                1,
                BlockingLocationActorFactory.invocations.get());
        }
    }

    @Test
    void actorJoinAndLeaveRenewLocationRow() throws Exception {
        ZLinkInMemoryLocationStore store = new ZLinkInMemoryLocationStore();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        options.addRelocationStore(new InMemoryRelocationStore());
        RoutingId nodeRid = RoutingId.from("join-node");
        String spotId = "join-room";
        LocationActorFactory.last.set(null);
        LocationSpot.last.set(null);
        var mesh = options.addRouteMesh("rooms")
            .setRoutingIdPrefix(nodeRid.toString())
            .listen("inproc://location-actor-join");
        mesh.channelName("rooms").server();
        mesh.objects().server().addActorFactory(
            "player",
            LocationActor.class,
            LocationActorFactory.class,
            factory -> factory.disableRelocation());
        mesh.objects().server().addSpotFactory(
            "location-spot",
            LocationSpot.class,
            factory -> factory.disableRelocation());
        RoutingId actualNodeRid = options.registration().meshNodes().getFirst().routingId();

        try (ZLinkFrameworkRuntime runtime =
                 ZLinkFrameworkRuntimeTestAccess.start(options, new ZLinkJavaBackendAdapterFactory())) {
            runtime.actorManager()
                .create("player-join", "player")
                .submit()
                .toCompletableFuture()
                .get();
            runtime.spotManager()
                .getOrCreate(spotId, "location-spot")
                .submit()
                .toCompletableFuture()
                .get();
            LocationActor actor = LocationActorFactory.last.get();
            assertEquals("player-join", actor.context().actorId());
            assertEquals(1L, actor.context().objectGeneration());
            assertEquals("rooms", actor.context().meshName());

            assertThrows(
                systems.zlink.framework.errors.ZLinkFrameworkException.class,
                () -> actor.context()
                    .joinSpot(
                        spotId,
                        systems.zlink.framework.messaging.ZLinkMessage.empty())
                    .defer());

            var snapshot = assertInstanceOf(
                ZLinkAuthoritySnapshot.class,
                repository(store).read(
                    ZLinkAuthorityKeyCodec.actor("player-join"),
                    () -> false).toCompletableFuture().get());
            var authority = new systems.zlink.framework.runtime.locations
                .ZLinkActorAuthorityPayloadCodec()
                .decode(snapshot.payload()).orElseThrow();
            assertEquals("player-join", authority.actorId());
            assertEquals(1, authority.currentSpotKind());
        }
    }

    private static final class MinimalBackend implements ZLinkBackendAdapterProvider, ZLinkChannelBackendAdapter {
        @Override
        public ZLinkChannelBackendAdapter createChannelAdapter(ZLinkBackendAdapterOptions options) {
            return this;
        }

        @Override
        public ZLinkSpotBackendAdapter createSpotAdapter(ZLinkBackendAdapterOptions options) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkStreamBackendAdapter createStreamAdapter(ZLinkBackendAdapterOptions options) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkMonitoringBackendAdapter createMonitoringAdapter(ZLinkBackendAdapterOptions options) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkBackendContext createContext() {
            return new ZLinkBackendContext() {
                @Override
                public String name() {
                    return "context";
                }

                @Override
                public void shutdown() {
                }

                @Override
                public void close() {
                }
            };
        }

        @Override
        public ZLinkBackendDealerSocket createDealerSocket(ZLinkBackendContext context) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkBackendRouterSocket createRouterSocket(ZLinkBackendContext context) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkBackendPublisherSocket createPublisherSocket(ZLinkBackendContext context) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkBackendSubscriberSocket createSubscriberSocket(ZLinkBackendContext context) {
            throw new UnsupportedOperationException();
        }
    }

    public static final class LocationSpot implements ZLinkSpot<ZLinkActor> {
        static final AtomicReference<LocationSpot> last = new AtomicReference<>();
        private final ZLinkSpotContext context;

        public LocationSpot(ZLinkSpotContext context) {
            this.context = context;
            last.set(this);
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
            String actorId,
            systems.zlink.framework.messaging.ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
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

    public static final class LocationActorFactory implements ZLinkActorFactory {
        static final AtomicReference<LocationActor> last = new AtomicReference<>();

        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            LocationActor actor = new LocationActor(context.actorId(), context);
            last.set(actor);
            return CompletableFuture.completedFuture(actor);
        }
    }

    public static final class BlockingLocationActorFactory
        implements ZLinkActorFactory {
        static final java.util.concurrent.atomic.AtomicInteger invocations =
            new java.util.concurrent.atomic.AtomicInteger();
        static CompletableFuture<Void> release;

        static void reset() {
            invocations.set(0);
            release = new CompletableFuture<>();
        }

        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            invocations.incrementAndGet();
            return release.thenApply(
                ignored -> new LocationActor(context.actorId(), context));
        }
    }

public record LocationActor(String actorId, ZLinkActorContext context) implements ZLinkActor {
    }
}
