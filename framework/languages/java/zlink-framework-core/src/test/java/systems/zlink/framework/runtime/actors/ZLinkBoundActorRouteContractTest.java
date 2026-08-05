package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.util.Optional;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.streams.ZLinkStreamCodec;

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
            ignored -> true,
            null,
            true,
            ZLinkStreamCodec.JSON,
            new ZLinkSessionRelayHeaders(),
            null,
            () -> currentBinding,
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
}
