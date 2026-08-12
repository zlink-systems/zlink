package systems.zlink.framework.runtime.messaging;

import java.util.Objects;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.channels.ZLinkChannelContentTypeFrame;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.streams.ZLinkStreamCodec;

public final class ZLinkPayloadEncoding {
    private ZLinkPayloadEncoding() {
    }

    public static EncodedPayload encode(
        ZLinkMessageSerializer serializer,
        Object payload) {
        Objects.requireNonNull(serializer, "serializer");
        String contentType = ZLinkCodecRegistration.contentTypeForDeclaredType(
            serializer,
            declaredType(payload),
            ZLinkChannelContentTypeFrame.DEFAULT_CONTENT_TYPE);
        return encode(serializer, payload, contentType);
    }

    public static EncodedPayload encode(
        ZLinkMessageSerializer serializer,
        Object payload,
        String contentType) {
        Objects.requireNonNull(serializer, "serializer");
        Message message;
        if (serializer instanceof ZLinkJsonMessageSerializer json
            && !(payload instanceof ZLinkMessage)
            && ZLinkChannelContentTypeFrame.DEFAULT_CONTENT_TYPE.equals(contentType)) {
            message = json.serializeOwned(payload);
        } else {
            ZLinkEncodedPayload encoded = payload instanceof ZLinkMessage declared
                ? declared.toEncodedPayload(serializer)
                : ZLinkCodecRegistration.serializeForContentType(
                    serializer, payload, contentType);
            message = Message.from(encoded.bytes());
        }
        return encodedPayload(message, payload, contentType);
    }

    private static EncodedPayload encodedPayload(
        Message message,
        Object payload,
        String contentType) {
        return new EncodedPayload(
            message,
            ZLinkPacketNames.resolve(payload),
            Objects.requireNonNull(contentType, "contentType"));
    }

    public static String resolvePacketName(Object payload) {
        return ZLinkPacketNames.resolve(payload);
    }

    public static Class<?> declaredType(Object payload) {
        if (payload instanceof ZLinkMessage message) {
            return message.declaredType();
        }
        return payload == null ? null : payload.getClass();
    }

    public static ZLinkStreamCodec streamCodec(
        ZLinkMessageSerializer serializer,
        Object payload,
        ZLinkStreamCodec fallback) {
        return ZLinkCodecRegistration.streamCodecForDeclaredType(
            serializer, declaredType(payload), fallback);
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
