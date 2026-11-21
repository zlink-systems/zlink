package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.RepeatedTest;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.binding.ZLinkStreamAdmissionTestFixture;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;
import systems.zlink.framework.runtime.streams.*;
import systems.zlink.framework.spots.*;
import systems.zlink.framework.streams.*;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.util.*;
import java.util.concurrent.*;

final class ZLinkBoundSessionEgressOrderTest {

    @RepeatedTest(40)
    void replyRegistersBeforeTheNextTurnClosesEvenWhenDispatchPublicationIsHeld() throws Exception {
        exercise(false);
    }

    @RepeatedTest(40)
    void pushRegistersBeforeTheNextTurnClosesWithoutWaitingForTransport() throws Exception {
        exercise(true);
    }

    private void exercise(boolean push) throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        var server =
                options.addRouteMesh("egress-order")
                        .listen("inproc://egress-order-" + UUID.randomUUID())
                        .objects()
                        .server();
        server.addEntrySpot(Lobby.class);
        server.addActorFactory(
                "player", Player.class, Factory.class, factory -> factory.disableRelocation());
        var publishDispatch = new CompletableFuture<Void>();
        var dispatchTerminal = new CompletableFuture<Void>();
        var physicalTerminal = new CompletableFuture<Void>();
        var pushTerminal = new CompletableFuture<Void>();
        var releaseAdmission = new CompletableFuture<Void>();
        var closeRequested = new CompletableFuture<Void>();
        var observations = new CopyOnWriteArrayList<String>();
        var ready = new CompletableFuture<RoutingId>();
        try (var host = ZLinkFrameworkRuntimeTestAccess.start(options);
                var context = systems.zlink.contracts.core.Zlink.createContext();
                var physical =
                        ZLinkStreamAdmissionTestFixture.create(
                                context,
                                send ->
                                        (socket, rid, parts, timeout) -> {
                                            var retained =
                                                    parts.stream().map(Message::from).toList();
                                            return releaseAdmission
                                                    .thenCompose(
                                                            ignored ->
                                                                    send.send(
                                                                            socket, rid, retained,
                                                                            timeout))
                                                    .thenRun(() -> observations.add("admitted"))
                                                    .whenComplete(
                                                            (ignored, failure) ->
                                                                    retained.forEach(
                                                                            Message::close));
                                        })) {
            physical.onTransportError(
                    (rid, event, code, message) -> {
                        if (event
                                == systems.zlink.contracts.eventing.MonitorEventType
                                        .CONNECTION_READY) ready.complete(rid);
                    });
            physical.bind("tcp://127.0.0.1:0");
            var endpoint = java.net.URI.create(physical.lastEndpoint());
            try (var client = new java.net.Socket(endpoint.getHost(), endpoint.getPort())) {
                client.setSoTimeout(5000);
                var session = ready.get(5, TimeUnit.SECONDS);
                host.actorManager()
                        .create("player", "player")
                        .submit()
                        .toCompletableFuture()
                        .get(5, TimeUnit.SECONDS);
                var actors = ZLinkFrameworkRuntimeTestAccess.actors(host);
                var spots = ZLinkFrameworkRuntimeTestAccess.spots(host);
                var player = actors.localActor("player").orElseThrow();
                var ref = actors.currentRef(player);
                ZLinkBackendStreamSocket stream =
                        (ZLinkBackendStreamSocket)
                                Proxy.newProxyInstance(
                                        ZLinkBackendStreamSocket.class.getClassLoader(),
                                        new Class<?>[] {ZLinkBackendStreamSocket.class},
                                        (proxy, method, arguments) -> {
                                            if (method.getName().equals("replyAsync")) {
                                                observations.add("reply");
                                                @SuppressWarnings("unchecked")
                                                var submitted =
                                                        (CompletionStage<Void>)
                                                                method.invoke(physical, arguments);
                                                dispatchTerminal.complete(null);
                                                return submitted.thenCompose(
                                                        ignored -> physicalTerminal);
                                            }
                                            throw new AssertionError(method);
                                        });
                ZLinkInternalSpotNode node =
                        (ZLinkInternalSpotNode)
                                Proxy.newProxyInstance(
                                        ZLinkInternalSpotNode.class.getClassLoader(),
                                        new Class<?>[] {ZLinkInternalSpotNode.class},
                                        (proxy, method, arguments) ->
                                                switch (method.getName()) {
                                                    case "boundSessionRoute" -> Optional.empty();
                                                    case "hasRemoteActorBoundSessionRoute" -> false;
                                                    case "hasLocalActorBoundSessionRoute" -> true;
                                                    case "sendLocalActorBoundSessionAsync" -> {
                                                        observations.add("push");
                                                        @SuppressWarnings("unchecked")
                                                        var parts = (List<Message>) arguments[1];
                                                        yield physical.sendBoundSessionPushAsync(
                                                                        session, 1, parts)
                                                                .thenCompose(
                                                                        ignored ->
                                                                                physicalTerminal);
                                                    }
                                                    case "closeActorBoundSessionAsync" -> {
                                                        observations.add("close-requested");
                                                        var close =
                                                                physical
                                                                        .disconnectBoundSessionPeerAsync(
                                                                                session, 1);
                                                        closeRequested.complete(null);
                                                        yield close.thenRun(
                                                                () -> observations.add("EOF"));
                                                    }
                                                    default -> throw new AssertionError(method);
                                                });
                long bindingToken =
                        actors.bindNativeSession(player, node, ref, ref.nodeRid(), session)
                                .toCompletableFuture()
                                .get(5, TimeUnit.SECONDS);
                try {
                    // Hold dispatch publication outside the actual Actor turn. Forwarding the
                    // runtime signature lets the same fixture test the old and new dispatchers.
                    var dispatcher =
                            (ZLinkSessionActorsRuntime.LocalActorDispatcher)
                                    Proxy.newProxyInstance(
                                            ZLinkSessionActorsRuntime.LocalActorDispatcher.class
                                                    .getClassLoader(),
                                            new Class<?>[] {
                                                ZLinkSessionActorsRuntime.LocalActorDispatcher.class
                                            },
                                            (proxy, method, arguments) -> {
                                                CompletionStage<?> stage;
                                                try {
                                                    stage =
                                                            (CompletionStage<?>)
                                                                    spots.getClass()
                                                                            .getMethod(
                                                                                    "dispatchLocalSessionActor",
                                                                                    method
                                                                                            .getParameterTypes())
                                                                            .invoke(
                                                                                    spots,
                                                                                    arguments);
                                                } catch (InvocationTargetException failure) {
                                                    throw failure.getCause();
                                                }
                                                return stage.thenCompose(
                                                        value -> {
                                                            dispatchTerminal.complete(null);
                                                            return publishDispatch.thenApply(
                                                                    ignored -> value);
                                                        });
                                            });
                    var headers = new ZLinkSessionRelayHeaders();
                    var bound =
                            new ZLinkBoundActor(
                                    stream,
                                    session,
                                    ref,
                                    "egress-order",
                                    Optional.of(player),
                                    actors,
                                    new ZLinkJsonMessageSerializer(),
                                    0,
                                    new ZLinkSessionDisconnected(ref.nodeRid(), 1, 1),
                                    dispatcher,
                                    true,
                                    ZLinkStreamCodec.JSON,
                                    headers,
                                    null,
                                    () -> true,
                                    operation -> operation.apply(1),
                                    ZLinkRelayMetadataPolicy.EMPTY);
                    CompletionStage<Void> first;
                    if (push) {
                        first =
                                actors.submitActorDispatch(
                                        "player",
                                        () -> {
                                            player.context()
                                                    .boundSession()
                                                    .send(new Reply("push"))
                                                    .submit()
                                                    .whenComplete(
                                                            (ignored, failure) -> {
                                                                if (failure == null)
                                                                    pushTerminal.complete(null);
                                                                else
                                                                    pushTerminal
                                                                            .completeExceptionally(
                                                                                    failure);
                                                            });
                                            return CompletableFuture.completedFuture(null);
                                        });
                        first.toCompletableFuture().get(5, TimeUnit.SECONDS);
                    } else {
                        headers.enter(
                                new ZLinkStreamHeader(
                                        ZLinkStreamMessageKind.REQUEST,
                                        ZLinkStreamCodec.JSON,
                                        EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                                        Optional.of(7L),
                                        "Probe",
                                        Map.of()));
                        try {
                            first = bound.relay(ZLinkMessage.of(new Probe()));
                        } finally {
                            headers.exit();
                        }
                        dispatchTerminal.get(5, TimeUnit.SECONDS);
                    }
                    var closed =
                            actors.submitActorDispatch(
                                    "player", () -> player.context().boundSession().disconnect());
                    closeRequested.get(5, TimeUnit.SECONDS);
                    assertFalse(
                            physicalTerminal.isDone(),
                            "the next Actor turn must run before transport completion");
                    assertFalse(closed.toCompletableFuture().isDone());
                    releaseAdmission.complete(null);
                    closed.toCompletableFuture().get(5, TimeUnit.SECONDS);
                    physicalTerminal.complete(null);
                    publishDispatch.complete(null);
                    byte[] received = client.getInputStream().readAllBytes();
                    assertTrue(
                            received.length > 0,
                            () -> "client saw EOF before payload; submissions=" + observations);
                    first.toCompletableFuture().get(5, TimeUnit.SECONDS);
                    if (push) pushTerminal.get(5, TimeUnit.SECONDS);
                    assertEquals(
                            List.of(push ? "push" : "reply", "close-requested", "admitted", "EOF"),
                            observations);
                    var decoded = ZLinkStreamFrameCodec.tryDecode(received);
                    assertTrue(
                            decoded.isPresent(),
                            () ->
                                    "invalid STREAM frame bytes="
                                            + java.util.HexFormat.of().formatHex(received));
                    assertEquals(
                            new Reply(push ? "push" : "reply"),
                            new ZLinkJsonMessageSerializer()
                                    .deserializeBorrowed(
                                            java.nio.ByteBuffer.wrap(decoded.orElseThrow().body()),
                                            Reply.class));
                } finally {
                    releaseAdmission.complete(null);
                    publishDispatch.complete(null);
                    physicalTerminal.complete(null);
                    actors.clearSessionBinding(player, bindingToken)
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS);
                }
            }
        } finally {
            releaseAdmission.complete(null);
            publishDispatch.complete(null);
            physicalTerminal.complete(null);
        }
    }

    public record Probe() {}

    public record Reply(String value) {}

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

    public static final class Lobby implements ZLinkEntrySpot<Player> {
        private final ZLinkEntrySpotContext context;

        public Lobby(ZLinkEntrySpotContext context) {
            this.context = context;
        }

        public ZLinkEntrySpotContext context() {
            return context;
        }

        public void configure() {
            context.handlers().addHandler(ProbeHandler.class);
        }

        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class ProbeHandler
            implements ZLinkEntrySpotActorRequestHandler<Lobby, Player, Probe, Reply> {
        public CompletionStage<Reply> handle(
                Lobby spot, Player actor, ZLinkMessageContext context, Probe request) {
            return CompletableFuture.completedFuture(new Reply("reply"));
        }
    }
}
