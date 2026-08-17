package systems.zlink.framework.runtime.streams;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.nio.charset.StandardCharsets;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;
import systems.zlink.framework.streams.ZLinkStreamCodec;

final class ZLinkStreamSessionDeclaredTypeCodecTest {
    @Test
    void sessionSendAndReplyCarryTheDeclaredTypeCodec() {
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
        ZLinkStreamSessionClient client = new ZLinkStreamSessionClient(
            null,
            RoutingId.from("session"),
            null,
            codecs.serializerWithFallback(new ZLinkJsonMessageSerializer()),
            ZLinkStreamCodec.RAW,
            null);
        ZLinkMessage message = ZLinkMessage.of(
            new DerivedMessage(), BaseMessage.class);

        ZLinkStreamSessionSendCall send =
            (ZLinkStreamSessionSendCall) client.send(message);
        ZLinkStreamSessionReplyCall reply =
            (ZLinkStreamSessionReplyCall) client.reply(message);
        try {
            assertEquals(ZLinkStreamCodec.PROTOBUF, send.codec());
            assertEquals(ZLinkStreamCodec.PROTOBUF, reply.codec());
            assertEquals("BaseMessage", send.packetName());
            assertEquals("BaseMessage", reply.packetName());
            assertEquals("BASE", send.payload().toUtf8String());
            assertEquals("BASE", reply.payload().toUtf8String());
        } finally {
            send.payload().close();
            reply.payload().close();
        }
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
