package systems.zlink.framework.runtime.internal.json;
import java.lang.annotation.Annotation;

import com.fasterxml.jackson.core.JsonGenerator;
import com.fasterxml.jackson.core.JsonParser;
import com.fasterxml.jackson.core.JsonToken;
import com.fasterxml.jackson.core.StreamReadFeature;
import com.fasterxml.jackson.databind.DeserializationContext;
import com.fasterxml.jackson.databind.DeserializationFeature;
import com.fasterxml.jackson.databind.JsonDeserializer;
import com.fasterxml.jackson.databind.JsonMappingException;
import com.fasterxml.jackson.databind.JsonSerializer;
import com.fasterxml.jackson.databind.MapperFeature;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.PropertyNamingStrategy;
import com.fasterxml.jackson.databind.SerializerProvider;
import com.fasterxml.jackson.databind.Module;
import com.fasterxml.jackson.databind.cfg.MapperConfig;
import com.fasterxml.jackson.databind.introspect.AnnotatedMethod;
import com.fasterxml.jackson.databind.json.JsonMapper;
import com.fasterxml.jackson.databind.module.SimpleModule;
import com.fasterxml.jackson.datatype.jsr310.JavaTimeModule;
import java.io.IOException;
import java.util.Base64;

/** Owns the framework-json-v1 mapper decisions shared by JVM transports. */
public final class ZLinkFrameworkJsonProfile {
    private ZLinkFrameworkJsonProfile() {
    }

    public static ObjectMapper mapper(Module... transportModules) {
        JsonMapper.Builder builder = JsonMapper.builder()
            .enable(StreamReadFeature.STRICT_DUPLICATE_DETECTION)
            .configure(MapperFeature.ACCEPT_CASE_INSENSITIVE_PROPERTIES, false)
            .configure(MapperFeature.USE_STD_BEAN_NAMING, true)
            .configure(DeserializationFeature.FAIL_ON_MISSING_CREATOR_PROPERTIES, true)
            .configure(DeserializationFeature.FAIL_ON_READING_DUP_TREE_KEY, true)
            .configure(DeserializationFeature.ACCEPT_FLOAT_AS_INT, false)
            .configure(DeserializationFeature.FAIL_ON_UNKNOWN_PROPERTIES, false)
            .propertyNamingStrategy(new KotlinAwareLowerCamelCaseStrategy())
            .findAndAddModules()
            .addModule(new JavaTimeModule())
            .addModule(profileModule());
        for (Module module : transportModules) {
            builder.addModule(module);
        }
        return builder.build();
    }

    private static SimpleModule profileModule() {
        SimpleModule module = new SimpleModule("framework-json-v1");
        JsonSerializer<Long> longSerializer = new JsonSerializer<>() {
            @Override
            public void serialize(
                Long value,
                JsonGenerator generator,
                SerializerProvider serializers) throws IOException {
                generator.writeString(Long.toString(value));
            }
        };
        JsonDeserializer<Long> longDeserializer = new JsonDeserializer<>() {
            @Override
            public Long deserialize(
                JsonParser parser,
                DeserializationContext context) throws IOException {
                if (parser.currentToken() != JsonToken.VALUE_STRING) {
                    throw JsonMappingException.from(
                        parser, "64-bit integers must be decimal JSON strings");
                }
                String value = parser.getText();
                if (!value.matches("-?(0|[1-9][0-9]*)")) {
                    throw JsonMappingException.from(
                        parser, "64-bit integer is not a canonical decimal string");
                }
                try {
                    return Long.parseLong(value);
                } catch (NumberFormatException failure) {
                    throw context.weirdStringException(
                        value, Long.class, "64-bit integer is outside its supported range");
                }
            }
        };
        module.addSerializer(Long.class, longSerializer);
        module.addSerializer(Long.TYPE, longSerializer);
        module.addDeserializer(Long.class, longDeserializer);
        module.addDeserializer(Long.TYPE, longDeserializer);
        module.addDeserializer(byte[].class, new JsonDeserializer<>() {
            @Override
            public byte[] deserialize(
                JsonParser parser,
                DeserializationContext context) throws IOException {
                if (parser.currentToken() != JsonToken.VALUE_STRING) {
                    throw JsonMappingException.from(
                        parser, "bytes must be a padded base64 JSON string");
                }
                String value = parser.getText();
                if ((value.length() & 3) != 0) {
                    throw context.weirdStringException(
                        value, byte[].class, "base64 padding is required");
                }
                try {
                    byte[] decoded = Base64.getDecoder().decode(value);
                    if (!Base64.getEncoder().encodeToString(decoded).equals(value)) {
                        throw new IllegalArgumentException("base64 is not canonical");
                    }
                    return decoded;
                } catch (IllegalArgumentException failure) {
                    throw context.weirdStringException(
                        value, byte[].class, "bytes are not canonical padded base64");
                }
            }
        });
        return module;
    }

    private static final class KotlinAwareLowerCamelCaseStrategy
        extends PropertyNamingStrategy {
        @Override
        public String nameForGetterMethod(
            MapperConfig<?> config,
            AnnotatedMethod method,
            String defaultName) {
            return kotlinPropertyName(method, defaultName);
        }

        @Override
        public String nameForSetterMethod(
            MapperConfig<?> config,
            AnnotatedMethod method,
            String defaultName) {
            return kotlinPropertyName(method, defaultName);
        }

        private static String kotlinPropertyName(
            AnnotatedMethod method,
            String defaultName) {
            if (defaultName == null || defaultName.isEmpty()
                || !isKotlinClass(method.getDeclaringClass())) {
                return defaultName;
            }
            String lowerCamel = Character.toLowerCase(defaultName.charAt(0))
                + defaultName.substring(1);
            return hasDeclaredField(method.getDeclaringClass(), lowerCamel)
                ? lowerCamel
                : defaultName;
        }

        private static boolean hasDeclaredField(Class<?> type, String name) {
            Class<?> current = type;
            while (current != null) {
                try {
                    current.getDeclaredField(name);
                    return true;
                } catch (NoSuchFieldException ignored) {
                    current = current.getSuperclass();
                }
            }
            return false;
        }

        private static boolean isKotlinClass(Class<?> type) {
            for (Annotation annotation
                : type.getDeclaredAnnotations()) {
                if (annotation.annotationType().getName().equals("kotlin.Metadata")) {
                    return true;
                }
            }
            return false;
        }
    }
}
