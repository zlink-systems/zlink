package systems.zlink.stream.connector;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.core.JsonGenerator;
import com.fasterxml.jackson.core.JsonParser;
import com.fasterxml.jackson.databind.DeserializationContext;
import com.fasterxml.jackson.databind.JsonDeserializer;
import com.fasterxml.jackson.databind.JsonSerializer;
import com.fasterxml.jackson.databind.MapperFeature;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.SerializerProvider;
import com.fasterxml.jackson.databind.json.JsonMapper;
import com.fasterxml.jackson.databind.module.SimpleModule;
import com.fasterxml.jackson.datatype.jsr310.JavaTimeModule;
import java.io.IOException;
import java.util.Map;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;

public final class ZLinkStreamJson {
    public static final String CONTENT_TYPE = "application/json";
    private static final ObjectMapper MAPPER = JsonMapper.builder()
        .configure(MapperFeature.ACCEPT_CASE_INSENSITIVE_PROPERTIES, true)
        .configure(MapperFeature.USE_STD_BEAN_NAMING, true)
        .findAndAddModules()
        .addModule(new JavaTimeModule())
        .addModule(routingIdModule())
        .build();

    private ZLinkStreamJson() {
    }

    public static ZLinkStreamTypedCodec codec() {
        return JsonCodec.INSTANCE;
    }

    public static ZLinkStreamSendCall send(
        ZLinkStreamConnector connector,
        Object payload) {
        return connector.send(encode(packetName(connector, payload), payload));
    }

    public static ZLinkStreamRequestCall request(
        ZLinkStreamConnector connector,
        Object payload) {
        return connector.request(encode(packetName(connector, payload), payload));
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
        return connector.on(name, message -> handler.handleAsync(new ZLinkStreamMessage<>(
            message.packetName(),
            decode(message.payload(), payloadType),
            message.metadata(),
            message.flowId(),
            message.flowOrigin())));
    }

    public static ZLinkStreamEncodedPayload encode(String packetName, Object value) {
        return new ZLinkStreamEncodedPayload(
            packetName,
            Message.from(encodeBytes(value)),
            Map.of(),
            ZLinkStreamCodec.JSON);
    }

    public static ZLinkStreamEncodedPayload encode(Object value) {
        if (value == null) {
            throw new IllegalArgumentException("payload is required");
        }
        return encode(ZLinkStreamPacketNameResolver.defaultResolver().resolve(value.getClass()), value);
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
                    + new String(payload.payload().toByteArray(), java.nio.charset.StandardCharsets.UTF_8),
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
                "failed to serialize JSON stream payload: " + valueTypeName(value),
                ex);
        }
    }

    private static String packetName(ZLinkStreamConnector connector, Object payload) {
        return connector.options().nameResolver().resolve(payload.getClass());
    }

    private static String valueTypeName(Object value) {
        return value == null ? "null" : value.getClass().getName();
    }

    private static SimpleModule routingIdModule() {
        SimpleModule module = new SimpleModule("zlink-routing-id");
        module.addSerializer(RoutingId.class, new JsonSerializer<>() {
            @Override
            public void serialize(
                RoutingId value,
                JsonGenerator generator,
                SerializerProvider serializers) throws IOException {
                generator.writeString(value.toHex());
            }
        });
        module.addDeserializer(RoutingId.class, new JsonDeserializer<>() {
            @Override
            public RoutingId deserialize(
                JsonParser parser,
                DeserializationContext context) throws IOException {
                return RoutingId.fromHex(parser.getValueAsString());
            }
        });
        return module;
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
