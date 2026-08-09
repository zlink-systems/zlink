package systems.zlink.framework.runtime.spots;
import java.util.concurrent.CompletionStage;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerCatalog;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerKind;

final class ZLinkSpotHandlerLoaderTest {
    @Test
    void configuredActorHandlerIsIndexedAndPreparesSerializerTypes() {
        TrackingSerializer serializer = new TrackingSerializer();
        ZLinkScannedHandlerCatalog scannedHandlers = new ZLinkScannedHandlerCatalog(List.of());
        ZLinkSpotActorHandlerCatalog actorHandlers =
            new ZLinkSpotActorHandlerCatalog(scannedHandlers, serializer);
        ZLinkSpotHandlerLoader loader = new ZLinkSpotHandlerLoader(
            scannedHandlers,
            actorHandlers);

        loader.load(
            TestSpot.class,
            List.of(ConfiguredActorHandler.class),
            (name, period, handlerType, options) -> CompletableFuture.completedFuture(null));

        List<SpotActorPacketHandlerRegistration> registrations =
            actorHandlers.handlers("configured-request");
        assertEquals(1, registrations.size());
        assertEquals(ZLinkScannedHandlerKind.ACTOR_REQUEST, registrations.get(0).kind());
        assertEquals(TestSpot.class, registrations.get(0).spotType());
        assertTrue(serializer.preparedTypes.contains(Request.class));
        assertTrue(serializer.preparedTypes.contains(Reply.class));
    }

    private static final class ConfiguredActorHandler {
        @ZLinkSpotActorRequest(packetName = "configured-request")
        public CompletionStage<Reply> handle(
            TestActor actor,
            Request request) {
            return CompletableFuture.completedFuture(new Reply());
        }
    }

    private static final class TestSpot {
    }

    private static final class TestActor {
    }

    private static final class Request {
    }

    private static final class Reply {
    }

    private static final class TrackingSerializer implements ZLinkMessageSerializer {
        private final Set<Class<?>> preparedTypes = new HashSet<>();

        @Override
        public <T> ZLinkEncodedPayload serialize(T value) {
            throw new UnsupportedOperationException();
        }

        @Override
        public <T> T deserialize(ZLinkEncodedPayload payload, Class<T> type) {
            throw new UnsupportedOperationException();
        }

        @Override
        public void prepare(Class<?> type) {
            preparedTypes.add(type);
        }
    }
}
