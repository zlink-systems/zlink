package systems.zlink.framework.perf;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.DeserializationFeature;
import com.fasterxml.jackson.databind.MapperFeature;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.SerializationFeature;
import com.fasterxml.jackson.databind.cfg.CoercionAction;
import com.fasterxml.jackson.databind.cfg.CoercionInputShape;
import com.fasterxml.jackson.databind.json.JsonMapper;
import com.fasterxml.jackson.databind.type.LogicalType;

// This serializer is only for application admin/config/result files. Framework messages use the packages' default
// typed JSON serializer without registering any codec.
public final class PerfJson {
    private PerfJson() {}

    private static final ObjectMapper MAPPER = create();

    private static ObjectMapper create() {
        return JsonMapper.builder()
                .configure(MapperFeature.USE_STD_BEAN_NAMING, true)
                .configure(DeserializationFeature.FAIL_ON_UNKNOWN_PROPERTIES, true)
                .configure(DeserializationFeature.FAIL_ON_NULL_FOR_PRIMITIVES, true)
                .configure(SerializationFeature.FAIL_ON_EMPTY_BEANS, false)
                .withCoercionConfig(LogicalType.Textual, coercions -> coercions
                        .setCoercion(CoercionInputShape.Integer, CoercionAction.Fail)
                        .setCoercion(CoercionInputShape.Float, CoercionAction.Fail)
                        .setCoercion(CoercionInputShape.Boolean, CoercionAction.Fail))
                .build();
    }

    /** The shared mapper; modules for a role's public-status observation are added by its own mapper. */
    public static ObjectMapper mapper() {
        return MAPPER;
    }

    public static <T> T read(String text, Class<T> type) {
        try {
            return MAPPER.readValue(text, type);
        } catch (JsonProcessingException error) {
            throw new PerfJsonException(error.getOriginalMessage(), error);
        }
    }

    public static String write(Object value) {
        try {
            return MAPPER.writeValueAsString(value);
        } catch (JsonProcessingException error) {
            throw new PerfJsonException(error.getOriginalMessage(), error);
        }
    }

    /** A malformed document or a field of the wrong type: the admin surface answers it with HTTP 400. */
    public static final class PerfJsonException extends RuntimeException {
        private static final long serialVersionUID = 1L;

        PerfJsonException(String message, Throwable cause) {
            super(message, cause);
        }
    }
}
