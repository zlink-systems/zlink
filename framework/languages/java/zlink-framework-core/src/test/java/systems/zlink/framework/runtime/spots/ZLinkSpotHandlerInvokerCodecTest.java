package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;

final class ZLinkSpotHandlerInvokerCodecTest {
    @Test
    void knownWireContentTypeSelectsItsSerializerInsteadOfTheHandlerTypeSelector() {
        ZLinkCodecRegistration codecs = new ZLinkCodecRegistration();
        codecs.addSerializer(
            "application/x-declared",
            new MarkerSerializer("DECLARED"),
            Probe.class::equals);
        codecs.addSerializer(
            "application/x-wire",
            new MarkerSerializer("WIRE"),
            ignored -> false);
        codecs.freeze();
        CapturingHandler handler = new CapturingHandler();
        ZLinkSpotHandlerInvoker invoker = new ZLinkSpotHandlerInvoker(
            codecs.serializerWithFallback(new FailingJsonSerializer()), List.of());
        SpotPacketHandlerRegistration registration = new SpotPacketHandlerRegistration(
            CapturingHandler.class,
            null,
            Object.class,
            Probe.class,
            Void.class,
            "Probe",
            false);

        try (Message payload = Message.from(new byte[] {1})) {
            invoker.invokePacket(
                    registration,
                    new Object(),
                    payload,
                    "application/x-wire",
                    Map.of(),
                    ignored -> handler)
                .toCompletableFuture()
                .join();
        }

        assertEquals("WIRE", handler.received.marker());
    }

    @Test
    void unknownWireContentTypeIsProtocolErrorBeforeHandlerDispatch() {
        ZLinkCodecRegistration codecs = new ZLinkCodecRegistration();
        codecs.freeze();
        CapturingHandler handler = new CapturingHandler();
        ZLinkSpotHandlerInvoker invoker = new ZLinkSpotHandlerInvoker(
            codecs.serializerWithFallback(new FailingJsonSerializer()), List.of());
        SpotPacketHandlerRegistration registration = new SpotPacketHandlerRegistration(
            CapturingHandler.class,
            null,
            Object.class,
            Probe.class,
            Void.class,
            "Probe",
            false);

        try (Message payload = Message.from(new byte[] {1})) {
            ZLinkFrameworkException failure = assertThrows(
                ZLinkFrameworkException.class,
                () -> invoker.invokePacket(
                    registration,
                    new Object(),
                    payload,
                    "application/x-unknown",
                    Map.of(),
                    ignored -> handler));
            assertEquals(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, failure.kind());
        }
        assertEquals(null, handler.received);
    }

    private record Probe(String marker) {
    }

    private static final class CapturingHandler
        implements ZLinkSpotPacketHandler<Object, Probe> {
        private Probe received;

        @Override
        public CompletionStage<Void> handle(Object spot, Probe message) {
            received = message;
            return CompletableFuture.completedFuture(null);
        }
    }

    private record MarkerSerializer(String marker) implements ZLinkMessageSerializer {
        @Override
        public <T> ZLinkEncodedPayload serialize(T value) {
            return ZLinkEncodedPayload.from(marker.getBytes(StandardCharsets.UTF_8));
        }

        @Override
        public <T> T deserialize(ZLinkEncodedPayload payload, Class<T> type) {
            return type.cast(new Probe(marker));
        }
    }

    private static final class FailingJsonSerializer implements ZLinkMessageSerializer {
        @Override
        public <T> ZLinkEncodedPayload serialize(T value) {
            throw new IllegalStateException("JSON serializer must not be selected");
        }

        @Override
        public <T> T deserialize(ZLinkEncodedPayload payload, Class<T> type) {
            throw new IllegalStateException("JSON serializer must not be selected");
        }
    }
}
