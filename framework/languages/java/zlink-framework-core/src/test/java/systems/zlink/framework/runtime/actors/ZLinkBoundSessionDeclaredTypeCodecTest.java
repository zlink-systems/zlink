package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.nio.charset.StandardCharsets;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;
import systems.zlink.framework.streams.ZLinkStreamCodec;

final class ZLinkBoundSessionDeclaredTypeCodecTest {
    @Test
    void boundSessionOptionsCarryTheDeclaredTypeCodec() {
        ZLinkCodecRegistration codecs = new ZLinkCodecRegistration();
        codecs.addSerializer(
            "application/x-broad",
            new MarkerSerializer("BROAD"),
            type -> type == BaseMessage.class || type == DerivedMessage.class);
        codecs.addStreamCodec("application/x-broad", ZLinkStreamCodec.MESSAGE_PACK);
        codecs.addSerializer(
            "application/x-base",
            new MarkerSerializer("BASE"),
            BaseMessage.class::equals);
        codecs.addStreamCodec("application/x-base", ZLinkStreamCodec.PROTOBUF);
        codecs.freeze();
        ZLinkMessageSerializer serializer =
            codecs.serializerWithFallback(new ZLinkJsonMessageSerializer());
        ZLinkMessage message = ZLinkMessage.of(
            new DerivedMessage(), BaseMessage.class);

        ZLinkBoundSessionSendOptions options =
            ZLinkBoundSessionSendOptions.createForPayload(
                serializer,
                message,
                "BaseMessage",
                ZLinkStreamCodec.RAW);

        assertEquals(ZLinkStreamCodec.PROTOBUF, options.header().codec());
        assertEquals("BaseMessage", options.header().packetName());
        assertEquals(
            "BASE",
            new String(
                message.toEncodedPayload(serializer).bytes(),
                StandardCharsets.UTF_8));
    }

    private static class BaseMessage {
    }

    private static final class DerivedMessage extends BaseMessage {
    }

    private record MarkerSerializer(String marker) implements ZLinkMessageSerializer {
        @Override
        public <T> ZLinkEncodedPayload serialize(T value) {
            return ZLinkEncodedPayload.from(marker.getBytes(StandardCharsets.UTF_8));
        }

        @Override
        public <T> T deserialize(ZLinkEncodedPayload payload, Class<T> type) {
            throw new UnsupportedOperationException();
        }
    }
}
