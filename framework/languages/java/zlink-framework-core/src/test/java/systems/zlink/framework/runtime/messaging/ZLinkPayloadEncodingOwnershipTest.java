package systems.zlink.framework.runtime.messaging;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.internal.messaging.ZLinkOwnedMessageSerializer;

final class ZLinkPayloadEncodingOwnershipTest {
    private static final String CONTENT_TYPE = "application/x-owned-test";

    @Test
    void registeredCodecTransfersItsOutputWithoutEncodedPayloadCopies() {
        var codec = new OwnedCodec();
        var registration = new ZLinkCodecRegistration();
        registration.addSerializer(CONTENT_TYPE, codec, type -> type == String.class);
        var serializer = registration.serializerWithFallback(new ZLinkJsonMessageSerializer());
        var encoded = ZLinkPayloadEncoding.encode(serializer, "payload", CONTENT_TYPE);
        try (var message = encoded.payload()) {
            assertSame(codec.output, message);
            assertEquals("payload", message.toUtf8String());
            assertEquals(1, codec.serializations);
        }
    }

    @Test
    void replyTypeSelectionBorrowsTheBindingPart() {
        var codec = new OwnedCodec();
        var registration = new ZLinkCodecRegistration();
        registration.addSerializer(CONTENT_TYPE, codec, type -> type == String.class);
        var serializer = registration.serializerWithFallback(new ZLinkJsonMessageSerializer());
        try (Message message = Message.from("reply")) {
            assertEquals(
                    "reply", ZLinkMessagePayloads.deserialize(serializer, message, String.class));
            assertEquals("reply", message.toUtf8String());
        }
    }

    @Test
    void failedEnvelopeConstructionReleasesCodecOutput() {
        var codec = new OwnedCodec();
        assertThrows(
                NullPointerException.class,
                () -> ZLinkPayloadEncoding.encode(codec, "payload", null));
        assertThrows(IllegalStateException.class, codec.output::refCount);
    }

    private static final class OwnedCodec
            implements ZLinkMessageSerializer, ZLinkOwnedMessageSerializer {
        private Message output;
        private int serializations;

        @Override
        public Message serializeOwned(Object value) {
            serializations++;
            output = Message.from(value.toString());
            return output;
        }

        @Override
        public <T> ZLinkEncodedPayload serialize(T value) {
            throw new AssertionError("runtime must transfer codec output directly");
        }

        @Override
        public <T> T deserialize(ZLinkEncodedPayload payload, Class<T> type) {
            throw new AssertionError("send encoding must not deserialize");
        }

        @Override
        public <T> T deserializeBorrowed(Message payload, Class<T> type) {
            return type.cast(payload.toUtf8String());
        }
    }
}
