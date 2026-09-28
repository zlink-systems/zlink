package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;

import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Spec 05-spot-actor-membership §4.1–4.2: the Entry Spot Join operation owns its request Message
 * until the target submission stage consumes it, also when the Entry Spot target resolves after the
 * call returns.
 */
final class ZLinkEntrySpotJoinPayloadOwnershipTest {
    private static final String ACTOR_ID = "entry-join-player";
    private static final AtomicReference<ZLinkActorContext> PLAYER_CONTEXT =
            new AtomicReference<>();

    @Test
    void entrySpotJoinKeepsRequestUntilAsynchronousTargetResolutionSubmitsIt() throws Exception {
        PLAYER_CONTEXT.set(null);
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        var node = options.addRouteMesh("entry-join");
        node.listen("inproc://entry-join-" + UUID.randomUUID())
                .setRoutingId(RoutingId.from("entry-join-" + UUID.randomUUID()));
        var objects = node.objects().server();
        objects.addEntrySpot(EntrySpot.class);
        objects.addActorFactory(
                "player",
                Player.class,
                PlayerFactory.class,
                factory -> factory.disableRelocation());

        try (ZLinkFrameworkRuntime runtime =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options, new ZLinkJavaBackendAdapterFactory())) {
            assertInstanceOf(
                    ZLinkActorCreateResult.Created.class,
                    runtime.actorManager()
                            .create(ACTOR_ID, "player")
                            .submit()
                            .toCompletableFuture()
                            .get(10, TimeUnit.SECONDS));
            var context =
                    assertInstanceOf(
                            ZLinkActorRuntime.DefaultActorContext.class, PLAYER_CONTEXT.get());

            AtomicReference<String> submitted = new AtomicReference<>();
            ZLinkInternalSpotNode spotNode =
                    (ZLinkInternalSpotNode)
                            Proxy.newProxyInstance(
                                    ZLinkInternalSpotNode.class.getClassLoader(),
                                    new Class<?>[] {ZLinkInternalSpotNode.class},
                                    (proxy, method, args) -> {
                                        if (method.getName().equals("joinActorEntrySpot")) {
                                            Message request = (Message) args[2];
                                            submitted.set(
                                                    StandardCharsets.UTF_8
                                                            .decode(request.dataBuffer())
                                                            .toString());
                                            return CompletableFuture.failedFuture(
                                                    new IllegalStateException("submitted"));
                                        }
                                        throw new UnsupportedOperationException(method.getName());
                                    });
            CompletableFuture<ZLinkActorRuntime.EntrySpotTarget> target = new CompletableFuture<>();
            CompletionStage<?> outcome =
                    new ZLinkActorEntrySpotJoinCall(
                                    context,
                                    timeout -> target,
                                    Message.from("entry-join-payload"),
                                    Duration.ofSeconds(5),
                                    new ZLinkActorEntrySpotJoinCall.Services(
                                            spotNode, null, null, null))
                            .execute();

            target.complete(
                    new ZLinkActorRuntime.EntrySpotTarget(
                            RoutingId.from("entry-target"), "entry-spot"));
            assertThrows(
                    Exception.class, () -> outcome.toCompletableFuture().get(5, TimeUnit.SECONDS));
            assertEquals("entry-join-payload", submitted.get());
        }
    }

    public static final class Player implements ZLinkActor {
        private final ZLinkActorContext context;

        public Player(ZLinkActorContext context) {
            this.context = context;
            PLAYER_CONTEXT.set(context);
        }

        @Override
        public ZLinkActorContext context() {
            return context;
        }
    }

    public static final class PlayerFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new Player(context));
        }
    }

    public static final class EntrySpot implements ZLinkEntrySpot<Player> {
        private final ZLinkEntrySpotContext context;

        public EntrySpot(ZLinkEntrySpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkEntrySpotContext context() {
            return context;
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
}
