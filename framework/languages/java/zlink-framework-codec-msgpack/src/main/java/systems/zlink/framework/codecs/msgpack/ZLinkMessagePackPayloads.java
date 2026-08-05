package systems.zlink.framework.codecs.msgpack;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.MapperFeature;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.json.JsonMapper;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import systems.zlink.contracts.messaging.Message;

final class ZLinkMessagePackPayloads {
    private static final ObjectMapper MAPPER = JsonMapper.builder()
        .configure(MapperFeature.ACCEPT_CASE_INSENSITIVE_PROPERTIES, true)
        .configure(MapperFeature.USE_STD_BEAN_NAMING, true)
        .findAndAddModules()
        .build();

    private ZLinkMessagePackPayloads() {
    }

    static byte[] encode(Object value, String surface) {
        if (value instanceof byte[] bytes) {
            return bytes;
        }
        if (value instanceof Message message) {
            return message.toByteArray();
        }
        if (value instanceof String text) {
            return text.getBytes(StandardCharsets.UTF_8);
        }
        try {
            return MAPPER.writeValueAsBytes(value);
        } catch (JsonProcessingException ex) {
            throw new IllegalArgumentException(
                "failed to encode MessagePack " + surface + ": " + valueTypeName(value),
                ex);
        }
    }

    static <T> T decode(byte[] bytes, Class<T> type, String surface) {
        if (type == String.class) {
            return type.cast(new String(bytes, StandardCharsets.UTF_8));
        }
        if (type == byte[].class) {
            return type.cast(bytes);
        }
        if (type == Message.class) {
            return type.cast(Message.from(bytes));
        }
        try {
            return MAPPER.readValue(bytes, type);
        } catch (IOException ex) {
            throw new IllegalArgumentException(
                "failed to decode MessagePack " + surface + " as " + type.getName(),
                ex);
        }
    }

    private static String valueTypeName(Object value) {
        return value == null ? "null" : value.getClass().getName();
    }
}
