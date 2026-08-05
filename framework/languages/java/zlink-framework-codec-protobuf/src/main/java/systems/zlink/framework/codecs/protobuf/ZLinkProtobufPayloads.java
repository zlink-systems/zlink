package systems.zlink.framework.codecs.protobuf;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.MapperFeature;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.json.JsonMapper;
import com.google.protobuf.MessageLite;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import systems.zlink.contracts.messaging.Message;

final class ZLinkProtobufPayloads {
    private static final ObjectMapper MAPPER = JsonMapper.builder()
        .configure(MapperFeature.ACCEPT_CASE_INSENSITIVE_PROPERTIES, true)
        .configure(MapperFeature.USE_STD_BEAN_NAMING, true)
        .findAndAddModules()
        .build();

    private ZLinkProtobufPayloads() {
    }

    static byte[] encodeStream(Object value) {
        if (value instanceof byte[] bytes) {
            return bytes;
        }
        if (value instanceof Message message) {
            return message.toByteArray();
        }
        if (value instanceof MessageLite protobuf) {
            return protobuf.toByteArray();
        }
        if (value instanceof String text) {
            return text.getBytes(StandardCharsets.UTF_8);
        }
        try {
            return MAPPER.writeValueAsBytes(value);
        } catch (JsonProcessingException ex) {
            throw new IllegalArgumentException(
                "failed to encode Protobuf stream payload: " + valueTypeName(value),
                ex);
        }
    }

    static <T> T decodeStreamFallback(byte[] bytes, Class<T> type) {
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
                "failed to decode Protobuf stream payload as " + type.getName(),
                ex);
        }
    }

    static String valueTypeName(Object value) {
        return value == null ? "null" : value.getClass().getName();
    }
}
