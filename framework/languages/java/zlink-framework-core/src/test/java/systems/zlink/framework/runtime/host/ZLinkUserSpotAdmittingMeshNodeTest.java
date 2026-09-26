package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.SpotRef;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;

/**
 * A User Spot is activated on the MeshNode that admitted it (MeshNode §5, Location runtime §7):
 * a Spot created in the second MeshNode of a host reports that MeshNode and receives messages
 * there.
 */
final class ZLinkUserSpotAdmittingMeshNodeTest {
    @Test
    void userSpotCreatedInTheSecondMeshNodeLivesThere() throws Exception {
        String suffix = Long.toUnsignedString(System.nanoTime(), 36);
        RoutingId gameRid = RoutingId.from("admit-game-" + suffix);
        RoutingId lobbyRid = RoutingId.from("admit-lobby-" + suffix);
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        options.addRouteMesh("game")
                .listen("inproc://admit-game-" + suffix)
                .setRoutingId(gameRid)
                .objects()
                .server()
                .addSpotFactory("hall", RoomSpot.class, factory -> factory.disableRelocation());
        options.addRouteMesh("lobby")
                .listen("inproc://admit-lobby-" + suffix)
                .setRoutingId(lobbyRid)
                .objects()
                .server()
                .addSpotFactory("room", RoomSpot.class, factory -> factory.disableRelocation());

        try (ZLinkFrameworkRuntime runtime =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options, new ZLinkJavaBackendAdapterFactory())) {
            String spotId = "admit-room-" + suffix;
            SpotRef spot =
                    runtime.spotManager()
                            .getOrCreate(spotId, "room")
                            .inMesh("lobby")
                            .submit()
                            .toCompletableFuture()
                            .get(10, TimeUnit.SECONDS)
                            .spot();

            assertEquals("lobby", spot.meshName());
            assertEquals(lobbyRid, spot.nodeRid());
            assertEquals(lobbyRid, RoomSpot.last.context().nodeRid());
            assertEquals(
                    "pong:lobby",
                    RoomSpot.last
                            .context()
                            .outbound()
                            .requestToSpot(spotId, new Ping("lobby"))
                            .timeout(Duration.ofSeconds(5))
                            .submit(Pong.class)
                            .toCompletableFuture()
                            .get(10, TimeUnit.SECONDS)
                            .value());
            assertEquals(1, runtime.routeMeshRuntime().snapshot("lobby").placement().activeSpotCount());
            assertEquals(0, runtime.routeMeshRuntime().snapshot("game").placement().activeSpotCount());
        }
    }

    public record Ping(String value) {}

    public record Pong(String value) {}

    public static final class RoomSpot implements ZLinkSpot<ZLinkActor> {
        static volatile RoomSpot last;
        private final ZLinkSpotContext context;

        public RoomSpot(ZLinkSpotContext context) {
            this.context = context;
            last = this;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(PingHandler.class);
        }

        @Override
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, ZLinkMessage request) {
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

    public static final class PingHandler implements ZLinkSpotRequestHandler<RoomSpot, Ping, Pong> {
        @Override
        public CompletionStage<Pong> handle(RoomSpot spot, Ping request) {
            return CompletableFuture.completedFuture(new Pong("pong:" + request.value()));
        }
    }
}
