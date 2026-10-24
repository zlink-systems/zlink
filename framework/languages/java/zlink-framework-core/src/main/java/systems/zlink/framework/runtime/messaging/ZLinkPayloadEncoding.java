package systems.zlink.framework.runtime.messaging;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.channels.ZLinkChannelContentTypeFrame;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.internal.messaging.ZLinkOwnedMessageSerializer;
import systems.zlink.framework.streams.ZLinkStreamCodec;

import java.util.Objects;

public final class ZLinkPayloadEncoding {
    private ZLinkPayloadEncoding() {}

    public static EncodedPayload encode(ZLinkMessageSerializer serializer, Object payload) {
        Objects.requireNonNull(serializer, "serializer");
        String contentType =
                ZLinkCodecRegistration.contentTypeForDeclaredType(
                        serializer,
                        declaredType(payload),
                        ZLinkChannelContentTypeFrame.DEFAULT_CONTENT_TYPE);
        return encode(serializer, payload, contentType);
    }

    public static EncodedPayload encode(
            ZLinkMessageSerializer serializer, Object payload, String contentType) {
        Message message = encodeMessage(serializer, payload, contentType);
        try {
            return encodedPayload(message, payload, contentType);
        } catch (RuntimeException | Error failure) {
            message.close();
            throw failure;
        }
    }

    public static Message encodeMessage(
            ZLinkMessageSerializer serializer, Object payload, String contentType) {
        Objects.requireNonNull(serializer, "serializer");
        Message message;
        if (payload instanceof ZLinkMessage declared) {
            message = ZLinkMessagePayloads.message(declared.toEncodedPayload(serializer));
        } else {
            var selected =
                    ZLinkCodecRegistration.serializerForSendingContentType(serializer, contentType);
            message =
                    selected instanceof ZLinkOwnedMessageSerializer owned
                            ? owned.serializeOwned(payload)
                            : ZLinkMessagePayloads.message(selected.serialize(payload));
        }
        return message;
    }

    private static EncodedPayload encodedPayload(
            Message message, Object payload, String contentType) {
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
            ZLinkMessageSerializer serializer, Object payload, ZLinkStreamCodec fallback) {
        return ZLinkCodecRegistration.streamCodecForDeclaredType(
                serializer, declaredType(payload), fallback);
    }

    public record EncodedPayload(Message payload, String packetName, String contentType) {
        public EncodedPayload {
            Objects.requireNonNull(payload, "payload");
            Objects.requireNonNull(packetName, "packetName");
            Objects.requireNonNull(contentType, "contentType");
        }
    }
}
