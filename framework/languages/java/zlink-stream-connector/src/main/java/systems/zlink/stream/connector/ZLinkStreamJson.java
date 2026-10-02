package systems.zlink.stream.connector;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.ObjectMapper;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.json.ZLinkFrameworkJsonProfile;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.Map;

public final class ZLinkStreamJson {
    public static final String CONTENT_TYPE = ZLinkFrameworkJsonProfile.CONTENT_TYPE;
    private static final ObjectMapper MAPPER = ZLinkFrameworkJsonProfile.mapper();

    private ZLinkStreamJson() {}

    public static ZLinkStreamTypedCodec codec() {
        return JsonCodec.INSTANCE;
    }

    public static ZLinkStreamSendCall send(ZLinkStreamConnector connector, Object payload) {
        return connector.send(encode(packetName(connector, payload), payload));
    }

    public static ZLinkStreamSendCall send(
            ZLinkStreamConnector connector, String name, Object payload) {
        return connector.send(
                encode(DefaultZLinkStreamConnector.validatePacketName(name), payload));
    }

    public static ZLinkStreamRequestCall request(ZLinkStreamConnector connector, Object payload) {
        return connector.request(encode(packetName(connector, payload), payload));
    }

    public static ZLinkStreamRequestCall request(
            ZLinkStreamConnector connector, String name, Object payload) {
        return connector.request(
                encode(DefaultZLinkStreamConnector.validatePacketName(name), payload));
    }

    public static <TPayload> AutoCloseable on(
            ZLinkStreamConnector connector,
            Class<TPayload> payloadType,
            ZLinkStreamMessageHandler<TPayload> handler) {
        return on(
                connector,
                connector.options().nameResolver().resolve(payloadType),
                payloadType,
                handler);
    }

    public static <TPayload> AutoCloseable on(
            ZLinkStreamConnector connector,
            String name,
            Class<TPayload> payloadType,
            ZLinkStreamMessageHandler<TPayload> handler) {
        return connector.on(
                name,
                message ->
                        handler.handleAsync(
                                new ZLinkStreamMessage<>(
                                        message.packetName(),
                                        decode(message.payload(), payloadType),
                                        message.metadata(),
                                        message.actorId())));
    }

    public static ZLinkStreamEncodedPayload encode(String packetName, Object value) {
        return new ZLinkStreamEncodedPayload(
                packetName, Message.from(encodeBytes(value)), Map.of(), ZLinkStreamCodec.JSON);
    }

    public static ZLinkStreamEncodedPayload encode(Object value) {
        if (value == null) {
            throw new IllegalArgumentException("payload is required");
        }
        return encode(
                ZLinkStreamPacketNameResolver.defaultResolver().resolve(value.getClass()), value);
    }

    public static <T> T decode(ZLinkStreamEncodedPayload payload, Class<T> type) {
        if (payload.codec() != ZLinkStreamCodec.JSON) {
            throw new IllegalArgumentException(
                    "stream payload codec is " + payload.codec() + ", not JSON");
        }
        if (type == byte[].class) {
            return type.cast(payload.payload().toByteArray());
        }
        if (type == Message.class) {
            return type.cast(Message.from(payload.payload()));
        }
        try {
            return MAPPER.readValue(payload.payload().toByteArray(), type);
        } catch (IOException ex) {
            throw new IllegalArgumentException(
                    "failed to deserialize JSON stream payload packet="
                            + payload.packetName()
                            + " as "
                            + type.getName()
                            + " payload="
                            + new String(payload.payload().toByteArray(), StandardCharsets.UTF_8),
                    ex);
        }
    }

    private static byte[] encodeBytes(Object value) {
        if (value instanceof byte[] bytes) {
            return bytes;
        }
        if (value instanceof Message message) {
            return message.toByteArray();
        }
        try {
            return MAPPER.writeValueAsBytes(value);
        } catch (JsonProcessingException ex) {
            throw new IllegalArgumentException(
                    "failed to serialize JSON stream payload: " + valueTypeName(value), ex);
        }
    }

    private static String packetName(ZLinkStreamConnector connector, Object payload) {
        return connector.options().nameResolver().resolve(payload.getClass());
    }

    private static String valueTypeName(Object value) {
        return value == null ? "null" : value.getClass().getName();
    }

    private enum JsonCodec implements ZLinkStreamTypedCodec {
        INSTANCE;

        @Override
        public <T> ZLinkStreamEncodedPayload encode(String packetName, T value) {
            return ZLinkStreamJson.encode(packetName, value);
        }

        @Override
        public <T> T decode(ZLinkStreamEncodedPayload payload, Class<T> type) {
            return ZLinkStreamJson.decode(payload, type);
        }
    }
}
