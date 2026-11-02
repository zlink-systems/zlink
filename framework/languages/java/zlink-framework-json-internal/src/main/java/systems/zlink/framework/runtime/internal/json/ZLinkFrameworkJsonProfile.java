/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.framework.runtime.internal.json;

import com.fasterxml.jackson.core.JsonFactory;
import com.fasterxml.jackson.core.JsonGenerator;
import com.fasterxml.jackson.core.JsonParser;
import com.fasterxml.jackson.core.JsonToken;
import com.fasterxml.jackson.core.StreamReadFeature;
import com.fasterxml.jackson.core.io.IOContext;
import com.fasterxml.jackson.core.io.InputDecorator;
import com.fasterxml.jackson.databind.AnnotationIntrospector;
import com.fasterxml.jackson.databind.DeserializationContext;
import com.fasterxml.jackson.databind.DeserializationFeature;
import com.fasterxml.jackson.databind.JsonDeserializer;
import com.fasterxml.jackson.databind.JsonMappingException;
import com.fasterxml.jackson.databind.JsonSerializer;
import com.fasterxml.jackson.databind.MapperFeature;
import com.fasterxml.jackson.databind.Module;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.PropertyNamingStrategy;
import com.fasterxml.jackson.databind.SerializerProvider;
import com.fasterxml.jackson.databind.cfg.MapperConfig;
import com.fasterxml.jackson.databind.deser.NullValueProvider;
import com.fasterxml.jackson.databind.deser.SettableBeanProperty;
import com.fasterxml.jackson.databind.deser.ValueInstantiator;
import com.fasterxml.jackson.databind.deser.ValueInstantiators;
import com.fasterxml.jackson.databind.introspect.AnnotatedMember;
import com.fasterxml.jackson.databind.introspect.AnnotatedMethod;
import com.fasterxml.jackson.databind.introspect.AnnotatedParameter;
import com.fasterxml.jackson.databind.introspect.JacksonAnnotationIntrospector;
import com.fasterxml.jackson.databind.json.JsonMapper;
import com.fasterxml.jackson.databind.module.SimpleModule;
import com.fasterxml.jackson.databind.util.AccessPattern;
import com.fasterxml.jackson.datatype.jsr310.JavaTimeModule;

import systems.zlink.contracts.core.RoutingId;

import java.io.IOException;
import java.io.InputStream;
import java.io.PushbackInputStream;
import java.io.Reader;
import java.lang.annotation.Annotation;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.lang.reflect.RecordComponent;
import java.util.Base64;
import java.util.List;

/** Owns the framework-json-v1 mapper decisions shared by JVM transports. */
public final class ZLinkFrameworkJsonProfile {
    public static final String CONTENT_TYPE = "application/json";
    private static final String KOTLIN_METADATA_CLASS_NAME = "kotlin.Metadata";
    private static final String ZLINK_JSON_OPTIONAL_ANNOTATION_CLASS_NAME =
            "systems.zlink.framework.ZLinkJsonOptional";
    private static final int UTF8_BOM_LENGTH = 3;
    private static final int UTF8_BOM_PREFIX = 0xefbbbf;

    private static boolean isCanonicalDecimalInteger(String value) {
        if (value.isEmpty()) {
            return false;
        }
        if (value.equals("0")) {
            return true;
        }
        int firstDigit = value.charAt(0) == '-' ? 1 : 0;
        if (firstDigit == value.length()
                || value.charAt(firstDigit) < '1'
                || value.charAt(firstDigit) > '9') {
            return false;
        }
        for (int index = firstDigit + 1; index < value.length(); index++) {
            char digit = value.charAt(index);
            if (digit < '0' || digit > '9') {
                return false;
            }
        }
        return true;
    }

    private ZLinkFrameworkJsonProfile() {}

    private static final class ConstructorRequirednessIntrospector
            extends JacksonAnnotationIntrospector {
        @Override
        public Boolean hasRequiredMarker(AnnotatedMember member) {
            if (!(member instanceof AnnotatedParameter parameter)) {
                return super.hasRequiredMarker(member);
            }
            if (parameter.getDeclaringClass().isRecord()) {
                return !isJavaRecordOptionalComponent(parameter);
            }
            Boolean optional = isKotlinOptionalParameter(parameter);
            return optional != null ? !optional : Boolean.TRUE;
        }
    }

    private static boolean isJavaRecordOptionalComponent(AnnotatedParameter parameter) {
        RecordComponent[] components = parameter.getDeclaringClass().getRecordComponents();
        int index = parameter.getIndex();
        if (index < 0 || index >= components.length) {
            return false;
        }
        return java.util.Arrays.stream(components[index].getAnnotations())
                .anyMatch(
                        annotation ->
                                annotation
                                        .annotationType()
                                        .getName()
                                        .equals(ZLINK_JSON_OPTIONAL_ANNOTATION_CLASS_NAME));
    }

    private static Boolean isKotlinOptionalParameter(AnnotatedParameter parameter) {
        Class<?> declaringClass = parameter.getDeclaringClass();
        boolean kotlinType =
                java.util.Arrays.stream(declaringClass.getAnnotations())
                        .anyMatch(
                                annotation ->
                                        annotation
                                                .annotationType()
                                                .getName()
                                                .equals(KOTLIN_METADATA_CLASS_NAME));
        if (!kotlinType) {
            return null;
        }
        try {
            Class<?> mapping = Class.forName("kotlin.reflect.jvm.ReflectJvmMapping");
            Method getKotlinFunction = mapping.getMethod("getKotlinFunction", Constructor.class);
            Object owner = parameter.getOwner().getAnnotated();
            if (!(owner instanceof Constructor<?> constructor)) {
                return null;
            }
            Object function = getKotlinFunction.invoke(null, constructor);
            if (function == null) {
                return null;
            }
            Class<?> callable = Class.forName("kotlin.reflect.KCallable");
            List<?> parameters = (List<?>) callable.getMethod("getParameters").invoke(function);
            Class<?> kotlinParameter = Class.forName("kotlin.reflect.KParameter");
            Method getKind = kotlinParameter.getMethod("getKind");
            Method isOptional = kotlinParameter.getMethod("isOptional");
            int valueIndex = 0;
            for (Object candidate : parameters) {
                if (!"VALUE".equals(getKind.invoke(candidate).toString())) {
                    continue;
                }
                if (valueIndex++ == parameter.getIndex()) {
                    return (Boolean) isOptional.invoke(candidate);
                }
            }
            return null;
        } catch (ReflectiveOperationException ex) {
            throw new IllegalStateException(
                    "failed to inspect Kotlin constructor requiredness", ex);
        }
    }

    public static ObjectMapper mapper(Module... transportModules) {
        JsonMapper.Builder builder =
                JsonMapper.builder(
                                JsonFactory.builder()
                                        .inputDecorator(new ProfileInputDecorator())
                                        .build())
                        .enable(StreamReadFeature.STRICT_DUPLICATE_DETECTION)
                        .configure(MapperFeature.ACCEPT_CASE_INSENSITIVE_PROPERTIES, false)
                        .configure(MapperFeature.USE_STD_BEAN_NAMING, true)
                        .configure(DeserializationFeature.FAIL_ON_MISSING_CREATOR_PROPERTIES, false)
                        .configure(DeserializationFeature.FAIL_ON_NULL_FOR_PRIMITIVES, true)
                        .configure(DeserializationFeature.FAIL_ON_READING_DUP_TREE_KEY, true)
                        .configure(DeserializationFeature.ACCEPT_FLOAT_AS_INT, false)
                        .configure(DeserializationFeature.FAIL_ON_NUMBERS_FOR_ENUMS, true)
                        .configure(DeserializationFeature.FAIL_ON_UNKNOWN_PROPERTIES, false)
                        .propertyNamingStrategy(new KotlinAwareLowerCamelCaseStrategy())
                        .findAndAddModules()
                        .addModule(new JavaTimeModule())
                        .addModule(profileModule())
                        .addModule(routingIdModule());
        for (Module module : transportModules) {
            builder.addModule(module);
        }
        ObjectMapper mapper = builder.build();
        AnnotationIntrospector existing =
                mapper.getDeserializationConfig().getAnnotationIntrospector();
        mapper.setConfig(
                mapper.getDeserializationConfig()
                        .with(
                                AnnotationIntrospector.pair(
                                        new ConstructorRequirednessIntrospector(), existing)));
        return mapper;
    }

    private static void rejectBomPrefix(int prefix) throws IOException {
        if (prefix == UTF8_BOM_PREFIX) {
            throw new IOException("framework-json-v1 does not allow a UTF-8 BOM");
        }
    }

    private static final class ProfileInputDecorator extends InputDecorator {
        @Override
        public InputStream decorate(IOContext context, byte[] input, int offset, int length)
                throws IOException {
            rejectBom(input, offset, length);
            return null;
        }

        @Override
        public InputStream decorate(IOContext context, InputStream input) throws IOException {
            PushbackInputStream stream = new PushbackInputStream(input, UTF8_BOM_LENGTH);
            byte[] prefix = stream.readNBytes(UTF8_BOM_LENGTH);
            rejectBom(prefix, 0, prefix.length);
            stream.unread(prefix);
            return stream;
        }

        @Override
        public Reader decorate(IOContext context, Reader reader) {
            return reader;
        }

        private static void rejectBom(byte[] input, int offset, int length) throws IOException {
            int prefix = 0;
            for (int index = 0; index < Math.min(length, UTF8_BOM_LENGTH); index++) {
                prefix = (prefix << Byte.SIZE) | (input[offset + index] & 0xff);
            }
            rejectBomPrefix(prefix);
        }
    }

    private static SimpleModule routingIdModule() {
        SimpleModule module = new SimpleModule("zlink-routing-id");
        module.addSerializer(
                RoutingId.class,
                new JsonSerializer<>() {
                    @Override
                    public void serialize(
                            RoutingId value,
                            JsonGenerator generator,
                            SerializerProvider serializers)
                            throws IOException {
                        generator.writeString(value.toHex());
                    }
                });
        module.addDeserializer(
                RoutingId.class,
                new JsonDeserializer<>() {
                    @Override
                    public RoutingId deserialize(JsonParser parser, DeserializationContext context)
                            throws IOException {
                        return RoutingId.fromHex(parser.getValueAsString());
                    }
                });
        return module;
    }

    private static SimpleModule profileModule() {
        SimpleModule module =
                new SimpleModule("framework-json-v1") {
                    @Override
                    public void setupModule(Module.SetupContext context) {
                        super.setupModule(context);
                        context.addValueInstantiators(
                                new ValueInstantiators.Base() {
                                    @Override
                                    public ValueInstantiator findValueInstantiator(
                                            com.fasterxml.jackson.databind.DeserializationConfig
                                                    config,
                                            com.fasterxml.jackson.databind.BeanDescription
                                                    beanDescription,
                                            ValueInstantiator valueInstantiator) {
                                        return beanDescription.getBeanClass().isRecord()
                                                ? new OptionalPrimitiveValueInstantiator(
                                                        valueInstantiator)
                                                : valueInstantiator;
                                    }
                                });
                    }
                };
        JsonSerializer<Number> finiteNumberSerializer =
                new JsonSerializer<>() {
                    @Override
                    public void serialize(
                            Number value, JsonGenerator generator, SerializerProvider serializers)
                            throws IOException {
                        if (!Double.isFinite(value.doubleValue())) {
                            throw JsonMappingException.from(
                                    generator, "framework-json-v1 requires finite JSON numbers");
                        }
                        generator.writeNumber(value.toString());
                    }
                };
        module.addSerializer(Double.class, finiteNumberSerializer);
        module.addSerializer(Double.TYPE, finiteNumberSerializer);
        module.addSerializer(Float.class, finiteNumberSerializer);
        module.addSerializer(Float.TYPE, finiteNumberSerializer);
        JsonDeserializer<Double> doubleDeserializer =
                new JsonDeserializer<>() {
                    @Override
                    public Double deserialize(JsonParser parser, DeserializationContext context)
                            throws IOException {
                        return readFiniteNumber(parser, false).doubleValue();
                    }
                };
        JsonDeserializer<Float> floatDeserializer =
                new JsonDeserializer<>() {
                    @Override
                    public Float deserialize(JsonParser parser, DeserializationContext context)
                            throws IOException {
                        return readFiniteNumber(parser, true).floatValue();
                    }
                };
        module.addDeserializer(Double.class, doubleDeserializer);
        module.addDeserializer(Double.TYPE, doubleDeserializer);
        module.addDeserializer(Float.class, floatDeserializer);
        module.addDeserializer(Float.TYPE, floatDeserializer);

        JsonSerializer<Long> longSerializer =
                new JsonSerializer<>() {
                    @Override
                    public void serialize(
                            Long value, JsonGenerator generator, SerializerProvider serializers)
                            throws IOException {
                        generator.writeString(Long.toString(value));
                    }
                };
        JsonDeserializer<Long> longDeserializer =
                new JsonDeserializer<>() {
                    @Override
                    public Long deserialize(JsonParser parser, DeserializationContext context)
                            throws IOException {
                        if (parser.currentToken() != JsonToken.VALUE_STRING) {
                            throw JsonMappingException.from(
                                    parser, "64-bit integers must be decimal JSON strings");
                        }
                        String value = parser.getText();
                        if (!isCanonicalDecimalInteger(value)) {
                            throw JsonMappingException.from(
                                    parser, "64-bit integer is not a canonical decimal string");
                        }
                        try {
                            return Long.parseLong(value);
                        } catch (NumberFormatException failure) {
                            throw context.weirdStringException(
                                    value,
                                    Long.class,
                                    "64-bit integer is outside its supported range");
                        }
                    }
                };
        module.addSerializer(Long.class, longSerializer);
        module.addSerializer(Long.TYPE, longSerializer);
        module.addDeserializer(Long.class, longDeserializer);
        module.addDeserializer(Long.TYPE, longDeserializer);
        module.addDeserializer(
                byte[].class,
                new JsonDeserializer<>() {
                    @Override
                    public byte[] deserialize(JsonParser parser, DeserializationContext context)
                            throws IOException {
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

    private static final class OptionalPrimitiveValueInstantiator
            extends ValueInstantiator.Delegating {
        private OptionalPrimitiveValueInstantiator(ValueInstantiator delegate) {
            super(delegate);
        }

        @Override
        public SettableBeanProperty[] getFromObjectArguments(
                com.fasterxml.jackson.databind.DeserializationConfig config) {
            SettableBeanProperty[] properties = super.getFromObjectArguments(config);
            if (properties == null) {
                return null;
            }
            for (int index = 0; index < properties.length; index++) {
                SettableBeanProperty property = properties[index];
                if (property != null
                        && property.getType().isPrimitive()
                        && !property.getMetadata().isRequired()) {
                    properties[index] =
                            property.withNullProvider(
                                    new OptionalPrimitiveValueProvider(
                                            property.getType().getRawClass(), property.getName()));
                }
            }
            return properties;
        }
    }

    private static final class OptionalPrimitiveValueProvider implements NullValueProvider {
        private final Class<?> primitiveType;
        private final String propertyName;

        private OptionalPrimitiveValueProvider(Class<?> primitiveType, String propertyName) {
            this.primitiveType = primitiveType;
            this.propertyName = propertyName;
        }

        @Override
        public Object getNullValue(DeserializationContext context) throws JsonMappingException {
            throw JsonMappingException.from(
                    context.getParser(), "Property '" + propertyName + "' does not allow null.");
        }

        @Override
        public Object getAbsentValue(DeserializationContext context) {
            if (primitiveType == boolean.class) return false;
            if (primitiveType == byte.class) return (byte) 0;
            if (primitiveType == short.class) return (short) 0;
            if (primitiveType == int.class) return 0;
            if (primitiveType == long.class) return 0L;
            if (primitiveType == float.class) return 0.0f;
            if (primitiveType == double.class) return 0.0d;
            if (primitiveType == char.class) return '\0';
            throw new IllegalStateException(
                    "Unsupported primitive record component: " + primitiveType);
        }

        @Override
        public AccessPattern getNullAccessPattern() {
            return AccessPattern.DYNAMIC;
        }
    }

    private static Number readFiniteNumber(JsonParser parser, boolean singlePrecision)
            throws IOException {
        if (!parser.currentToken().isNumeric()) {
            throw JsonMappingException.from(parser, "floating-point values must be JSON numbers");
        }
        Number value;
        if (singlePrecision) {
            value = parser.getFloatValue();
        } else {
            value = parser.getDoubleValue();
        }
        if (!Double.isFinite(value.doubleValue())) {
            throw JsonMappingException.from(
                    parser, "framework-json-v1 requires finite JSON numbers");
        }
        return value;
    }

    private static final class KotlinAwareLowerCamelCaseStrategy extends PropertyNamingStrategy {
        @Override
        public String nameForGetterMethod(
                MapperConfig<?> config, AnnotatedMethod method, String defaultName) {
            return kotlinPropertyName(method, defaultName);
        }

        @Override
        public String nameForSetterMethod(
                MapperConfig<?> config, AnnotatedMethod method, String defaultName) {
            return kotlinPropertyName(method, defaultName);
        }

        private static String kotlinPropertyName(AnnotatedMethod method, String defaultName) {
            if (defaultName == null
                    || defaultName.isEmpty()
                    || !isKotlinClass(method.getDeclaringClass())) {
                return defaultName;
            }
            String lowerCamel =
                    Character.toLowerCase(defaultName.charAt(0)) + defaultName.substring(1);
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
            for (Annotation annotation : type.getDeclaredAnnotations()) {
                if (annotation.annotationType().getName().equals(KOTLIN_METADATA_CLASS_NAME)) {
                    return true;
                }
            }
            return false;
        }
    }
}
