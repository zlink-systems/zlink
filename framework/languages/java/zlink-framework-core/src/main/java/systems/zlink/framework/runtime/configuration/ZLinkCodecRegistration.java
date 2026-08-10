package systems.zlink.framework.runtime.internal.configuration;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.configuration.ZLinkCodecExtension;

import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.ConcurrentHashMap;
import java.util.function.Predicate;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.configuration.ZLinkCodecRegistryBuilder;
import systems.zlink.framework.configuration.ZLinkCodecRegistrar;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.streams.ZLinkStreamCodec;

public final class ZLinkCodecRegistration implements ZLinkCodecRegistryBuilder, ZLinkCodecRegistrar {
    private static final String DEFAULT_JSON_CONTENT_TYPE = "application/json";
    private static final String LEGACY_JSON_CONTENT_TYPE =
        "application/zlink-framework-json-v1";
    private static final int MAX_TYPE_CACHE_ENTRIES = 1024;
    private volatile Map<String, RegisteredSerializer> serializers = new LinkedHashMap<>();
    private volatile Map<String, ZLinkStreamCodec> streamCodecsByContentType =
        new LinkedHashMap<>();
    private volatile Map<ZLinkStreamCodec, String> contentTypesByStreamCodec =
        new LinkedHashMap<>();
    private final Map<Class<?>, SendSelection> sendTypeCache = new ConcurrentHashMap<>();
    private final Object sendTypeCacheGate = new Object();
    private volatile boolean frozen;

    @Override
    public synchronized void use(ZLinkCodecExtension extension) {
        requireMutable();
        Objects.requireNonNull(extension, "extension").register(this);
    }

    @Override
    public void addSerializer(String contentType, ZLinkMessageSerializer serializer) {
        addSerializer(contentType, serializer, ignored -> true, true);
    }

    @Override
    public void addSerializer(
        String contentType,
        ZLinkMessageSerializer serializer,
        Predicate<Class<?>> canSerialize) {
        addSerializer(contentType, serializer, canSerialize, false);
    }

    private synchronized void addSerializer(
        String contentType,
        ZLinkMessageSerializer serializer,
        Predicate<Class<?>> canSerialize,
        boolean fallbackSerializer) {
        requireMutable();
        Objects.requireNonNull(contentType, "contentType");
        Objects.requireNonNull(serializer, "serializer");
        Objects.requireNonNull(canSerialize, "canSerialize");
        String normalized = normalizeRegistrationContentType(
            contentType, "custom serializer");
        serializers.remove(normalized);
        serializers.put(normalized, new RegisteredSerializer(serializer, canSerialize, fallbackSerializer));
        clearSendTypeCache();
    }

    @Override
    public synchronized void addStreamCodec(String contentType, ZLinkStreamCodec codec) {
        requireMutable();
        Objects.requireNonNull(contentType, "contentType");
        Objects.requireNonNull(codec, "codec");
        String normalized = normalizeRegistrationContentType(contentType, "stream codec");
        ZLinkStreamCodec previousCodec = streamCodecsByContentType.remove(normalized);
        if (previousCodec != null) {
            contentTypesByStreamCodec.remove(previousCodec, normalized);
        }
        String previousContentType = contentTypesByStreamCodec.remove(codec);
        if (previousContentType != null && !previousContentType.equals(normalized)) {
            streamCodecsByContentType.remove(previousContentType, codec);
        }
        streamCodecsByContentType.put(normalized, codec);
        contentTypesByStreamCodec.put(codec, normalized);
    }

    /** Makes the startup registry immutable and safely publishes its receive tables. */
    public synchronized void freeze() {
        if (frozen) {
            return;
        }
        serializers = immutableOrderedCopy(serializers);
        streamCodecsByContentType = immutableOrderedCopy(streamCodecsByContentType);
        contentTypesByStreamCodec = immutableOrderedCopy(contentTypesByStreamCodec);
        frozen = true;
    }

    public Map<String, ZLinkMessageSerializer> serializers() {
        Map<String, ZLinkMessageSerializer> snapshot = new LinkedHashMap<>();
        serializers.forEach((contentType, serializer) -> snapshot.put(contentType, serializer.serializer()));
        return Collections.unmodifiableMap(snapshot);
    }

    public Optional<ZLinkStreamCodec> streamCodec(String contentType) {
        if (contentType == null) {
            return Optional.empty();
        }
        return Optional.ofNullable(
            streamCodecsByContentType.get(
                normalizeRegistrationContentType(contentType, "stream codec")));
    }

    /**
     * Resolves the stream marker carried by an incoming application envelope.
     * JSON is the only built-in content type; every other type must have an
     * explicit immutable registration.
     */
    public Optional<ZLinkStreamCodec> streamCodecForReceivedContentType(
        String contentType) {
        if (contentType == null) {
            return Optional.empty();
        }
        if (!isCanonicalWireContentType(contentType)) {
            return Optional.empty();
        }
        if (DEFAULT_JSON_CONTENT_TYPE.equals(contentType)
            || LEGACY_JSON_CONTENT_TYPE.equals(contentType)) {
            return Optional.of(ZLinkStreamCodec.JSON);
        }
        return Optional.ofNullable(streamCodecsByContentType.get(contentType));
    }

    public Optional<String> streamContentType(ZLinkStreamCodec codec) {
        String registered = contentTypesByStreamCodec.get(codec);
        if (registered != null) {
            return Optional.of(registered);
        }
        return codec == ZLinkStreamCodec.JSON
            ? Optional.of(DEFAULT_JSON_CONTENT_TYPE)
            : Optional.empty();
    }

    /** Resolves the content type represented by an incoming STREAM codec marker. */
    public String contentTypeForReceivedStreamCodec(ZLinkStreamCodec codec) {
        Objects.requireNonNull(codec, "codec");
        return streamContentType(codec).orElseThrow(() -> protocolError(
            "No payload content type is registered for received STREAM codec '"
                + codec + "'"));
    }

    public Optional<ZLinkStreamCodec> streamCodecForCustomSerializer() {
        Optional<Map.Entry<String, RegisteredSerializer>> fallbackSerializer =
            lastFallbackSerializer();
        if (fallbackSerializer.isEmpty()) {
            if (serializers.size() == 1) {
                return streamCodec(serializers.keySet().iterator().next());
            }
            return Optional.empty();
        }
        return streamCodec(fallbackSerializer.get().getKey());
    }

    /** Returns the last registered serializer that applies to every declared type. */
    public Optional<ZLinkMessageSerializer> customSerializer() {
        return lastFallbackSerializer()
            .map(entry -> entry.getValue().serializer());
    }

    private Optional<Map.Entry<String, RegisteredSerializer>> lastFallbackSerializer() {
        Map.Entry<String, RegisteredSerializer> match = null;
        for (Map.Entry<String, RegisteredSerializer> entry : serializers.entrySet()) {
            if (entry.getValue().fallbackSerializer()) {
                match = entry;
            }
        }
        return Optional.ofNullable(match);
    }

    public ZLinkMessageSerializer serializerWithFallback(ZLinkMessageSerializer fallback) {
        Objects.requireNonNull(fallback, "fallback");
        if (serializers.isEmpty()) {
            return fallback;
        }
        return new CompositeSerializer(this, fallback);
    }

    public String contentTypeFor(Class<?> type) {
        return sendSelectionFor(type).contentType();
    }

    public ZLinkMessageSerializer serializerForSending(
        Class<?> declaredType,
        ZLinkMessageSerializer jsonFallback) {
        Objects.requireNonNull(jsonFallback, "jsonFallback");
        RegisteredSerializer selected = sendSelectionFor(declaredType).serializer();
        return selected == null ? jsonFallback : selected.serializer();
    }

    /**
     * Resolves the serializer selected by an incoming wire content type.
     * Incoming non-JSON content types are strict: the JSON fallback is not
     * allowed to reinterpret a payload whose envelope selected another type.
     */
    public ZLinkMessageSerializer serializerForReceivedContentType(
        String contentType,
        ZLinkMessageSerializer jsonFallback) {
        Objects.requireNonNull(jsonFallback, "jsonFallback");
        if (!isCanonicalWireContentType(contentType)) {
            throw protocolError(
                "received payload content type is not a canonical bare media type");
        }
        if (DEFAULT_JSON_CONTENT_TYPE.equals(contentType)
            || LEGACY_JSON_CONTENT_TYPE.equals(contentType)) {
            return jsonFallback;
        }
        RegisteredSerializer registered = serializers.get(contentType);
        if (registered == null) {
            throw protocolError(
                "No payload serializer is registered for received content type '"
                    + contentType + "'");
        }
        return registered.serializer();
    }

    public static <T> ZLinkEncodedPayload serializeForContentType(
        ZLinkMessageSerializer serializer,
        T value,
        String contentType) {
        if (serializer instanceof CompositeSerializer composite) {
            return composite.serializeForContentType(value, contentType);
        }
        return serializer.serialize(value);
    }

    public static <T> ZLinkEncodedPayload serializeForDeclaredType(
        ZLinkMessageSerializer serializer,
        T value,
        Class<?> declaredType) {
        if (serializer instanceof CompositeSerializer composite) {
            return composite.serializeForDeclaredType(value, declaredType);
        }
        return serializer.serialize(value, declaredType);
    }

    public static ZLinkMessageSerializer serializerForReceivedContentType(
        ZLinkMessageSerializer serializer,
        String contentType) {
        Objects.requireNonNull(serializer, "serializer");
        if (serializer instanceof CompositeSerializer composite) {
            return composite.serializerForReceivedContentType(contentType);
        }
        if (!isCanonicalWireContentType(contentType)) {
            throw protocolError(
                "received payload content type is not a canonical bare media type");
        }
        if (DEFAULT_JSON_CONTENT_TYPE.equals(contentType)
            || LEGACY_JSON_CONTENT_TYPE.equals(contentType)) {
            return serializer;
        }
        throw protocolError(
            "No payload serializer is registered for received content type '"
                + contentType + "'");
    }

    public static ZLinkMessageSerializer serializerForReceivedStreamCodec(
        ZLinkMessageSerializer serializer,
        ZLinkStreamCodec codec) {
        Objects.requireNonNull(serializer, "serializer");
        Objects.requireNonNull(codec, "codec");
        if (serializer instanceof CompositeSerializer composite) {
            return composite.serializerForReceivedStreamCodec(codec);
        }
        if (codec == ZLinkStreamCodec.JSON
            || codec == ZLinkStreamCodec.RAW) {
            return serializer;
        }
        throw protocolError(
            "No payload serializer is registered for received STREAM codec '"
                + codec + "'");
    }

    public static ZLinkStreamCodec streamCodecForDeclaredType(
        ZLinkMessageSerializer serializer,
        Class<?> declaredType,
        ZLinkStreamCodec fallback) {
        Objects.requireNonNull(serializer, "serializer");
        Objects.requireNonNull(fallback, "fallback");
        if (serializer instanceof CompositeSerializer composite) {
            return composite.streamCodecForDeclaredType(declaredType);
        }
        return fallback;
    }

    public static String contentTypeForDeclaredType(
        ZLinkMessageSerializer serializer,
        Class<?> declaredType,
        String fallback) {
        Objects.requireNonNull(serializer, "serializer");
        Objects.requireNonNull(fallback, "fallback");
        if (serializer instanceof CompositeSerializer composite) {
            return composite.registration.contentTypeFor(declaredType);
        }
        return fallback;
    }

    private static Optional<Map.Entry<String, RegisteredSerializer>> lastSerializerFor(
        Map<String, RegisteredSerializer> serializers,
        Class<?> type) {
        Map.Entry<String, RegisteredSerializer> match = null;
        for (Map.Entry<String, RegisteredSerializer> entry : serializers.entrySet()) {
            if (entry.getValue().canSerialize().test(type)) {
                match = entry;
            }
        }
        return Optional.ofNullable(match);
    }

    private SendSelection sendSelectionFor(Class<?> type) {
        if (type == null) {
            return SendSelection.JSON;
        }
        SendSelection cached = sendTypeCache.get(type);
        if (cached != null) {
            return cached;
        }
        synchronized (sendTypeCacheGate) {
            cached = sendTypeCache.get(type);
            if (cached != null) {
                return cached;
            }
            SendSelection selected = lastSerializerFor(serializers, type)
                .map(entry -> new SendSelection(entry.getKey(), entry.getValue()))
                .orElse(SendSelection.JSON);
            if (sendTypeCache.size() < MAX_TYPE_CACHE_ENTRIES) {
                sendTypeCache.put(type, selected);
            }
            return selected;
        }
    }

    private ZLinkMessageSerializer serializerForContentType(
        String contentType,
        ZLinkMessageSerializer jsonFallback) {
        if (DEFAULT_JSON_CONTENT_TYPE.equals(contentType)
            || LEGACY_JSON_CONTENT_TYPE.equals(contentType)) {
            return jsonFallback;
        }
        RegisteredSerializer selected = serializers.get(contentType);
        if (selected == null) {
            throw new ZLinkConfigurationException(
                "no payload serializer is registered for selected content type '"
                    + contentType + "'");
        }
        return selected.serializer();
    }

    private ZLinkStreamCodec streamCodecForSending(Class<?> declaredType) {
        SendSelection selected = sendSelectionFor(declaredType);
        if (DEFAULT_JSON_CONTENT_TYPE.equals(selected.contentType())
            || LEGACY_JSON_CONTENT_TYPE.equals(selected.contentType())) {
            return ZLinkStreamCodec.JSON;
        }
        ZLinkStreamCodec streamCodec =
            streamCodecsByContentType.get(selected.contentType());
        if (streamCodec == null) {
            throw new ZLinkConfigurationException(
                "selected payload content type '" + selected.contentType()
                    + "' does not have a STREAM codec mapping");
        }
        return streamCodec;
    }

    private void clearSendTypeCache() {
        synchronized (sendTypeCacheGate) {
            sendTypeCache.clear();
        }
    }

    private void requireMutable() {
        if (frozen) {
            throw new ZLinkConfigurationException(
                "codec registry is immutable after framework startup");
        }
    }

    private static String normalizeRegistrationContentType(String contentType, String label) {
        int start = 0;
        int end = contentType.length();
        while (start < end && isAsciiOuterWhitespace(contentType.charAt(start))) {
            start++;
        }
        while (end > start && isAsciiOuterWhitespace(contentType.charAt(end - 1))) {
            end--;
        }
        int slash = -1;
        char[] normalized = new char[end - start];
        for (int source = start, target = 0; source < end; source++, target++) {
            char value = contentType.charAt(source);
            if (value == '/') {
                if (slash >= 0) {
                    throw invalidContentType(label, contentType);
                }
                slash = target;
                normalized[target] = value;
                continue;
            }
            if (!isTokenCharacter(value)) {
                throw invalidContentType(label, contentType);
            }
            normalized[target] = value >= 'A' && value <= 'Z'
                ? (char) (value + ('a' - 'A'))
                : value;
        }
        if (slash <= 0 || slash >= normalized.length - 1) {
            throw invalidContentType(label, contentType);
        }
        return new String(normalized);
    }

    private static boolean isCanonicalWireContentType(String contentType) {
        if (contentType == null || contentType.isEmpty()) {
            return false;
        }
        int slash = -1;
        for (int index = 0; index < contentType.length(); index++) {
            char value = contentType.charAt(index);
            if (value == '/') {
                if (slash >= 0) {
                    return false;
                }
                slash = index;
                continue;
            }
            if (!isTokenCharacter(value) || (value >= 'A' && value <= 'Z')) {
                return false;
            }
        }
        return slash > 0 && slash < contentType.length() - 1;
    }

    private static boolean isAsciiOuterWhitespace(char value) {
        return value == ' ' || value == '\t';
    }

    private static boolean isTokenCharacter(char value) {
        return value >= 'a' && value <= 'z'
            || value >= 'A' && value <= 'Z'
            || value >= '0' && value <= '9'
            || switch (value) {
                case '!', '#', '$', '%', '&', '\'', '*', '+', '-', '.', '^', '_', '`', '|', '~' -> true;
                default -> false;
            };
    }

    private static ZLinkConfigurationException invalidContentType(
        String label,
        String contentType) {
        return new ZLinkConfigurationException(
            label + " content type must be a bare type/subtype media type: '"
                + contentType + "'");
    }

    private static ZLinkFrameworkException protocolError(String message) {
        return new ZLinkFrameworkException(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, message);
    }

    private static <K, V> Map<K, V> immutableOrderedCopy(Map<K, V> source) {
        return Collections.unmodifiableMap(new LinkedHashMap<>(source));
    }

    private record RegisteredSerializer(
        ZLinkMessageSerializer serializer,
        Predicate<Class<?>> canSerialize,
        boolean fallbackSerializer) {
    }

    private record SendSelection(
        String contentType,
        RegisteredSerializer serializer) {
        private static final SendSelection JSON =
            new SendSelection(DEFAULT_JSON_CONTENT_TYPE, null);
    }

    private static final class CompositeSerializer implements ZLinkMessageSerializer {
        private final ZLinkCodecRegistration registration;
        private final ZLinkMessageSerializer fallback;

        CompositeSerializer(
            ZLinkCodecRegistration registration,
            ZLinkMessageSerializer fallback) {
            this.registration = registration;
            this.fallback = fallback;
        }

        @Override
        public <T> ZLinkEncodedPayload serialize(T value) {
            if (value != null) {
                return serialize(value, value.getClass());
            }
            return fallback.serialize(value);
        }

        @Override
        public <T> ZLinkEncodedPayload serialize(T value, Class<?> declaredType) {
            Objects.requireNonNull(declaredType, "declaredType");
            return registration.serializerForSending(declaredType, fallback)
                .serialize(value, declaredType);
        }

        @Override
        public <T> T deserialize(ZLinkEncodedPayload payload, Class<T> type) {
            return registration.serializerForSending(type, fallback)
                .deserialize(payload, type);
        }

        @Override
        public void prepare(Class<?> type) {
            registration.serializerForSending(type, fallback).prepare(type);
        }

        private <T> ZLinkEncodedPayload serializeForContentType(
            T value,
            String contentType) {
            return registration.serializerForContentType(contentType, fallback)
                .serialize(value);
        }

        private <T> ZLinkEncodedPayload serializeForDeclaredType(
            T value,
            Class<?> declaredType) {
            return serialize(value, declaredType);
        }

        private ZLinkStreamCodec streamCodecForDeclaredType(Class<?> declaredType) {
            return registration.streamCodecForSending(declaredType);
        }

        private ZLinkMessageSerializer serializerForReceivedContentType(
            String contentType) {
            return registration.serializerForReceivedContentType(contentType, fallback);
        }

        private ZLinkMessageSerializer serializerForReceivedStreamCodec(
            ZLinkStreamCodec codec) {
            if (codec == ZLinkStreamCodec.RAW) {
                return fallback;
            }
            return serializerForReceivedContentType(
                registration.contentTypeForReceivedStreamCodec(codec));
        }
    }
}
