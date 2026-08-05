package systems.zlink.framework.runtime;

import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;

import systems.zlink.framework.runtime.internal.backend.*;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;

final class ActorManagerTest {
    @Test
    void actorManager_createGetOrCreateFind_work() {
        Zlink.version();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        { var node = options.addRouteMesh("game");
            node.listen("inproc://play-router-" + System.nanoTime());
            node.objects().server().addSpotFactory(
                "GameSpot", GameSpot.class, factory -> factory.disableRelocation());
            node.objects().server().addActorFactory(
                "player", PlayerActor.class, PlayerActorFactory.class,
                factory -> factory.disableRelocation()); }

        try (ZLinkFrameworkRuntime runtime =
                 RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory())) {
            ZLinkActorCreateResult.Created createdResult =
                (ZLinkActorCreateResult.Created) runtime.actorManager()
                .create("player-1", "player")
                .submit()
                .toCompletableFuture()
                .join();
            ZLinkActorCreateResult.Existing reusedResult =
                (ZLinkActorCreateResult.Existing) runtime.actorManager()
                .getOrCreate("player-1", "player")
                .submit()
                .toCompletableFuture()
                .join();
            ActorRef created = createdResult.actor();
            ActorRef reused = reusedResult.actor();
            Optional<ActorRef> found = runtime.actorManager()
                .find("player-1")
                .toCompletableFuture()
                .join();

            assertEquals(created, reused);
            assertEquals(Optional.of(created), found);
            assertEquals("player-1", created.actorId());
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

    public static final class PlayerActorFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new PlayerActor(context));
        }
    }

    public static final class GameSpot implements ZLinkSpot<ZLinkActor> {
        @Override
        public ZLinkSpotContext context() {
            return null;
        }

        @Override public CompletionStage<Void> onJoinedActor(ZLinkActor actor) { return CompletableFuture.completedFuture(null); }
        @Override public CompletionStage<Void> onLeaveActor(ZLinkActor actor) { return CompletableFuture.completedFuture(null); }
    }
}
