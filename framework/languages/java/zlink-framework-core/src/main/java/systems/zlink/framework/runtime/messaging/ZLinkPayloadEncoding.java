package systems.zlink.framework.runtime.messaging;

import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.channels.ZLinkChannelContentTypeFrame;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.internal.messaging.ZLinkOwnedMessageSerializer;
import systems.zlink.framework.runtime.internal.service.ZLinkFrameworkMultipartView;
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
        var encoded = encodeMessage(serializer, payload, contentType);
        try {
            return new EncodedPayload(
                    encoded,
                    ZLinkPacketNames.resolve(payload),
                    Objects.requireNonNull(contentType, "contentType"));
        } catch (RuntimeException | Error failure) {
            encoded.close();
            throw failure;
        }
    }

    public static ZLinkFrameworkMultipartView encodeMessage(
            ZLinkMessageSerializer serializer, Object payload, String contentType) {
        Objects.requireNonNull(serializer, "serializer");
        if (payload instanceof ZLinkMessage declared) {
            return ZLinkFrameworkMultipartView.encoded(declared.toEncodedPayload(serializer));
        }
        var selected =
                ZLinkCodecRegistration.serializerForSendingContentType(serializer, contentType);
        return encodeSelected(selected, payload, null);
    }

    private static ZLinkFrameworkMultipartView encodeSelected(
            ZLinkMessageSerializer selected, Object payload, Class<?> declaredType) {
        if (selected instanceof ZLinkJsonMessageSerializer json) {
            return json.serializeRetained(payload);
        }
        if (selected instanceof ZLinkOwnedMessageSerializer owned) {
            return ZLinkFrameworkMultipartView.owned(owned.serializeOwned(payload));
        }
        return ZLinkFrameworkMultipartView.encoded(
                declaredType == null
                        ? selected.serialize(payload)
                        : selected.serialize(payload, declaredType));
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

    public record EncodedPayload(
            ZLinkFrameworkMultipartView payload, String packetName, String contentType) {
        public EncodedPayload {
            Objects.requireNonNull(payload, "payload");
            Objects.requireNonNull(packetName, "packetName");
            Objects.requireNonNull(contentType, "contentType");
        }
    }
}
