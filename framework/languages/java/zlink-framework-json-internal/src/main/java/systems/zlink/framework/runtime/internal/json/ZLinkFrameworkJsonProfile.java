/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.framework.runtime.internal.json;

import com.fasterxml.jackson.annotation.JsonSetter;
import com.fasterxml.jackson.annotation.Nulls;
import com.fasterxml.jackson.core.Base64Variant;
import com.fasterxml.jackson.core.Base64Variants;
import com.fasterxml.jackson.core.JsonFactory;
import com.fasterxml.jackson.core.JsonGenerator;
import com.fasterxml.jackson.core.JsonParseException;
import com.fasterxml.jackson.core.JsonParser;
import com.fasterxml.jackson.core.JsonToken;
import com.fasterxml.jackson.core.io.IOContext;
import com.fasterxml.jackson.core.io.InputDecorator;
import com.fasterxml.jackson.core.json.DupDetector;
import com.fasterxml.jackson.core.util.JsonParserDelegate;
import com.fasterxml.jackson.databind.AnnotationIntrospector;
import com.fasterxml.jackson.databind.BeanDescription;
import com.fasterxml.jackson.databind.DeserializationConfig;
import com.fasterxml.jackson.databind.DeserializationContext;
import com.fasterxml.jackson.databind.DeserializationFeature;
import com.fasterxml.jackson.databind.JsonDeserializer;
import com.fasterxml.jackson.databind.JsonMappingException;
import com.fasterxml.jackson.databind.JsonSerializer;
import com.fasterxml.jackson.databind.MapperFeature;
import com.fasterxml.jackson.databind.Module;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.PropertyNamingStrategy;
import com.fasterxml.jackson.databind.SerializationConfig;
import com.fasterxml.jackson.databind.SerializerProvider;
import com.fasterxml.jackson.databind.cfg.MapperConfig;
import com.fasterxml.jackson.databind.deser.BeanDeserializerBase;
import com.fasterxml.jackson.databind.deser.BeanDeserializerModifier;
import com.fasterxml.jackson.databind.deser.NullValueProvider;
import com.fasterxml.jackson.databind.deser.SettableBeanProperty;
import com.fasterxml.jackson.databind.deser.ValueInstantiator;
import com.fasterxml.jackson.databind.deser.ValueInstantiators;
import com.fasterxml.jackson.databind.deser.std.DelegatingDeserializer;
import com.fasterxml.jackson.databind.introspect.Annotated;
import com.fasterxml.jackson.databind.introspect.AnnotatedMember;
import com.fasterxml.jackson.databind.introspect.AnnotatedMethod;
import com.fasterxml.jackson.databind.introspect.AnnotatedParameter;
import com.fasterxml.jackson.databind.introspect.BeanPropertyDefinition;
import com.fasterxml.jackson.databind.introspect.JacksonAnnotationIntrospector;
import com.fasterxml.jackson.databind.json.JsonMapper;
import com.fasterxml.jackson.databind.module.SimpleModule;
import com.fasterxml.jackson.databind.ser.BeanPropertyWriter;
import com.fasterxml.jackson.databind.ser.BeanSerializerModifier;
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
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

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
        public JsonSetter.Value findSetterInfo(Annotated member) {
            JsonSetter.Value declared = super.findSetterInfo(member);
            // Only a Nullable annotation declares null (spec 05-message-model 5): type-use
            // (JSpecify) or declaration (JSR 305, Jakarta). Jackson null settings do not.
            java.lang.reflect.AnnotatedType type = null;
            Annotation[] declaration = null;
            if (member instanceof AnnotatedParameter parameter
                    && parameter.getOwner().getAnnotated() instanceof Constructor<?> constructor) {
                type = constructor.getAnnotatedParameterTypes()[parameter.getIndex()];
                declaration = constructor.getParameterAnnotations()[parameter.getIndex()];
            } else if (member.getAnnotated() instanceof java.lang.reflect.Field field) {
                type = field.getAnnotatedType();
                declaration = field.getAnnotations();
            } else if (member.getAnnotated() instanceof Method method) {
                boolean setter = method.getParameterCount() == 1;
                type =
                        setter
                                ? method.getAnnotatedParameterTypes()[0]
                                : method.getAnnotatedReturnType();
                declaration =
                        setter ? method.getParameterAnnotations()[0] : method.getAnnotations();
            }
            if (type != null && (isNullable(type.getAnnotations()) || isNullable(declaration)))
                return declared;
            // Kotlin's module owns its declared nullable constructor types.
            if (member instanceof AnnotatedMember annotated
                    && KotlinAwareLowerCamelCaseStrategy.isKotlinClass(
                            annotated.getDeclaringClass())) return declared;
            return (declared == null ? JsonSetter.Value.empty() : declared)
                    .withValueNulls(Nulls.FAIL);
        }

        private static boolean isNullable(Annotation[] annotations) {
            if (annotations == null) return false;
            for (Annotation annotation : annotations) {
                if (annotation.annotationType().getSimpleName().equals("Nullable")) return true;
            }
            return false;
        }

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
                JsonMapper.builder(new ProfileJsonFactory())
                        .configure(MapperFeature.ACCEPT_CASE_INSENSITIVE_PROPERTIES, false)
                        .configure(MapperFeature.USE_STD_BEAN_NAMING, true)
                        .configure(DeserializationFeature.FAIL_ON_MISSING_CREATOR_PROPERTIES, false)
                        .configure(DeserializationFeature.FAIL_ON_NULL_FOR_PRIMITIVES, true)
                        .configure(DeserializationFeature.FAIL_ON_READING_DUP_TREE_KEY, true)
                        .configure(DeserializationFeature.FAIL_ON_TRAILING_TOKENS, true)
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

    private static final class ProfileJsonFactory extends JsonFactory {
        ProfileJsonFactory() {
            super(JsonFactory.builder().inputDecorator(new ProfileInputDecorator()).build(), null);
        }

        @Override
        protected JsonParser _createParser(InputStream input, IOContext context)
                throws IOException {
            return new ProfileParser(super._createParser(input, context));
        }

        @Override
        protected JsonParser _createParser(Reader input, IOContext context) throws IOException {
            return new ProfileParser(super._createParser(input, context));
        }

        @Override
        protected JsonParser _createParser(
                char[] input, int offset, int length, IOContext context, boolean recyclable)
                throws IOException {
            return new ProfileParser(
                    super._createParser(input, offset, length, context, recyclable));
        }

        @Override
        protected JsonParser _createParser(byte[] input, int offset, int length, IOContext context)
                throws IOException {
            return new ProfileParser(super._createParser(input, offset, length, context));
        }

        @Override
        protected JsonParser _createParser(java.io.DataInput input, IOContext context)
                throws IOException {
            return new ProfileParser(super._createParser(input, context));
        }
    }

    private static final class ProfileParser extends JsonParserDelegate {
        private ObjectScope[] objectScopes = new ObjectScope[4];
        private int objectDepth;

        ProfileParser(JsonParser parser) {
            super(parser);
        }

        @Override
        public JsonToken nextToken() throws IOException {
            JsonToken token = super.nextToken();
            if (token == JsonToken.START_OBJECT) {
                if (objectDepth == objectScopes.length) {
                    objectScopes = Arrays.copyOf(objectScopes, objectScopes.length * 2);
                }
                ObjectScope scope = objectScopes[objectDepth];
                if (scope == null) {
                    scope = new ObjectScope(this);
                    objectScopes[objectDepth] = scope;
                }
                scope.reset();
                objectDepth++;
            } else if (token == JsonToken.FIELD_NAME && objectDepth != 0) {
                String name = currentName();
                if (objectScopes[objectDepth - 1].isDuplicate(name)) {
                    throw new JsonParseException(this, "Duplicate field '" + name + "'");
                }
            } else if (token == JsonToken.END_OBJECT && objectDepth != 0) {
                objectDepth--;
            }
            // Only fractions and integers wider than long can exceed the double range.
            if ((token == JsonToken.VALUE_NUMBER_FLOAT
                            || (token == JsonToken.VALUE_NUMBER_INT
                                    && getNumberType() == NumberType.BIG_INTEGER))
                    && !Double.isFinite(getDoubleValue()))
                throw JsonMappingException.from(
                        this, "framework-json-v1 requires finite JSON numbers");
            return token;
        }

        @Override
        public JsonToken nextValue() throws IOException {
            JsonToken token = nextToken();
            return token == JsonToken.FIELD_NAME ? nextToken() : token;
        }

        @Override
        public String nextFieldName() throws IOException {
            return nextToken() == JsonToken.FIELD_NAME ? currentName() : null;
        }

        @Override
        public boolean nextFieldName(com.fasterxml.jackson.core.SerializableString expected)
                throws IOException {
            return expected.getValue().equals(nextFieldName());
        }

        void usePropertyMask(Map<String, Integer> propertyIndexes) throws IOException {
            if (objectDepth == 0) return;
            ObjectScope scope = objectScopes[objectDepth - 1];
            scope.usePropertyMask(propertyIndexes);
            if (currentToken() == JsonToken.FIELD_NAME) scope.mark(currentName());
        }

        @Override
        public JsonParser skipChildren() throws IOException {
            if (currentToken() != JsonToken.START_OBJECT && currentToken() != JsonToken.START_ARRAY)
                return this;
            int depth = 1;
            while (depth != 0) {
                JsonToken token = nextToken();
                if (token == null) throw JsonMappingException.from(this, "Incomplete JSON value");
                if (token.isStructStart()) depth++;
                else if (token.isStructEnd()) depth--;
            }
            return this;
        }

        private static final class ObjectScope {
            private final DupDetector fallback;
            private Map<String, Integer> propertyIndexes;
            private long seen;
            private long[] seenWords;

            private ObjectScope(JsonParser parser) {
                fallback = DupDetector.rootDetector(parser);
            }

            private void reset() {
                propertyIndexes = null;
                seen = 0;
                if (seenWords != null) Arrays.fill(seenWords, 0L);
                fallback.reset();
            }

            private void usePropertyMask(Map<String, Integer> indexes) {
                propertyIndexes = indexes;
                seen = 0;
                int words = (indexes.size() + Long.SIZE - 1) / Long.SIZE;
                if (words > 1) {
                    if (seenWords == null || seenWords.length < words) {
                        seenWords = new long[words];
                    } else {
                        Arrays.fill(seenWords, 0, words, 0L);
                    }
                }
            }

            private boolean isDuplicate(String name) throws JsonParseException {
                Integer index = propertyIndexes == null ? null : propertyIndexes.get(name);
                if (index == null) return fallback.isDup(name);
                return mark(index);
            }

            private void mark(String name) {
                Integer index = propertyIndexes == null ? null : propertyIndexes.get(name);
                if (index != null) mark(index);
            }

            private boolean mark(int index) {
                if (index < Long.SIZE) {
                    long bit = 1L << index;
                    boolean duplicate = (seen & bit) != 0;
                    seen |= bit;
                    return duplicate;
                }
                int word = index / Long.SIZE;
                long bit = 1L << (index % Long.SIZE);
                boolean duplicate = (seenWords[word] & bit) != 0;
                seenWords[word] |= bit;
                return duplicate;
            }
        }
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
                        ObjectMapper mapper = context.getOwner();
                        context.addBeanSerializerModifier(
                                new BeanSerializerModifier() {
                                    @Override
                                    public List<BeanPropertyWriter> changeProperties(
                                            SerializationConfig config,
                                            BeanDescription bean,
                                            List<BeanPropertyWriter> writers) {
                                        // Reuse decode null rules, including constructor
                                        // annotations.
                                        for (BeanPropertyDefinition property :
                                                mapper.getDeserializationConfig()
                                                        .introspect(bean.getType())
                                                        .findProperties()) {
                                            if (property.getMetadata().getValueNulls()
                                                    != Nulls.FAIL) continue;
                                            for (BeanPropertyWriter writer : writers) {
                                                if (writer.getName().equals(property.getName())) {
                                                    writer.assignNullSerializer(
                                                            new JsonSerializer<>() {
                                                                @Override
                                                                public void serialize(
                                                                        Object value,
                                                                        JsonGenerator generator,
                                                                        SerializerProvider provider)
                                                                        throws IOException {
                                                                    throw ZLinkJsonDiagnostic
                                                                            .nullProperty(
                                                                                    generator,
                                                                                    writer
                                                                                            .getName());
                                                                }
                                                            });
                                                }
                                            }
                                        }
                                        return writers;
                                    }
                                });
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
                        context.addBeanDeserializerModifier(
                                new BeanDeserializerModifier() {
                                    @Override
                                    public JsonDeserializer<?> modifyDeserializer(
                                            DeserializationConfig config,
                                            BeanDescription bean,
                                            JsonDeserializer<?> deserializer) {
                                        if (!(deserializer instanceof BeanDeserializerBase)) {
                                            return deserializer;
                                        }
                                        List<BeanPropertyDefinition> properties =
                                                bean.findProperties();
                                        Map<String, Integer> indexes =
                                                new HashMap<>(properties.size());
                                        for (BeanPropertyDefinition property : properties) {
                                            indexes.putIfAbsent(property.getName(), indexes.size());
                                        }
                                        return new TypedPropertyDeserializer(
                                                deserializer, Map.copyOf(indexes));
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
                            throw ZLinkJsonDiagnostic.decimalStringExpected(parser);
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
                        return readCanonicalBytes(parser, context);
                    }
                });
        return module;
    }

    private static final class TypedPropertyDeserializer extends DelegatingDeserializer {
        private final Map<String, Integer> propertyIndexes;

        private TypedPropertyDeserializer(
                JsonDeserializer<?> delegate, Map<String, Integer> propertyIndexes) {
            super(delegate);
            this.propertyIndexes = propertyIndexes;
        }

        @Override
        protected JsonDeserializer<?> newDelegatingInstance(JsonDeserializer<?> delegate) {
            return new TypedPropertyDeserializer(delegate, propertyIndexes);
        }

        @Override
        public Object deserialize(JsonParser parser, DeserializationContext context)
                throws IOException {
            useFixedPropertyNames(parser);
            return super.deserialize(parser, context);
        }

        @Override
        public Object deserialize(
                JsonParser parser, DeserializationContext context, Object intoValue)
                throws IOException {
            useFixedPropertyNames(parser);
            return super.deserialize(parser, context, intoValue);
        }

        @Override
        public Object deserializeWithType(
                JsonParser parser,
                DeserializationContext context,
                com.fasterxml.jackson.databind.jsontype.TypeDeserializer typeDeserializer)
                throws IOException {
            useFixedPropertyNames(parser);
            return super.deserializeWithType(parser, context, typeDeserializer);
        }

        private void useFixedPropertyNames(JsonParser parser) throws IOException {
            if (parser.currentToken() == JsonToken.START_OBJECT
                    && parser instanceof ProfileParser profileParser) {
                profileParser.usePropertyMask(propertyIndexes);
            }
        }
    }

    private static byte[] readCanonicalBytes(JsonParser parser, DeserializationContext context)
            throws IOException {
        int length = parser.getTextLength();
        if ((length & 3) != 0) {
            throw context.weirdStringException(
                    parser.getText(), byte[].class, "base64 padding is required");
        }
        char[] text = parser.getTextCharacters();
        int offset = parser.getTextOffset();
        int end = offset + length;
        int padding = 0;
        if (length != 0 && text[end - 1] == '=') {
            padding = text[end - 2] == '=' ? 2 : 1;
        }
        byte[] decoded = new byte[(length / 4) * 3 - padding];
        Base64Variant base64 = Base64Variants.MIME_NO_LINEFEEDS;
        int output = 0;
        for (int index = offset; index < end; index += 4) {
            int first = base64.decodeBase64Char(text[index]);
            int second = base64.decodeBase64Char(text[index + 1]);
            int third = base64.decodeBase64Char(text[index + 2]);
            int fourth = base64.decodeBase64Char(text[index + 3]);
            if ((first | second | third | fourth) < 0) {
                // Only the final quartet may contain padding, with zero unused bits.
                if (index + 4 != end
                        || first < 0
                        || second < 0
                        || fourth != Base64Variant.BASE64_VALUE_PADDING
                        || (third == Base64Variant.BASE64_VALUE_PADDING
                                ? (second & 15) != 0
                                : third < 0 || (third & 3) != 0)) {
                    throw context.weirdStringException(
                            parser.getText(),
                            byte[].class,
                            "bytes are not canonical padded base64");
                }
            }
            decoded[output++] = (byte) ((first << 2) | (second >> 4));
            if (third >= 0) {
                decoded[output++] = (byte) ((second << 4) | (third >> 2));
                if (fourth >= 0) {
                    decoded[output++] = (byte) ((third << 6) | fourth);
                }
            }
        }
        return decoded;
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
                if (property == null || property.getMetadata().isRequired()) continue;
                if (property.getType().isPrimitive()) {
                    properties[index] =
                            property.withNullProvider(
                                    new OptionalPrimitiveValueProvider(
                                            property.getType().getRawClass(), property.getName()));
                } else if (property.getMetadata().getValueNulls() == Nulls.FAIL) {
                    // findSetterInfo decided the null rule; an absent optional reference is null.
                    properties[index] =
                            property.withNullProvider(
                                    new OptionalReferenceValueProvider(property.getName()));
                }
            }
            return properties;
        }
    }

    private static final class OptionalReferenceValueProvider implements NullValueProvider {
        private final String propertyName;

        private OptionalReferenceValueProvider(String propertyName) {
            this.propertyName = propertyName;
        }

        @Override
        public Object getNullValue(DeserializationContext context) throws JsonMappingException {
            throw ZLinkJsonDiagnostic.nullProperty(context.getParser(), propertyName);
        }

        @Override
        public Object getAbsentValue(DeserializationContext context) {
            return null;
        }

        @Override
        public AccessPattern getNullAccessPattern() {
            return AccessPattern.DYNAMIC;
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
            throw ZLinkJsonDiagnostic.nullProperty(context.getParser(), propertyName);
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
