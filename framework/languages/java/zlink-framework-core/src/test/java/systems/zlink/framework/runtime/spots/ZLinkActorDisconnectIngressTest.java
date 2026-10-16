package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkBoundSession;
import systems.zlink.framework.actors.ZLinkBoundSessionSendCall;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.actors.ZLinkActorSpotRoutePackets;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;

import java.lang.reflect.Method;
import java.util.List;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkActorDisconnectIngressTest {
    private static final String ACTOR_ID = "player-1";
    private static final String ACTOR_TYPE = "player";
    private static final int TIMEOUT_SECONDS = 3;

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void ingressRemainsOwnedUntilDisconnectCallbackTerminal(boolean failCallback) throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        var objects =
                options.addRouteMesh("mesh")
                        .listen("inproc://disconnect-ingress-" + UUID.randomUUID())
                        .objects()
                        .server();
        objects.addEntrySpot(Entry.class);
        objects.addActorFactory(
                ACTOR_TYPE, Player.class, Factory.class, factory -> factory.disableRelocation());
        try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            runtime.actorManager()
                    .create(ACTOR_ID, ACTOR_TYPE)
                    .submit()
                    .toCompletableFuture()
                    .get(TIMEOUT_SECONDS, TimeUnit.SECONDS);
            var spots = (ZLinkSpotRuntime) runtime.spotManager();
            var actor = spots.actorSessions().localActor(ACTOR_ID).orElseThrow();
            var entry = (Entry) spots.localActorSpotSurface(actor);
            var activationMethod =
                    ZLinkSpotRuntime.class.getDeclaredMethod(
                            "entrySpotActivationFor", String.class);
            activationMethod.setAccessible(true);
            var activation =
                    (SpotActivationBase<?>)
                            activationMethod.invoke(spots, entry.context().spotId());
            var ref = spots.actorSessions().runtime().currentRef(actor);
            try (var header =
                            new ZLinkBackendActorReceived(
                                    ref,
                                    ref.nodeRid(),
                                    null,
                                    Optional.empty(),
                                    0,
                                    0,
                                    Message.from(
                                            ZLinkActorSpotRoutePackets
                                                    .SESSION_DISCONNECTED_PACKET_NAME),
                                    true,
                                    null,
                                    null);
                    var body =
                            new ZLinkBackendActorReceived(
                                    ref,
                                    ref.nodeRid(),
                                    null,
                                    Optional.empty(),
                                    0,
                                    0,
                                    Message.from(new byte[0]),
                                    false,
                                    null,
                                    null)) {
                try {
                    var dispatched =
                            activation
                                    .dispatchActorMessages(List.of(header, body))
                                    .toCompletableFuture();
                    entry.entered.get(TIMEOUT_SECONDS, TimeUnit.SECONDS);
                    assertFalse(
                            dispatched.isDone(),
                            "ingress terminal must follow the disconnect callback terminal");
                    if (failCallback) {
                        entry.release.completeExceptionally(
                                new IllegalStateException("callback failure"));
                    } else {
                        entry.release.complete(null);
                    }
                    dispatched.get(TIMEOUT_SECONDS, TimeUnit.SECONDS);
                } finally {
                    entry.release.complete(null);
                }
            }
        }
    }

    @Test
    void disconnectOfAnotherBindingIsDroppedAndTheCurrentBindingRunsOnce() throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        var objects =
                options.addRouteMesh("mesh")
                        .listen("inproc://disconnect-binding-" + UUID.randomUUID())
                        .objects()
                        .server();
        objects.addEntrySpot(Entry.class);
        objects.addActorFactory(
                ACTOR_TYPE, Player.class, Factory.class, factory -> factory.disableRelocation());
        try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            runtime.actorManager()
                    .create(ACTOR_ID, ACTOR_TYPE)
                    .submit()
                    .toCompletableFuture()
                    .get(TIMEOUT_SECONDS, TimeUnit.SECONDS);
            var spots = (ZLinkSpotRuntime) runtime.spotManager();
            var actors = spots.actorSessions().runtime();
            var actor = spots.actorSessions().localActor(ACTOR_ID).orElseThrow();
            var entry = (Entry) spots.localActorSpotSurface(actor);
            entry.release.complete(null);
            var activationMethod =
                    ZLinkSpotRuntime.class.getDeclaredMethod(
                            "entrySpotActivationFor", String.class);
            activationMethod.setAccessible(true);
            var activation =
                    (SpotActivationBase<?>)
                            activationMethod.invoke(spots, entry.context().spotId());
            RoutingId sessionOwner = RoutingId.from("session-owner-node");
            RoutingId currentSession = RoutingId.from("current-session");
            Method bindSession =
                    ZLinkActorRuntime.class.getDeclaredMethod(
                            "bindSession",
                            ZLinkActor.class,
                            ZLinkBoundSession.class,
                            RoutingId.class,
                            RoutingId.class,
                            long.class);
            bindSession.setAccessible(true);
            ((CompletionStage<?>)
                            bindSession.invoke(
                                    actors,
                                    actor,
                                    new IdleBoundSession(),
                                    sessionOwner,
                                    currentSession,
                                    7L))
                    .toCompletableFuture()
                    .get(TIMEOUT_SECONDS, TimeUnit.SECONDS);
            var ref = actors.currentRef(actor);

            //  Session-Actor binding §8.3: a notification for a binding that is not the
            //  Actor's current binding is dropped.
            dispatchDisconnect(activation, ref, sessionOwner, RoutingId.from("replaced-session"));
            assertEquals(0, entry.callbacks.get());

            dispatchDisconnect(activation, ref, sessionOwner, currentSession);
            assertEquals(1, entry.callbacks.get());
        }
    }

    private static void dispatchDisconnect(
            SpotActivationBase<?> activation,
            ZLinkBackendActorRef ref,
            RoutingId sourceNodeRid,
            RoutingId sourceSessionRid)
            throws Exception {
        try (var header =
                        new ZLinkBackendActorReceived(
                                ref,
                                sourceNodeRid,
                                sourceSessionRid,
                                Optional.empty(),
                                0,
                                0,
                                Message.from(
                                        ZLinkActorSpotRoutePackets
                                                .SESSION_DISCONNECTED_PACKET_NAME),
                                true,
                                null,
                                null);
                var body =
                        new ZLinkBackendActorReceived(
                                ref,
                                sourceNodeRid,
                                sourceSessionRid,
                                Optional.empty(),
                                0,
                                0,
                                Message.from(new byte[0]),
                                false,
                                null,
                                null)) {
            activation
                    .dispatchActorMessages(List.of(header, body))
                    .toCompletableFuture()
                    .get(TIMEOUT_SECONDS, TimeUnit.SECONDS);
        }
    }

    private static final class IdleBoundSession implements ZLinkBoundSession {
        @Override
        public ZLinkBoundSessionSendCall send(Object message) {
            throw new UnsupportedOperationException("not used");
        }

        @Override
        public CompletionStage<Void> disconnect() {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class Entry implements ZLinkEntrySpot<ZLinkActor> {
        private final ZLinkEntrySpotContext context;
        final CompletableFuture<Void> entered = new CompletableFuture<>();
        final CompletableFuture<Void> release = new CompletableFuture<>();
        final AtomicInteger callbacks = new AtomicInteger();

        public Entry(ZLinkEntrySpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkEntrySpotContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDisconnectActor(ZLinkActor actor) {
            callbacks.incrementAndGet();
            entered.complete(null);
            return release;
        }
    }

    public record Player(ZLinkActorContext context) implements ZLinkActor {}

    public static final class Factory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new Player(context));
        }
    }
}
