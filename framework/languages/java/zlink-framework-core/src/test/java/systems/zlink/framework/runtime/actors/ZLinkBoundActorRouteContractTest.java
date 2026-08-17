package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.util.Optional;
import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.util.EnumSet;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamSocket;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamFrameCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.actors.ZLinkActor;

final class ZLinkBoundActorRouteContractTest {
    @Test
    void relocationRouteSwitchRejectsAnotherObjectGeneration() {
        ZLinkBoundActor actor = actor(true);

        assertThrows(
            ZLinkConfigurationException.class,
            () -> actor.rebindNativeActor(new ZLinkBackendActorRef(
                RoutingId.from("actor-node-b"), "actor-1", 8)));
    }

    @Test
    void staleBindingDisconnectIsIdempotentAndDoesNotTouchTheReplacement() {
        ZLinkBoundActor actor = actor(false);

        var first = actor.notifyDisconnected();
        var second = actor.notifyDisconnected();

        assertSame(first, second);
        assertDoesNotThrow(() -> first.toCompletableFuture().join());
    }

    @Test
    void localReplyUsesItsDeclaredCodecInsteadOfEchoingTheRequestCodec() {
        AtomicReference<ZLinkStreamHeader> sentHeader = new AtomicReference<>();
        AtomicReference<String> sentPayload = new AtomicReference<>();
        ZLinkBackendStreamSocket stream = (ZLinkBackendStreamSocket)
            Proxy.newProxyInstance(
                ZLinkBackendStreamSocket.class.getClassLoader(),
                new Class<?>[] {ZLinkBackendStreamSocket.class},
                (proxy, method, arguments) -> {
                    if (method.getName().equals("replyAsync")
                        && arguments[1] instanceof ZLinkStreamHeader header) {
                        sentHeader.set(header);
                        @SuppressWarnings("unchecked")
                        var parts = (java.util.List<Message>) arguments[2];
                        sentPayload.set(parts.getFirst().toUtf8String());
                        return CompletableFuture.completedFuture(null);
                    }
                    throw new UnsupportedOperationException(method.getName());
                });
        ZLinkSessionRelayHeaders relayHeaders = new ZLinkSessionRelayHeaders();
        ZLinkActor managed = () -> null;
        ZLinkBoundActor actor = new ZLinkBoundActor(
            stream,
            RoutingId.from("session"),
            new ZLinkBackendActorRef(
                RoutingId.from("actor-node-a"), "actor-1", 7),
            "game",
            Optional.of(managed),
            null,
            new RawSerializer(),
            0,
            1,
            ignored -> true,
            (ignoredActor, ignoredSequence, ignoredHeader, ignoredPayload) ->
                CompletableFuture.completedFuture(Optional.of(
                    new ZLinkSessionActorsRuntime.LocalActorReply(
                        Message.from("custom-reply".getBytes(StandardCharsets.UTF_8)),
                        ZLinkStreamCodec.PROTOBUF))),
            true,
            ZLinkStreamCodec.JSON,
            relayHeaders,
            null,
            () -> true,
            operation -> operation.apply(1),
            ZLinkRelayMetadataPolicy.EMPTY);
        relayHeaders.enter(new ZLinkStreamHeader(
            ZLinkStreamMessageKind.REQUEST,
            ZLinkStreamCodec.JSON,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.of(41L),
            "Request",
            Map.of()));
        try {
            actor.relay(ZLinkMessage.of("request"))
                .toCompletableFuture()
                .join();
        } finally {
            relayHeaders.exit();
        }

        assertEquals(ZLinkStreamCodec.PROTOBUF, sentHeader.get().codec());
        assertEquals("custom-reply", sentPayload.get());
    }

    @Test
    void remoteOneWayRelayKeepsTheStrictHeaderAndPayloadActorPacket() {
        AtomicReference<ZLinkStreamCodec> sentCodec = new AtomicReference<>();
        AtomicReference<Integer> sentPayloadPartCount = new AtomicReference<>();
        ZLinkBackendStreamSocket stream = (ZLinkBackendStreamSocket)
            Proxy.newProxyInstance(
                ZLinkBackendStreamSocket.class.getClassLoader(),
                new Class<?>[] {ZLinkBackendStreamSocket.class},
                (proxy, method, arguments) -> {
                    if (method.getName().equals("relayBoundActorAsync")
                        && arguments.length == 5
                        && arguments[3] instanceof ZLinkStreamHeader header) {
                        @SuppressWarnings("unchecked")
                        var payloadParts = (List<Message>) arguments[4];
                        sentCodec.set(header.codec());
                        sentPayloadPartCount.set(payloadParts.size());
                        return CompletableFuture.completedFuture(null);
                    }
                    throw new UnsupportedOperationException(method.getName());
                });
        ZLinkCodecRegistration codecs = new ZLinkCodecRegistration();
        codecs.addSerializer("application/x-protobuf", new RawSerializer());
        codecs.addStreamCodec(
            "application/x-protobuf", ZLinkStreamCodec.PROTOBUF);
        codecs.freeze();
        ZLinkMessageSerializer serializer =
            codecs.serializerWithFallback(new RawSerializer());
        ZLinkSessionRelayHeaders relayHeaders = new ZLinkSessionRelayHeaders();
        ZLinkBoundActor actor = new ZLinkBoundActor(
            stream,
            RoutingId.from("session"),
            new ZLinkBackendActorRef(
                RoutingId.from("actor-node-a"), "actor-1", 7),
            "game",
            Optional.empty(),
            null,
            serializer,
            0,
            1,
            ignored -> true,
            null,
            true,
            ZLinkStreamCodec.PROTOBUF,
            relayHeaders,
            null,
            () -> true,
            operation -> operation.apply(1),
            ZLinkRelayMetadataPolicy.EMPTY);
        relayHeaders.enter(new ZLinkStreamHeader(
            ZLinkStreamMessageKind.SEND,
            ZLinkStreamCodec.PROTOBUF,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.empty(),
            "MatchBingoReq",
            Map.of()));
        try {
            actor.relay(ZLinkMessage.fromEncoded(
                    ZLinkEncodedPayload.from(
                        "protobuf".getBytes(StandardCharsets.UTF_8)),
                    serializer))
                .toCompletableFuture()
                .join();
        } finally {
            relayHeaders.exit();
        }

        assertEquals(ZLinkStreamCodec.PROTOBUF, sentCodec.get());
        assertEquals(1, sentPayloadPartCount.get());
    }

    @Test
    void remoteRelayPreservesOneWayAndRequestReplyFrameKinds() {
        AtomicInteger relays = new AtomicInteger();
        AtomicInteger requests = new AtomicInteger();
        AtomicInteger replies = new AtomicInteger();
        AtomicReference<ZLinkStreamHeader> clientReply = new AtomicReference<>();
        ZLinkBackendStreamSocket stream = (ZLinkBackendStreamSocket)
            Proxy.newProxyInstance(
                ZLinkBackendStreamSocket.class.getClassLoader(),
                new Class<?>[] {ZLinkBackendStreamSocket.class},
                (proxy, method, arguments) -> switch (method.getName()) {
                    case "relayBoundActorAsync" -> {
                        ZLinkStreamHeader header =
                            (ZLinkStreamHeader) arguments[3];
                        assertEquals(ZLinkStreamMessageKind.SEND, header.kind());
                        relays.incrementAndGet();
                        yield CompletableFuture.completedFuture(null);
                    }
                    case "requestBoundActor" -> {
                        ZLinkStreamHeader header =
                            (ZLinkStreamHeader) arguments[3];
                        assertEquals(
                            ZLinkStreamMessageKind.REQUEST, header.kind());
                        assertEquals(1L, arguments[2]);
                        requests.incrementAndGet();
                        ZLinkStreamHeader response =
                            ZLinkStreamHeader.createResponse(
                                header,
                                ZLinkStreamCodec.JSON,
                                EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                                header.packetName(),
                                Map.of());
                        yield CompletableFuture.completedFuture(List.of(
                            Message.from(ZLinkStreamFrameCodec.encode(
                                response,
                                "reply".getBytes(StandardCharsets.UTF_8)))));
                    }
                    case "replyAsync" -> {
                        ZLinkStreamHeader header =
                            (ZLinkStreamHeader) arguments[1];
                        clientReply.set(header);
                        replies.incrementAndGet();
                        yield CompletableFuture.completedFuture(null);
                    }
                    default -> throw new UnsupportedOperationException(
                        method.getName());
                });
        ZLinkSessionRelayHeaders relayHeaders = new ZLinkSessionRelayHeaders();
        ZLinkBoundActor actor = new ZLinkBoundActor(
            stream,
            RoutingId.from("session"),
            new ZLinkBackendActorRef(
                RoutingId.from("actor-node-a"), "actor-1", 7),
            "game",
            Optional.empty(),
            null,
            new RawSerializer(),
            0,
            1,
            ignored -> true,
            null,
            true,
            ZLinkStreamCodec.JSON,
            relayHeaders,
            null,
            () -> true,
            operation -> operation.apply(1),
            ZLinkRelayMetadataPolicy.EMPTY);

        relayHeaders.enter(new ZLinkStreamHeader(
            ZLinkStreamMessageKind.SEND,
            ZLinkStreamCodec.JSON,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.empty(),
            "Push",
            Map.of()));
        try {
            actor.relay(ZLinkMessage.of("push"))
                .toCompletableFuture().join();
        } finally {
            relayHeaders.exit();
        }

        relayHeaders.enter(new ZLinkStreamHeader(
            ZLinkStreamMessageKind.REQUEST,
            ZLinkStreamCodec.JSON,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.of(41L),
            "Request",
            Map.of()));
        try {
            actor.relay(ZLinkMessage.of("request"))
                .toCompletableFuture().join();
        } finally {
            relayHeaders.exit();
        }

        assertEquals(1, relays.get());
        assertEquals(1, requests.get());
        assertEquals(1, replies.get());
        assertEquals(ZLinkStreamMessageKind.RESPONSE,
            clientReply.get().kind());
        assertEquals(Optional.of(41L),
            clientReply.get().requestSequence());
    }

    private static ZLinkBoundActor actor(boolean currentBinding) {
        return new ZLinkBoundActor(
            null,
            RoutingId.from("session"),
            new ZLinkBackendActorRef(
                RoutingId.from("actor-node-a"), "actor-1", 7),
            "game",
            Optional.empty(),
            null,
            new UnsupportedSerializer(),
            0,
            1,
            ignored -> true,
            null,
            true,
            ZLinkStreamCodec.JSON,
            new ZLinkSessionRelayHeaders(),
            null,
            () -> currentBinding,
            operation -> operation.apply(1),
            ZLinkRelayMetadataPolicy.EMPTY);
    }

    private static final class UnsupportedSerializer implements ZLinkMessageSerializer {
        @Override
        public <T> ZLinkEncodedPayload serialize(T value) {
            throw new UnsupportedOperationException();
        }

        @Override
        public <T> T deserialize(ZLinkEncodedPayload payload, Class<T> type) {
            throw new UnsupportedOperationException();
        }
    }

    private static final class RawSerializer implements ZLinkMessageSerializer {
        @Override
        public <T> ZLinkEncodedPayload serialize(T value) {
            return ZLinkEncodedPayload.from(
                value.toString().getBytes(StandardCharsets.UTF_8));
        }

        @Override
        public <T> T deserialize(ZLinkEncodedPayload payload, Class<T> type) {
            throw new UnsupportedOperationException();
        }
    }
}
