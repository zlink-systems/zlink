package systems.zlink.framework.spring;

import static org.junit.jupiter.api.Assertions.*;
import static org.junit.jupiter.api.Assumptions.assumeTrue;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.context.ConfigurableApplicationContext;
import org.springframework.context.annotation.*;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.errors.*;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.locations.redis.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.host.*;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseMissing;
import systems.zlink.framework.runtime.internal.locations.ZLinkProviderLocationRepository;
import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryProviderLocationStore;
import systems.zlink.framework.runtime.locations.ZLinkServiceAuthorityPayloadCodec;
import systems.zlink.framework.spots.*;

import java.util.*;
import java.util.concurrent.*;

final class ZLinkStaleSpotOwnerIntegrationTest {
    private static final String ACTOR_ID = "P";
    private static final String ACTOR_TYPE = "player";
    private static final String USER_SPOT_TYPE = "slot";
    private static final String INSTANCE_SPOT_TYPE = "slot-hall-jackpot";
    private static final String NODE_ID = "restarted-node";
    private static final String USER_SPOT_ID = "slot:P";
    private static final String INSTANCE_ID = "slot-hall-jackpot/MAJOR_GRAND";
    private static final String MESH = "slots";
    private static final int WAIT_SECONDS = 10;

    @ParameterizedTest
    @CsvSource({"memory, false", "memory, true", "redis, false", "redis, true"})
    void userSpotRejoinsAfterSameNodeRestart(String backend, boolean spotFirst) throws Exception {
        try (var fixture = new Fixture(backend)) {
            long actorGeneration;
            long spotGeneration;
            List<ZLinkStoreScanItem> retained;
            try (var original = start(fixture)) {
                getActor(original);
                getUserSpot(original);
                join(original);
                actorGeneration =
                        fixture.authority(ZLinkAuthorityKeyCodec.actor(ACTOR_ID))
                                .objectGeneration();
                spotGeneration =
                        fixture.authority(ZLinkAuthorityKeyCodec.spot(USER_SPOT_ID))
                                .objectGeneration();
                fixture.assertMembership(spotGeneration);
                retained = ZLinkEndedOwnerStoreFixture.snapshot(fixture.store);
            }
            ZLinkEndedOwnerStoreFixture.restore(fixture.store, retained);
            fixture.assertEnded(USER_SPOT_ID);
            try (var restarted = start(fixture)) {
                if (spotFirst) {
                    // Location runtime §6.1: the Spot's only member also lost its owner, so the
                    // Spot is re-created and the member is released with it (Missing).
                    getUserSpot(restarted);
                    assertTrue(
                            fixture.authority(ZLinkAuthorityKeyCodec.spot(USER_SPOT_ID))
                                            .objectGeneration()
                                    > spotGeneration);
                    assertInstanceOf(
                            systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityMissing
                                    .class,
                            fixture.repository
                                    .read(ZLinkAuthorityKeyCodec.actor(ACTOR_ID), () -> false)
                                    .toCompletableFuture()
                                    .get());
                }
                getActor(restarted);
                assertTrue(
                        fixture.authority(ZLinkAuthorityKeyCodec.actor(ACTOR_ID)).objectGeneration()
                                > actorGeneration);
                getUserSpot(restarted);
                assertTrue(
                        fixture.authority(ZLinkAuthorityKeyCodec.spot(USER_SPOT_ID))
                                        .objectGeneration()
                                > spotGeneration);
                join(restarted);
                fixture.assertMembership(
                        fixture.authority(ZLinkAuthorityKeyCodec.spot(USER_SPOT_ID))
                                .objectGeneration());
            }
        }
    }

    @ParameterizedTest
    @CsvSource({"memory", "redis"})
    void instanceIntentRecreatesButFindAndOrdinaryMessagePreserveEndedAuthority(String backend)
            throws Exception {
        try (var fixture = new Fixture(backend)) {
            long generation;
            List<ZLinkStoreScanItem> retained;
            try (var original = start(fixture)) {
                generation = instance(original);
                fixture.steadyInstance.get(WAIT_SECONDS, TimeUnit.SECONDS);
                retained = ZLinkEndedOwnerStoreFixture.snapshot(fixture.store);
            }
            ZLinkEndedOwnerStoreFixture.restore(fixture.store, retained);
            fixture.assertEnded(INSTANCE_ID);
            try (var restarted = start(fixture)) {
                var key = ZLinkAuthorityKeyCodec.spot(INSTANCE_ID);
                var before = fixture.authority(key);
                assertEquals(
                        generation,
                        restarted
                                .runtime()
                                .spotManager()
                                .find(INSTANCE_ID)
                                .toCompletableFuture()
                                .get(WAIT_SECONDS, TimeUnit.SECONDS)
                                .orElseThrow()
                                .objectGeneration());
                assertEquals(before.storeVersion(), fixture.authority(key).storeVersion());
                unavailable(
                        () ->
                                restarted
                                        .runtime()
                                        .route()
                                        .requestToSpot(INSTANCE_ID, new Probe())
                                        .inMesh(MESH)
                                        .submit(Reply.class)
                                        .toCompletableFuture()
                                        .get(WAIT_SECONDS, TimeUnit.SECONDS));
                assertEquals(before.storeVersion(), fixture.authority(key).storeVersion());
                assertTrue(instance(restarted) > generation);
            }
        }
    }

    @Configuration(proxyBeanMethods = false)
    @EnableZLinkFramework
    @Import(ZLinkFrameworkAutoConfiguration.class)
    public static class Config {
        @Bean(destroyMethod = "close")
        public ZLinkRedisRelocationStore relocationStore() {
            return new ZLinkRedisRelocationStore(
                    new ZLinkRedisRelocationOptions()
                            .setConnectionString(System.getenv("ZLINK_REDIS_LOCATION_ENDPOINT"))
                            .setKeyPrefix("stale-spot-blobs-1618-" + UUID.randomUUID() + ":"));
        }

        @Bean
        public ZLinkFrameworkConfigurer framework(ZLinkRedisRelocationStore relocationStore) {
            return options -> {
                options.addRelocationStore(relocationStore);
                options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
                var server =
                        options.addRouteMesh(MESH)
                                .listen("tcp://127.0.0.1:0")
                                .setRoutingId(RoutingId.from(NODE_ID))
                                .objects()
                                .server()
                                .addEntrySpot(Entry.class);
                server.addActorFactory(
                        ACTOR_TYPE, Player.class, Factory.class, f -> f.disableRelocation());
                server.addSpotFactory(USER_SPOT_TYPE, Slot.class, f -> f.disableRelocation());
                server.addInstanceSpotFactory(
                        INSTANCE_SPOT_TYPE, Jackpot.class, f -> f.disableRelocation());
            };
        }
    }

    private record Session(ConfigurableApplicationContext app, ZLinkFrameworkRuntime runtime)
            implements AutoCloseable {
        public void close() {
            app.close();
        }
    }

    private static Session start(ZLinkLocationStore store) {
        var app =
                new SpringApplicationBuilder(Config.class)
                        .web(WebApplicationType.NONE)
                        .initializers(
                                context ->
                                        context.getBeanFactory().registerSingleton("store", store))
                        .run();
        return new Session(app, app.getBean(ZLinkFrameworkRuntime.class));
    }

    private static void getActor(Session session) throws Exception {
        var runtime = session.runtime();
        runtime.actorManager()
                .getOrCreate(ACTOR_ID, ACTOR_TYPE)
                .inMesh(MESH)
                .submit()
                .toCompletableFuture()
                .get(WAIT_SECONDS, TimeUnit.SECONDS);
    }

    private static void getUserSpot(Session session) throws Exception {
        var runtime = session.runtime();
        runtime.spotManager()
                .getOrCreate(USER_SPOT_ID, USER_SPOT_TYPE)
                .inMesh(MESH)
                .submit()
                .toCompletableFuture()
                .get(WAIT_SECONDS, TimeUnit.SECONDS);
    }

    private static void join(Session session) throws Exception {
        var runtime = session.runtime();
        Slot.joined = new CompletableFuture<>();
        runtime.actorClient()
                .requestToActor(ACTOR_ID, new Join())
                .submit(String.class)
                .toCompletableFuture()
                .get(WAIT_SECONDS, TimeUnit.SECONDS);
        Slot.joined.get(WAIT_SECONDS, TimeUnit.SECONDS);
    }

    private static long instance(Session session) throws Exception {
        var runtime = session.runtime();
        Jackpot.handled = new CompletableFuture<>();
        runtime.route()
                .sendToSpot(INSTANCE_ID, new Signal())
                .instanceSpot(INSTANCE_SPOT_TYPE)
                .inMesh(MESH)
                .submit()
                .toCompletableFuture()
                .get(WAIT_SECONDS, TimeUnit.SECONDS);
        return Jackpot.handled.get(WAIT_SECONDS, TimeUnit.SECONDS);
    }

    private static void unavailable(ThrowingOperation operation) {
        var failure = assertThrows(ExecutionException.class, operation::run);
        assertEquals(
                ZLinkFrameworkErrorKind.UNAVAILABLE,
                assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
    }

    private interface ThrowingOperation {
        void run() throws Exception;
    }

    private static final class Fixture implements AutoCloseable, ZLinkLocationStore {
        final CompletableFuture<Void> steadyInstance = new CompletableFuture<>();
        final ZLinkLocationStore store;
        final ZLinkProviderLocationRepository repository;

        Fixture(String backend) {
            String endpoint = System.getenv("ZLINK_REDIS_LOCATION_ENDPOINT");
            assumeTrue(
                    endpoint != null && !endpoint.isBlank(),
                    "Redis relocation provider endpoint is required");
            if (backend.equals("redis")) {
                store =
                        new ZLinkRedisLocationStore(
                                new ZLinkRedisLocationOptions()
                                        .setConnectionString(endpoint)
                                        .setKeyPrefix(
                                                "stale-spot-1618-" + UUID.randomUUID() + ":"));
            } else store = new ZLinkInMemoryProviderLocationStore();
            repository = new ZLinkProviderLocationRepository(store);
        }

        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            return store.read(key, cancellation);
        }

        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return store.scan(request, cancellation);
        }

        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            return store.write(request, cancellation)
                    .thenCompose(
                            result -> {
                                if (steadyInstance.isDone()
                                        || request.mutations().stream()
                                                .noneMatch(
                                                        mutation ->
                                                                mutation
                                                                                instanceof
                                                                                ZLinkStorePut put
                                                                        && put.key()
                                                                                .value()
                                                                                .startsWith(
                                                                                        ZLinkEndedOwnerStoreFixture
                                                                                                .AUTHORITY_PREFIX)))
                                    return CompletableFuture.completedFuture(result);
                                return repository
                                        .read(
                                                ZLinkAuthorityKeyCodec.spot(INSTANCE_ID),
                                                cancellation::isCancellationRequested)
                                        .thenApply(
                                                read -> {
                                                    if (read
                                                            instanceof
                                                            ZLinkAuthoritySnapshot snapshot) {
                                                        var authority =
                                                                new ZLinkServiceAuthorityPayloadCodec()
                                                                        .decode(snapshot.payload())
                                                                        .orElseThrow();
                                                        if (authority.state()
                                                                        == ZLinkServiceAuthorityPayloadCodec
                                                                                .State.READY
                                                                && authority
                                                                        .activationRecoveryState()
                                                                        .isEmpty())
                                                            steadyInstance.complete(null);
                                                    }
                                                    return result;
                                                });
                            });
        }

        ZLinkAuthoritySnapshot authority(String key) throws Exception {
            return assertInstanceOf(
                    ZLinkAuthoritySnapshot.class,
                    repository.read(key, () -> false).toCompletableFuture().get());
        }

        void assertEnded(String spotId) throws Exception {
            var retained = authority(ZLinkAuthorityKeyCodec.spot(spotId));
            assertInstanceOf(
                    ZLinkOwnerLeaseMissing.class,
                    repository.readOwnerLease(retained.ownerId()).toCompletableFuture().get());
        }

        void assertMembership(long generation) throws Exception {
            var member =
                    new ZLinkActorAuthorityPayloadCodec()
                            .decode(authority(ZLinkAuthorityKeyCodec.actor(ACTOR_ID)).payload())
                            .orElseThrow();
            assertEquals(USER_SPOT_ID, member.currentSpotId());
            assertEquals(generation, member.currentSpotGeneration());
        }

        public void close() throws Exception {
            if (store instanceof AutoCloseable closeable) closeable.close();
        }
    }

    public record Join() {}

    public record Probe() {}

    public record Signal() {}

    public record Reply(long generation) {}

    public static final class Player implements ZLinkActor {
        private final ZLinkActorContext context;

        public Player(ZLinkActorContext context) {
            this.context = context;
        }

        public ZLinkActorContext context() {
            return context;
        }
    }

    public static final class Factory implements ZLinkActorFactory {
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new Player(context));
        }
    }

    public static final class Entry implements ZLinkEntrySpot<Player> {
        private final ZLinkEntrySpotContext context;

        public Entry(ZLinkEntrySpotContext context) {
            this.context = context;
        }

        public ZLinkEntrySpotContext context() {
            return context;
        }

        public void configure() {
            context.handlers().addHandler(JoinHandler.class);
        }

        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class JoinHandler
            implements ZLinkEntrySpotActorRequestHandler<Entry, Player, Join, String> {
        public CompletionStage<String> handle(
                Entry entry, Player player, ZLinkMessageContext context, Join request) {
            player.context().joinSpot(USER_SPOT_ID).defer();
            return CompletableFuture.completedFuture("scheduled");
        }
    }

    public static final class Slot implements ZLinkSpot<ZLinkActor> {
        static CompletableFuture<Void> joined;
        private final ZLinkSpotContext context;

        public Slot(ZLinkSpotContext context) {
            this.context = context;
        }

        public ZLinkSpotContext context() {
            return context;
        }

        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
        }

        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            joined.complete(null);
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class Jackpot implements ZLinkInstanceSpot {
        static CompletableFuture<Long> handled;
        private final ZLinkInstanceSpotContext context;

        public Jackpot(ZLinkInstanceSpotContext context) {
            this.context = context;
        }

        public ZLinkInstanceSpotContext context() {
            return context;
        }

        public void configure() {
            context.handlers().addPacket(ProbeHandler.class);
            context.handlers().addPacket(SignalHandler.class);
        }
    }

    public static final class SignalHandler implements ZLinkSpotPacketHandler<Jackpot, Signal> {
        public CompletionStage<Void> handle(Jackpot spot, Signal signal) {
            Jackpot.handled.complete(spot.context().objectGeneration());
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class ProbeHandler
            implements ZLinkSpotRequestHandler<Jackpot, Probe, Reply> {
        public CompletionStage<Reply> handle(Jackpot spot, Probe probe) {
            return CompletableFuture.completedFuture(new Reply(spot.context().objectGeneration()));
        }
    }
}
