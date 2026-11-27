package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkActorJoinCompletion;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.runtime.actors.ZLinkActorEntryTransferEnvelope;
import systems.zlink.framework.runtime.actors.ZLinkActorSpotRoutePackets;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddressResolver;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.streams.ZLinkStreamFrameCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotActorMembershipLifecycle;
import systems.zlink.framework.spots.ZLinkSpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkSpotActorSendHandler;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

import zlink.framework.channel.v1.ChannelHeaderV1.ChannelHeader;
import zlink.framework.channel.v1.ChannelHeaderV1.ChannelKind;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.util.EnumSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkRemoteJoinedActorChannelHeaderTest {
    private static final String MESH = "actor-packet";
    private static final String SPOT_ID = "table";
    private static final String ACTOR_ID = "player";
    private static final String PACKET_NAME = "__zlink.actor.packet";
    private static final String PAYLOAD = "{\"value\":\"remote input\"}";

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void remoteJoinedActorPacketUsesChannelHeaderAndDispatchesOnce(boolean request)
            throws Exception {
        var locations = new ZLinkInMemoryLocationStore();
        var wire = new CompletableFuture<List<byte[]>>();
        var sourceMesh = new AtomicReference<ZLinkInternalMeshNode>();
        var sourceBackend =
                ZLinkFrameworkRuntimeTestAccess.withMeshNodeDecorator(
                        new ZLinkJavaBackendAdapterFactory(),
                        mesh -> {
                            sourceMesh.set(mesh);
                            return observe(mesh, wire);
                        });
        try (var target = ZLinkFrameworkRuntimeTestAccess.start(options(locations, null));
                var source =
                        ZLinkFrameworkRuntimeTestAccess.start(
                                options(
                                        locations,
                                        target.listenerStatus(ZLinkListenerKind.ROUTE_MESH, MESH)
                                                .endpoint()),
                                sourceBackend)) {
            await(ZLinkFrameworkRuntimeTestAccess.startupCompletion(target));
            await(ZLinkFrameworkRuntimeTestAccess.startupCompletion(source));
            await(target.spotManager().getOrCreate(SPOT_ID, "table").submit());
            await(target.actorManager().create(ACTOR_ID, "player").submit());
            var targetActors = ZLinkFrameworkRuntimeTestAccess.actors(target);
            var player = (Player) targetActors.localActor(ACTOR_ID).orElseThrow();
            await(target.actorClient().requestToActor(ACTOR_ID, new Join()).submit(String.class));
            assertInstanceOf(ZLinkActorJoinCompletion.Accepted.class, await(player.joined));

            var handles = source.spotHandleResolver();
            var handle = await(handles.resolveSpotHandle(MESH, SPOT_ID)).orElseThrow();
            var address =
                    await(((SpotTransportAddressResolver) handles).resolve(handle)).orElseThrow();
            var mesh = sourceMesh.get();
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(10);
            while (!mesh.observePeerRoute(address.targetNodeRid()).ready()) {
                assertTrue(System.nanoTime() < deadline, "remote mesh peer did not become ready");
                Thread.sleep(10);
            }
            // The low-level Actor route takes the Spot fence from the sender's transport cache.
            mesh.spotNode()
                    .entrySpot()
                    .rememberSpotAuthority(
                            address.targetNodeRid(),
                            address.spotId(),
                            address.spotGeneration(),
                            address.authorityOwnerGeneration(),
                            address.ownerLeaseGeneration());

            var streamHeader = header(request);
            try (Message payload = Message.from(PAYLOAD)) {
                var result =
                        ZLinkFrameworkRuntimeTestAccess.actors(source)
                                .dispatchRemoteJoinedActor(
                                        targetActors.currentRef(player),
                                        SPOT_ID,
                                        streamHeader,
                                        payload);
                List<byte[]> parts = await(wire);
                assertEquals(2, parts.size());
                ChannelHeader outer = ChannelHeader.parseFrom(parts.getFirst());
                assertEquals(PACKET_NAME, outer.getMessageName());
                assertEquals(request ? ChannelKind.REQUEST : ChannelKind.COMMAND, outer.getKind());
                assertEquals("application/octet-stream", outer.getContentType());
                assertEquals(request, outer.hasCorrelationId());

                Optional<Message> reply = await(result);
                if (request) {
                    try (Message message = reply.orElseThrow()) {
                        var frame =
                                ZLinkStreamFrameCodec.tryDecode(message.toByteArray())
                                        .orElseThrow();
                        var replyHeader = ZLinkStreamHeaderCodec.decodeOrPlain(frame.header());
                        assertEquals(Optional.of(23L), replyHeader.requestSequence());
                        assertEquals(
                                "\"remote reply\"",
                                new String(frame.body(), StandardCharsets.UTF_8));
                    }
                } else {
                    assertTrue(reply.isEmpty());
                }
                assertEquals("remote input", await(player.received));
                assertEquals(1, player.calls.get());
            }
        }
    }

    @Test
    void rawActorNameIsRejectedWithAnOtherwiseValidPackedBody() {
        var actor = new ZLinkBackendActorRef(RoutingId.from("actor-owner"), ACTOR_ID, 1);
        try (Message payload = Message.from(PAYLOAD);
                Message rawName = Message.from(PACKET_NAME)) {
            var parts =
                    ZLinkActorSpotRoutePackets.createActorPacketParts(
                            actor, header(false), payload, null);
            try (var body = ZLinkActorEntryTransferEnvelope.encode(parts)) {
                var failure =
                        assertThrows(
                                ZLinkFrameworkException.class,
                                () -> ZLinkSpotRuntime.parsePacket(List.of(rawName, body)));
                assertEquals(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, failure.kind());
            } finally {
                parts.forEach(Message::close);
            }
        }
    }

    private static ZLinkStreamHeader header(boolean request) {
        return new ZLinkStreamHeader(
                request ? ZLinkStreamMessageKind.REQUEST : ZLinkStreamMessageKind.SEND,
                ZLinkStreamCodec.JSON,
                EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                request ? Optional.of(23L) : Optional.empty(),
                Payload.class.getSimpleName(),
                Map.of());
    }

    private static DefaultZLinkFrameworkOptions options(
            ZLinkInMemoryLocationStore locations, String targetEndpoint) {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(locations);
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        var mesh = options.addRouteMesh(MESH);
        mesh.listen("tcp://127.0.0.1:0").setRoutingIdPrefix("actor-packet");
        if (targetEndpoint != null) mesh.peerConnections().connect(targetEndpoint);
        var server = mesh.objects().server();
        server.addEntrySpot(Lobby.class);
        server.addActorFactory(
                "player",
                Player.class,
                PlayerFactory.class,
                factory -> factory.disableRelocation());
        if (targetEndpoint == null)
            server.addSpotFactory("table", Table.class, factory -> factory.disableRelocation());
        return options;
    }

    private static ZLinkInternalMeshNode observe(
            ZLinkInternalMeshNode mesh, CompletableFuture<List<byte[]>> wire) {
        var node = mesh.spotNode();
        var observedNode =
                proxy(
                        ZLinkInternalSpotNode.class,
                        (method, args) -> {
                            if (method.getName().equals("entrySpot")) {
                                return observeEntry(node.entrySpot(), wire);
                            }
                            return invoke(method, node, args);
                        });
        return proxy(
                ZLinkInternalMeshNode.class,
                (method, args) ->
                        method.getName().equals("spotNode")
                                ? observedNode
                                : invoke(method, mesh, args));
    }

    private static ZLinkBackendSpot observeEntry(
            ZLinkBackendSpot entry, CompletableFuture<List<byte[]>> wire) {
        return proxy(
                ZLinkBackendSpot.class,
                (method, args) -> {
                    int partsIndex =
                            switch (method.getName()) {
                                case "sendToSpot" -> args.length - 1;
                                case "requestToSpot" -> 5;
                                default -> -1;
                            };
                    if (partsIndex >= 0 && SPOT_ID.equals(args[1])) {
                        var parts = (List<?>) args[partsIndex];
                        wire.complete(
                                parts.stream()
                                        .map(Message.class::cast)
                                        .map(Message::toByteArray)
                                        .toList());
                    }
                    return invoke(method, entry, args);
                });
    }

    @FunctionalInterface
    private interface Invocation {
        Object call(Method method, Object[] args) throws Throwable;
    }

    private static <T> T proxy(Class<T> type, Invocation invocation) {
        return type.cast(
                Proxy.newProxyInstance(
                        type.getClassLoader(),
                        new Class<?>[] {type},
                        (proxy, method, args) -> invocation.call(method, args)));
    }

    private static Object invoke(Method method, Object target, Object[] args) throws Throwable {
        try {
            return method.invoke(target, args);
        } catch (InvocationTargetException failure) {
            throw failure.getCause();
        }
    }

    private static <T> T await(CompletionStage<T> stage) throws Exception {
        return stage.toCompletableFuture().get(10, TimeUnit.SECONDS);
    }

    public record Join() {}

    public record Payload(String value) {}

    public record Player(
            ZLinkActorContext context,
            CompletableFuture<ZLinkActorJoinCompletion> joined,
            CompletableFuture<String> received,
            AtomicInteger calls)
            implements ZLinkActor {
        @Override
        public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion completion) {
            joined.complete(completion);
            return CompletableFuture.completedFuture(null);
        }

        void record(Payload payload) {
            calls.incrementAndGet();
            received.complete(payload.value());
        }
    }

    public static final class PlayerFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(
                    new Player(
                            context,
                            new CompletableFuture<>(),
                            new CompletableFuture<>(),
                            new AtomicInteger()));
        }
    }

    private interface Membership extends ZLinkSpotActorMembershipLifecycle<Player> {
        @Override
        default CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        default CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public record Lobby(ZLinkEntrySpotContext context)
            implements ZLinkEntrySpot<Player>, Membership {
        @Override
        public void configure() {
            context.handlers().addHandler(JoinHandler.class);
        }
    }

    public static final class JoinHandler
            implements ZLinkEntrySpotActorRequestHandler<Lobby, Player, Join, String> {
        @Override
        public CompletionStage<String> handle(
                Lobby spot, Player actor, ZLinkMessageContext context, Join request) {
            actor.context().joinSpot(SPOT_ID).defer();
            return CompletableFuture.completedFuture("scheduled");
        }
    }

    public record Table(ZLinkSpotContext context) implements ZLinkSpot<Player>, Membership {
        @Override
        public void configure() {
            context.handlers().addHandler(SendHandler.class);
            context.handlers().addHandler(RequestHandler.class);
        }

        @Override
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
        }
    }

    public static final class SendHandler
            implements ZLinkSpotActorSendHandler<Table, Player, Payload> {
        @Override
        public CompletionStage<Void> handle(
                Table spot, Player actor, ZLinkMessageContext context, Payload message) {
            actor.record(message);
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class RequestHandler
            implements ZLinkSpotActorRequestHandler<Table, Player, Payload, String> {
        @Override
        public CompletionStage<String> handle(
                Table spot, Player actor, ZLinkMessageContext context, Payload message) {
            actor.record(message);
            return CompletableFuture.completedFuture("remote reply");
        }
    }
}
