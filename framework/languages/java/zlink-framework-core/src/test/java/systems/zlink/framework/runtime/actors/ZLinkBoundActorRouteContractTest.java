package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.util.Optional;
import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.util.EnumSet;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamSocket;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
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
                    if (method.getName().equals("reply")
                        && arguments[1] instanceof ZLinkStreamHeader header) {
                        sentHeader.set(header);
                        @SuppressWarnings("unchecked")
                        var parts = (java.util.List<Message>) arguments[2];
                        sentPayload.set(parts.getFirst().toUtf8String());
                        return true;
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
