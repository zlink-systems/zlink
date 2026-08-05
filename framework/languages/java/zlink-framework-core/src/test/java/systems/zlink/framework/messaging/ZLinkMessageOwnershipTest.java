package systems.zlink.framework.messaging;

import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.nio.charset.StandardCharsets;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;

final class ZLinkMessageOwnershipTest {
    @Test
    void encodedMessageReusesItsImmutablePayloadAcrossDecodeAndEncode() {
        ZLinkEncodedPayload payload = ZLinkEncodedPayload.from(
            "payload".getBytes(StandardCharsets.UTF_8));
        RecordingSerializer serializer = new RecordingSerializer();
        ZLinkMessage message = ZLinkMessage.fromEncoded(payload, serializer);

        message.decode(String.class);

        assertSame(payload, serializer.decoded);
        assertSame(payload, message.toEncodedPayload(serializer));
    }

    @Test
    void explicitlyEmptyEncodedMessageRemainsEmpty() {
        ZLinkMessage message = ZLinkMessage.fromEncoded(
            ZLinkEncodedPayload.from(new byte[0]),
            new RecordingSerializer());

        assertTrue(message.isEmpty());
    }

    private static final class RecordingSerializer implements ZLinkMessageSerializer {
        private ZLinkEncodedPayload decoded;

        @Override
        public <T> ZLinkEncodedPayload serialize(T value) {
            return ZLinkEncodedPayload.from(
                String.valueOf(value).getBytes(StandardCharsets.UTF_8));
        }

        @Override
        public <T> T deserialize(ZLinkEncodedPayload payload, Class<T> type) {
            decoded = payload;
            return type.cast(new String(payload.bytes(), StandardCharsets.UTF_8));
        }
    }
}
