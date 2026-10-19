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
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.actors.ZLinkActorSpotRoutePackets;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;

import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.List;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

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
                                    Message.from(
                                            identity(
                                                    RoutingId.from("session-owner-node"), -1L, 7L)),
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
    void canonicalDisconnectWithBodyIdentityRunsWithoutCurrentBinding() throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        var objects =
                options.addRouteMesh("mesh")
                        .listen("inproc://disconnect-canonical-" + UUID.randomUUID())
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
            entry.release.complete(null);
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
                                    RoutingId.from("forwarder"),
                                    null,
                                    Optional.empty(),
                                    0,
                                    0,
                                    Message.from("zlink.framework.actor.session_disconnected"),
                                    true,
                                    null,
                                    null);
                    var body =
                            new ZLinkBackendActorReceived(
                                    ref,
                                    RoutingId.from("forwarder"),
                                    null,
                                    Optional.empty(),
                                    0,
                                    0,
                                    Message.from(
                                            java.util.HexFormat.of()
                                                    .parseHex(
                                                            "066e6f64652d6100000000000000020000000000000007")),
                                    false,
                                    null,
                                    null)) {
                activation
                        .dispatchActorMessages(List.of(header, body))
                        .toCompletableFuture()
                        .get(TIMEOUT_SECONDS, TimeUnit.SECONDS);
            }
            assertEquals(1, entry.callbacks.get());
            var warnings =
                    new java.util.concurrent.CopyOnWriteArrayList<java.util.logging.LogRecord>();
            var logger =
                    java.util.logging.Logger.getLogger(
                            systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer.class
                                    .getName());
            var handler =
                    new java.util.logging.Handler() {
                        @Override
                        public void publish(java.util.logging.LogRecord record) {
                            warnings.add(record);
                        }

                        @Override
                        public void flush() {}

                        @Override
                        public void close() {}
                    };
            logger.addHandler(handler);
            try {
                var golden =
                        new com.fasterxml.jackson.databind.ObjectMapper()
                                .readTree(
                                        ZLinkActorDisconnectIngressTest.class.getResourceAsStream(
                                                "/session-disconnected-v1.json"));
                for (var rejected : golden.path("rejected")) {
                    dispatchDisconnect(
                            activation,
                            ref,
                            java.util.HexFormat.of().parseHex(rejected.path("hex").asText()));
                }
                byte[] zeroLifecycle = identity(RoutingId.from("node-a"), 0L, 7L);
                byte[] zeroBinding = identity(RoutingId.from("node-a"), 2L, 0L);
                dispatchDisconnect(activation, ref, zeroLifecycle);
                dispatchDisconnect(activation, ref, zeroBinding);
                assertEquals(1, entry.callbacks.get());
                var malformed =
                        warnings.stream()
                                .filter(
                                        record ->
                                                record.getMessage()
                                                        .contains("reason=invalid_frame"))
                                .toList();
                assertEquals(7, malformed.size());
                for (var warning : malformed)
                    assertEquals(java.util.logging.Level.WARNING, warning.getLevel());
            } finally {
                logger.removeHandler(handler);
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
            var ref = actors.currentRef(actor);
            var binding =
                    new AtomicReference<>(
                            new ZLinkInternalSpotNode.BoundSessionRoute(
                                    sessionOwner, -1L, "session-owner", 1L, currentSession, 7L));
            var node =
                    (ZLinkInternalSpotNode)
                            Proxy.newProxyInstance(
                                    getClass().getClassLoader(),
                                    new Class<?>[] {ZLinkInternalSpotNode.class},
                                    (proxy, method, arguments) ->
                                            switch (method.getName()) {
                                                case "boundSessionRoute" ->
                                                        Optional.ofNullable(binding.get());
                                                case "routingId" -> ref.nodeRid();
                                                case "hashCode" -> System.identityHashCode(proxy);
                                                case "equals" -> proxy == arguments[0];
                                                case "toString" -> "disconnect-binding-node";
                                                case "closeActorBoundSessionAsync" -> {
                                                    binding.set(null);
                                                    yield CompletableFuture.completedFuture(null);
                                                }
                                                default ->
                                                        throw new UnsupportedOperationException(
                                                                method.getName());
                                            });
            actors.bindNativeSession(actor, node, ref, sessionOwner, currentSession, 7L, -1L)
                    .toCompletableFuture()
                    .get(TIMEOUT_SECONDS, TimeUnit.SECONDS);

            dispatchDisconnect(activation, ref, identity(RoutingId.from("other-owner"), -1L, 7L));
            dispatchDisconnect(activation, ref, identity(sessionOwner, 2L, 7L));
            dispatchDisconnect(activation, ref, identity(sessionOwner, -1L, 8L));
            assertEquals(0, entry.callbacks.get());
            dispatchDisconnect(activation, ref, identity(sessionOwner, -1L, 7L));
            assertEquals(1, entry.callbacks.get());
            long oldToken =
                    actors.bindNativeSession(
                                    actor, node, ref, sessionOwner, currentSession, 7L, -1L)
                            .toCompletableFuture()
                            .join();
            actors.clearSessionBinding(actor, oldToken).toCompletableFuture().join();
            binding.set(
                    new ZLinkInternalSpotNode.BoundSessionRoute(
                            sessionOwner, 2L, "session-owner", 1L, currentSession, 9L));
            actors.bindNativeSession(actor, node, ref, sessionOwner, currentSession, 9L, 2L)
                    .toCompletableFuture()
                    .join();
            Method localNotify =
                    ZLinkActorRuntime.class.getDeclaredMethod(
                            "notifyDisconnected",
                            ZLinkActor.class,
                            systems.zlink.framework.runtime.actors.ZLinkSessionDisconnected.class);
            localNotify.setAccessible(true);
            ((CompletionStage<?>)
                            localNotify.invoke(
                                    actors,
                                    actor,
                                    new systems.zlink.framework.runtime.actors
                                            .ZLinkSessionDisconnected(sessionOwner, -1L, 7L)))
                    .toCompletableFuture()
                    .join();
            assertEquals(
                    1,
                    entry.callbacks.get(),
                    "local clear then replacement must fence the old callback");
        }
    }

    private static void dispatchDisconnect(
            SpotActivationBase<?> activation, ZLinkBackendActorRef ref, byte[] identity)
            throws Exception {
        RoutingId sourceNodeRid = RoutingId.from("message-follow-forwarder");
        RoutingId sourceSessionRid = RoutingId.from("transport-session");
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
                                Message.from(identity),
                                false,
                                null,
                                null)) {
            activation
                    .dispatchActorMessages(List.of(header, body))
                    .toCompletableFuture()
                    .get(TIMEOUT_SECONDS, TimeUnit.SECONDS);
        }
    }

    private static byte[] identity(RoutingId owner, long lifecycle, long binding) {
        byte[] rid = owner.toBytes();
        return java.nio.ByteBuffer.allocate(Byte.BYTES + rid.length + Long.BYTES * 2)
                .put((byte) rid.length)
                .put(rid)
                .putLong(lifecycle)
                .putLong(binding)
                .array();
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
