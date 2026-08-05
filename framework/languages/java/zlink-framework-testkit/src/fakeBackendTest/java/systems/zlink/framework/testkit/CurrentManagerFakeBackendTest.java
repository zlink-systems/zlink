package systems.zlink.framework.testkit;

import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorCreateCall;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotCreateCall;
import systems.zlink.framework.spots.ZLinkSpotContext;

final class CurrentManagerFakeBackendTest {
    @Test
    void actorAndSpotManagersExposeCurrentFluentCallsAgainstFakeBackend() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        var mesh = options.addRouteMesh("game")
            .listen("inproc://current-manager")
            .setRoutingIdPrefix("current-manager");
        mesh.channelName("game").server();
        mesh.objects().server()
            .addSpotFactory(
                "room",
                RoomSpot.class,
                factory -> factory.disableRelocation())
            .addActorFactory(
                "player",
                PlayerActor.class,
                PlayerActorFactory.class,
                factory -> factory.disableRelocation());

        FakeZLinkBackendAdapterFactory backend =
            new FakeZLinkBackendAdapterFactory();
        try (ZLinkFrameworkRuntime runtime =
                 RuntimeTestSupport.startFramework(options, backend)) {
            var actorCall = runtime.actorManager()
                .create("player-1", "player")
                .inMesh("game")
                .request("setup")
                .timeout(Duration.ofSeconds(1));
            assertInstanceOf(ZLinkActorCreateCall.class, actorCall);

            var spotCall = runtime.spotManager()
                .create("room")
                .inMesh("game")
                .request("setup")
                .timeout(Duration.ofSeconds(1));
            assertInstanceOf(ZLinkSpotCreateCall.class, spotCall);

            assertThrows(
                RuntimeException.class,
                () -> runtime.actorManager()
                    .create("outside-turn", "player")
                    .yield());
            assertThrows(
                RuntimeException.class,
                () -> runtime.spotManager()
                    .create("room")
                    .inMesh("game")
                    .request("setup")
                    .timeout(Duration.ofSeconds(1))
                    .yield());
        }
    }

    public static final class PlayerActor implements ZLinkActor {
        private final ZLinkActorContext context;

        PlayerActor(ZLinkActorContext context) {
            this.context = context;
        }

        @Override
        public ZLinkActorContext context() {
            return context;
        }
    }

    public static final class PlayerActorFactory
        implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(
            ZLinkActorContext context) {
            return CompletableFuture.completedFuture(
                new PlayerActor(context));
        }
    }

    public static final class RoomSpot implements ZLinkSpot<ZLinkActor> {
        @Override
        public ZLinkSpotContext context() {
            return null;
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
