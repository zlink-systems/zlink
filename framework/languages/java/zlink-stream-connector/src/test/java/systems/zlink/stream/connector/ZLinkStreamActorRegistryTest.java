package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;

import java.lang.reflect.Field;
import java.net.URI;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.Map;
import java.util.concurrent.CompletableFuture;

final class ZLinkStreamActorRegistryTest {
    @Test
    void boundAndUnboundOwnTheSnapshotCallbacksAndClosedHandleValidation() throws Exception {
        ZLinkStreamConnector connector = connector();
        ZLinkStreamActorRegistry registry = registry(connector);
        CompletableFuture<ZLinkStreamActor> bound = new CompletableFuture<>();
        CompletableFuture<ZLinkStreamActor> unbound = new CompletableFuture<>();
        connector.onActorBound(
                actor -> {
                    bound.complete(actor);
                    return CompletableFuture.completedFuture(null);
                });
        connector.onActorUnbound(
                actor -> {
                    unbound.complete(actor);
                    return CompletableFuture.completedFuture(null);
                });

        registry.bound(boundControl(7, "player-a"));
        ZLinkStreamActor actor = connector.actor("player-a").orElseThrow();

        assertEquals(1, connector.actors().size());
        assertSame(actor, connector.actors().getFirst());
        connector.dispatch().submit().toCompletableFuture().join();
        assertSame(actor, bound.join());

        registry.unbound(unboundControl(7));
        assertTrue(connector.actors().isEmpty());
        assertFalse(actor.isBound());
        connector.dispatch().submit().toCompletableFuture().join();
        assertSame(actor, unbound.join());
        registry.bound(boundControl(7, "player-b"));
        assertTrue(connector.actor("player-b").orElseThrow().isBound());
        ZLinkStreamException failure =
                assertThrows(ZLinkStreamException.class, () -> actor.send(payload()).submit());
        assertEquals(ZLinkStreamErrorCode.VALIDATION_FAILED, failure.errorCode());
        ZLinkStreamException rawTypedFailure =
                assertThrows(ZLinkStreamException.class, () -> actor.send((Object) payload()));
        assertEquals(ZLinkStreamErrorCode.VALIDATION_FAILED, rawTypedFailure.errorCode());
    }

    @Test
    void duplicateAndUnknownControlsAreDecodeFailures() throws Exception {
        ZLinkStreamActorRegistry registry = registry(connector());
        registry.bound(boundControl(7, "player-a"));

        assertThrows(
                IllegalArgumentException.class, () -> registry.bound(boundControl(7, "player-b")));
        assertThrows(
                IllegalArgumentException.class, () -> registry.bound(boundControl(8, "player-a")));
        assertThrows(IllegalArgumentException.class, () -> registry.unbound(unboundControl(9)));
        assertThrows(IllegalArgumentException.class, () -> registry.bound(new byte[] {1, 0, 1, 0}));
    }

    private static ZLinkStreamConnector connector() {
        return ZLinkStreamConnectorFactory.create(
                ZLinkStreamConnectorOptions.createDefault(URI.create("tcp://127.0.0.1:1")));
    }

    private static ZLinkStreamActorRegistry registry(ZLinkStreamConnector connector)
            throws Exception {
        Field field = DefaultZLinkStreamConnector.class.getDeclaredField("actorRegistry");
        field.setAccessible(true);
        return (ZLinkStreamActorRegistry) field.get(connector);
    }

    private static byte[] boundControl(int slot, String actorId) {
        byte[] id = actorId.getBytes(StandardCharsets.UTF_8);
        return ByteBuffer.allocate(4 + id.length)
                .put((byte) 1)
                .putShort((short) slot)
                .put((byte) id.length)
                .put(id)
                .array();
    }

    private static byte[] unboundControl(int slot) {
        return ByteBuffer.allocate(3).put((byte) 1).putShort((short) slot).array();
    }

    private static ZLinkStreamEncodedPayload payload() {
        return new ZLinkStreamEncodedPayload(
                "Ping", Message.from(new byte[] {1}), Map.of(), ZLinkStreamCodec.RAW);
    }
}
