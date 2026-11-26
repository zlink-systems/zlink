package MyGame.table;

import MyGame.*;

import org.springframework.context.annotation.AnnotationConfigApplicationContext;
import org.springframework.context.annotation.Bean;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.*;
import systems.zlink.framework.spring.*;

import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.concurrent.*;
import java.util.function.BooleanSupplier;

public final class RoomWithdrawalCheck {
    private RoomWithdrawalCheck() {}

    private static final String ROOM = "room-withdrawal-check",
            PLAYER = "entering-player",
            NO_SHOW = "absent-player",
            TICKET = "ticket-withdrawal-check";

    static void run() throws Exception {
        try (var app = new AnnotationConfigApplicationContext(Topology.class)) {
            var runtime = app.getBean(ZLinkFrameworkRuntime.class);
            var spots = runtime.spotManager();
            var actors = runtime.actorManager();
            require(
                    await(actors.create(PLAYER, Names.PLAYER).inMesh(Names.MESH).submit())
                            instanceof ZLinkActorCreateResult.Created,
                    "Could not create the entering player");
            var entry = await(actors.findSpot(PLAYER)).orElseThrow();
            require(
                    await(spots.getOrCreate(ROOM, Names.TABLE).inMesh(Names.MESH).submit()).state()
                            == ZLinkSpotCreateState.CREATED,
                    "Could not create the match room");

            var route = app.getBean(ZLinkRouteClient.class);
            await(route.sendToSpot(ROOM, new PrepareRoom(ROOM, List.of(PLAYER, NO_SHOW))).submit());
            require(
                    await(app.getBean(Preparation.class).result)
                            .equals(new RoomPrepared(ROOM, true)),
                    "Match room was not prepared");

            var actorClient = app.getBean(ZLinkActorClient.class);
            await(actorClient.sendToActor(PLAYER, new MatchAssigned(TICKET, ROOM, ROOM)).submit());
            eventually(
                    () ->
                            await(actors.findSpot(PLAYER))
                                    .filter(s -> s.spotId().equals(ROOM))
                                    .isPresent(),
                    "The entering player did not join the match room");

            var withdrawal = new WithdrawRoom(ROOM);
            await(route.sendToSpot(ROOM, withdrawal).submit());
            await(actorClient.sendToActor(PLAYER, withdrawal).submit());
            eventually(
                    () -> await(actors.findSpot(PLAYER)).filter(entry::equals).isPresent(),
                    "The entering player did not return to Entry");
            eventually(
                    () -> await(spots.find(ROOM)).isEmpty(),
                    "Withdrawn match room Spot still exists");
        }
    }

    private static <T> T await(CompletionStage<T> operation) {
        return operation
                .toCompletableFuture()
                .orTimeout(Control.HOST_DEADLINE.toSeconds(), TimeUnit.SECONDS)
                .join();
    }

    private static void eventually(BooleanSupplier condition, String failure)
            throws InterruptedException {
        long deadline = System.nanoTime() + Control.HOST_DEADLINE.toNanos();
        while (!condition.getAsBoolean()) {
            require(System.nanoTime() < deadline, failure);
            Thread.sleep(20);
        }
    }

    private static void require(boolean condition, String failure) {
        if (!condition) throw new AssertionError(failure);
    }

    @EnableZLinkFramework
    static class Topology {
        @Bean
        PlayerApi playerApi(ZLinkRouteClient route) {
            return new ChannelPlayerApi(route);
        }

        @Bean
        NodeConfig nodeConfig() {
            return new NodeConfig(
                    "withdrawal-check", null, null, "tcp://127.0.0.1:0", null, 0, null);
        }

        @Bean
        Preparation preparation() {
            return new Preparation();
        }

        @Bean
        ZLinkFrameworkConfigurer framework(NodeConfig config) {
            return options -> {
                options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
                options.addLocationStore(new ZLinkInMemoryLocationStore());
                options.addRelocationStore(new TestRelocationStore());
                options.addHandlersFromPackageOf(Matchmaking.class);
                options.addRouteMesh(Names.MESH)
                        .listen(config.meshEndpoint())
                        .objects()
                        .server()
                        .addEntrySpot(Entry.class)
                        .addSpotFactory(Names.TABLE, Table.class, b -> b.disableRelocation())
                        .addInstanceSpotFactory(
                                Names.POOL, Pool.class, b -> b.recreateOnRelocation())
                        .addActorFactory(
                                Names.PLAYER,
                                Player.class,
                                PlayerWithTicket.class,
                                b -> b.disableRelocation());
            };
        }
    }

    private static final class TestRelocationStore implements ZLinkRelocationStore {
        private final ConcurrentMap<ZLinkBlobReference, ZLinkBlobFound> blobs =
                new ConcurrentHashMap<>();

        public CompletionStage<ZLinkBlobPutResult> put(
                ZLinkBlobReference reference,
                byte[] payload,
                Duration retention,
                ZLinkStoreCancellation cancellation) {
            var now = Instant.now();
            var expires = now.plus(retention);
            blobs.put(reference, new ZLinkBlobFound(payload, expires, now));
            return CompletableFuture.completedFuture(new ZLinkBlobStored(expires, now));
        }

        public CompletionStage<ZLinkBlobReadResult> read(
                ZLinkBlobReference reference, ZLinkStoreCancellation cancellation) {
            var blob = blobs.get(reference);
            return CompletableFuture.completedFuture(
                    blob == null ? new ZLinkBlobMissing(Instant.now()) : blob);
        }

        public CompletionStage<ZLinkBlobRenewResult> renew(
                ZLinkBlobReference reference,
                Duration retention,
                ZLinkStoreCancellation cancellation) {
            var now = Instant.now();
            var blob =
                    blobs.computeIfPresent(
                            reference,
                            (key, old) ->
                                    new ZLinkBlobFound(old.bytes(), now.plus(retention), now));
            return CompletableFuture.completedFuture(
                    blob == null
                            ? new ZLinkBlobRenewMissing(now)
                            : new ZLinkBlobRenewed(blob.expiresAt(), now));
        }

        public CompletionStage<Void> delete(
                ZLinkBlobReference reference, ZLinkStoreCancellation cancellation) {
            blobs.remove(reference);
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class PlayerWithTicket implements ZLinkActorFactory {
        private final ZLinkRouteClient route;

        public PlayerWithTicket(ZLinkRouteClient route) {
            this.route = route;
        }

        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            var player = new Player(context, route);
            player.state.ticketId = TICKET;
            player.state.chips = Rules.INITIAL_CHIPS;
            player.state.disconnected = true;
            return CompletableFuture.completedFuture(player);
        }
    }

    public static final class Pool implements ZLinkInstanceSpot {
        private final ZLinkInstanceSpotContext context;

        public Pool(ZLinkInstanceSpotContext context) {
            this.context = context;
        }

        public ZLinkInstanceSpotContext context() {
            return context;
        }

        public void configure() {
            context.handlers().addPacket(Prepared.class);
        }
    }

    public static final class Preparation {
        final CompletableFuture<RoomPrepared> result = new CompletableFuture<>();
    }

    public static final class Prepared implements ZLinkSpotPacketHandler<Pool, RoomPrepared> {
        private final Preparation preparation;

        public Prepared(Preparation preparation) {
            this.preparation = preparation;
        }

        public CompletionStage<Void> handle(Pool pool, RoomPrepared message) {
            preparation.result.complete(message);
            return CompletableFuture.completedFuture(null);
        }
    }
}
