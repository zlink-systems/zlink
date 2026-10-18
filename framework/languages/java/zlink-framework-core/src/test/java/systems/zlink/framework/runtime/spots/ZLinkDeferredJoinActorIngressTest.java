package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorReceived;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.*;

import java.time.Duration;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;

final class ZLinkDeferredJoinActorIngressTest {
    private static final String ACTOR_ID = "joining-player";
    private static final String ROOM_ID = "joining-room";
    private static final int COMPLETION_SECONDS = 3;

    @Test
    void entryWithoutRoomViewHandlerAcceptsPayloadBehindDeferredJoinBarrier() throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        var objects =
                options.addRouteMesh("game")
                        .listen("inproc://deferred-join-ingress-" + UUID.randomUUID())
                        .objects()
                        .server();
        objects.addEntrySpot(Entry.class);
        objects.addSpotFactory("room", Room.class, factory -> factory.disableRelocation());
        objects.addActorFactory(
                "player", Player.class, Factory.class, factory -> factory.disableRelocation());
        try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            runtime.spotManager()
                    .getOrCreate(ROOM_ID, "room")
                    .submit()
                    .toCompletableFuture()
                    .get(COMPLETION_SECONDS, TimeUnit.SECONDS);
            runtime.actorManager()
                    .create(ACTOR_ID, "player")
                    .submit()
                    .toCompletableFuture()
                    .get(COMPLETION_SECONDS, TimeUnit.SECONDS);
            var spots = (ZLinkSpotRuntime) runtime.spotManager();
            var actor = (Player) spots.actorSessions().localActor(ACTOR_ID).orElseThrow();
            var entry = (Entry) spots.localActorSpotSurface(actor);
            var joinReply =
                    runtime.actorClient()
                            .requestToActor(ACTOR_ID, new JoinRoom())
                            .timeout(Duration.ofSeconds(COMPLETION_SECONDS))
                            .submit(String.class)
                            .toCompletableFuture();
            try {
                entry.registered.get(COMPLETION_SECONDS, TimeUnit.SECONDS);
                var ref = spots.actorSessions().runtime().currentRef(actor);
                var admission = new CompletableFuture<Void>();
                CompletableFuture<Void> dispatched;
                try (var header =
                                new ZLinkBackendActorReceived(
                                        ref,
                                        ref.nodeRid(),
                                        null,
                                        Optional.empty(),
                                        0,
                                        0,
                                        Message.from("RoomView"),
                                        true,
                                        null,
                                        "application/json");
                        var body =
                                new ZLinkBackendActorReceived(
                                        ref,
                                        ref.nodeRid(),
                                        null,
                                        Optional.empty(),
                                        0,
                                        0,
                                        Message.from("{\"roster\":7}"),
                                        false,
                                        null,
                                        "application/json")) {
                    dispatched =
                            spots.dispatchLocalActorPacket(
                                            (SpotDispatchLine) entry.context(),
                                            actor,
                                            ActorPacketFrames.decode(header),
                                            header,
                                            body,
                                            false,
                                            admission)
                                    .toCompletableFuture();
                    assertFalse(
                            dispatched.isDone(),
                            "RoomView must be accepted by the Actor queue behind Deferred Join");
                    admission.get(COMPLETION_SECONDS, TimeUnit.SECONDS);
                    assertFalse(actor.roomView.isDone());
                }
                entry.release.complete(null);
                assertEquals("scheduled", joinReply.get(COMPLETION_SECONDS, TimeUnit.SECONDS));
                assertInstanceOf(
                        ZLinkActorJoinCompletion.Accepted.class,
                        actor.joined.get(COMPLETION_SECONDS, TimeUnit.SECONDS));
                dispatched.get(COMPLETION_SECONDS, TimeUnit.SECONDS);
                assertEquals(7, actor.roomView.get(COMPLETION_SECONDS, TimeUnit.SECONDS).roster());
                assertInstanceOf(Room.class, spots.localActorSpotSurface(actor));
            } finally {
                entry.release.complete(null);
                joinReply.handle((value, error) -> null).join();
            }
        }
    }

    public record JoinRoom() {}

    public record RoomView(int roster) {}

    public static final class Player implements ZLinkActor {
        private final ZLinkActorContext context;
        final CompletableFuture<RoomView> roomView = new CompletableFuture<>();
        final CompletableFuture<ZLinkActorJoinCompletion> joined = new CompletableFuture<>();

        public Player(ZLinkActorContext context) {
            this.context = context;
        }

        @Override
        public ZLinkActorContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion completion) {
            joined.complete(completion);
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
        final CompletableFuture<Void> registered = new CompletableFuture<>();
        final CompletableFuture<Void> release = new CompletableFuture<>();

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

    public static final class Room implements ZLinkSpot<Player> {
        private final ZLinkSpotContext context;

        public Room(ZLinkSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(RoomViewHandler.class);
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
    }

    public static final class JoinHandler
            implements ZLinkEntrySpotActorRequestHandler<Entry, Player, JoinRoom, String> {
        @Override
        public CompletionStage<String> handle(
                Entry spot, Player actor, ZLinkMessageContext context, JoinRoom request) {
            actor.context().joinSpot(ROOM_ID).defer();
            spot.registered.complete(null);
            return spot.release.thenApply(ignored -> "scheduled");
        }
    }

    public static final class RoomViewHandler
            implements ZLinkSpotActorSendHandler<Room, Player, RoomView> {
        @Override
        public CompletionStage<Void> handle(
                Room spot, Player actor, ZLinkMessageContext context, RoomView message) {
            actor.roomView.complete(message);
            return CompletableFuture.completedFuture(null);
        }
    }
}
