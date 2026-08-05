package systems.zlink.framework.runtime.messaging;

import java.util.Objects;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.runtime.channels.ZLinkChannelContentTypeFrame;

public final class ZLinkPayloadEncoding {
    private ZLinkPayloadEncoding() {
    }

    public static EncodedPayload encode(
        ZLinkMessageSerializer serializer,
        Object payload) {
        return encode(serializer, payload, ZLinkChannelContentTypeFrame.DEFAULT_CONTENT_TYPE);
    }

    public static EncodedPayload encode(
        ZLinkMessageSerializer serializer,
        Object payload,
        String contentType) {
        Objects.requireNonNull(serializer, "serializer");
        ZLinkEncodedPayload encoded = serializer.serialize(payload);
        return new EncodedPayload(
            Message.from(encoded.bytes()),
            ZLinkPacketNames.resolve(payload),
            Objects.requireNonNull(contentType, "contentType"));
    }

    public static String resolvePacketName(Object payload) {
        return ZLinkPacketNames.resolve(payload);
    }

    public record EncodedPayload(
        Message payload,
        String packetName,
        String contentType) {
        public EncodedPayload {
            Objects.requireNonNull(payload, "payload");
            Objects.requireNonNull(packetName, "packetName");
            Objects.requireNonNull(contentType, "contentType");
        }
    }
}
