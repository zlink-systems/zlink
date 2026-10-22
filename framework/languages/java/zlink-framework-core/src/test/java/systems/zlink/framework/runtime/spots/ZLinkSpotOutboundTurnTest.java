package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkActorJoinCompletion;
import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.actors.ZLinkSessionDisconnected;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkSpotContext;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicBoolean;

final class ZLinkSpotOutboundTurnTest {
    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void actorPacketAndDisconnectRetainSpotGateUntilCallbackCompletes(boolean disconnect)
            throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        var objects =
                options.addRouteMesh("mesh")
                        .listen("inproc://outbound-turn-" + System.nanoTime())
                        .objects()
                        .server();
        objects.addEntrySpot(Entry.class);
        objects.addSpotFactory(
                "room",
                Room.class,
                factory -> {
                    factory.disableRelocation();
                    factory.executionMode(ZLinkUserSpotExecutionMode.SPOT_WIDE);
                });
        objects.addActorFactory(
                "player", Player.class, Factory.class, factory -> factory.disableRelocation());
        try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            runtime.spotManager()
                    .getOrCreate("room", "room")
                    .submit()
                    .toCompletableFuture()
                    .get(3, TimeUnit.SECONDS);
            runtime.actorManager()
                    .create("player", "player")
                    .submit()
                    .toCompletableFuture()
                    .get(3, TimeUnit.SECONDS);
            var spots = (ZLinkSpotRuntime) runtime.spotManager();
            var actor = (Player) spots.actorSessions().localActor("player").orElseThrow();
            runtime.actorClient()
                    .requestToActor("player", new Join())
                    .submit(String.class)
                    .toCompletableFuture()
                    .get(3, TimeUnit.SECONDS);
            assertInstanceOf(
                    ZLinkActorJoinCompletion.Accepted.class, actor.joined.get(3, TimeUnit.SECONDS));
            var room = (Room) spots.localActorSpotSurface(actor);
            var nextRan = new AtomicBoolean();
            CompletionStage<?> callback =
                    disconnect
                            ? spots.actorSessions()
                                    .runtime()
                                    .notifyDisconnected(
                                            actor,
                                            new ZLinkSessionDisconnected(
                                                    RoutingId.from("session"), 1, 1))
                            : runtime.actorClient()
                                    .requestToActor("player", new Probe())
                                    .submit(String.class);
            try {
                room.entered.get(3, TimeUnit.SECONDS);
                var next =
                        ((DefaultSpotContext) room.context())
                                .enqueueDispatch(
                                        0,
                                        () -> {
                                            nextRan.set(true);
                                            return CompletableFuture.completedFuture(null);
                                        });
                assertThrows(
                        TimeoutException.class,
                        () -> next.toCompletableFuture().get(250, TimeUnit.MILLISECONDS));
                assertFalse(nextRan.get());
                assertFalse(callback.toCompletableFuture().isDone());
                room.release.complete(null);
                callback.toCompletableFuture().get(3, TimeUnit.SECONDS);
                next.toCompletableFuture().get(3, TimeUnit.SECONDS);
                assertTrue(nextRan.get());
            } finally {
                room.release.complete(null);
            }
        }
    }

    public record Join() {}

    public record Probe() {}

    public static final class Player implements ZLinkActor {
        private final ZLinkActorContext context;
        final CompletableFuture<ZLinkActorJoinCompletion> joined = new CompletableFuture<>();

        public Player(ZLinkActorContext context) {
            this.context = context;
        }

        @Override
        public ZLinkActorContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion result) {
            joined.complete(result);
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class Factory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new Player(context));
        }
    }

    public static final class Entry implements ZLinkEntrySpot<Player> {
        private final ZLinkEntrySpotContext context;

        public Entry(ZLinkEntrySpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkEntrySpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(JoinHandler.class);
        }

        @Override
        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class JoinHandler
            implements ZLinkEntrySpotActorRequestHandler<Entry, Player, Join, String> {
        @Override
        public CompletionStage<String> handle(
                Entry spot, Player actor, ZLinkMessageContext context, Join request) {
            actor.context().joinSpot("room", new Join()).defer();
            return CompletableFuture.completedFuture("scheduled");
        }
    }

    public static final class Room implements ZLinkSpot<Player> {
        private final ZLinkSpotContext context;
        final CompletableFuture<Void> entered = new CompletableFuture<>();
        final CompletableFuture<Void> release = new CompletableFuture<>();

        public Room(ZLinkSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(ProbeHandler.class);
        }

        @Override
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
        }

        @Override
        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDisconnectActor(Player actor) {
            entered.complete(null);
            return release;
        }
    }

    public static final class ProbeHandler
            implements ZLinkSpotActorRequestHandler<Room, Player, Probe, String> {
        @Override
        public CompletionStage<String> handle(
                Room spot, Player actor, ZLinkMessageContext context, Probe request) {
            spot.entered.complete(null);
            return spot.release.thenApply(ignored -> "done");
        }
    }
}
