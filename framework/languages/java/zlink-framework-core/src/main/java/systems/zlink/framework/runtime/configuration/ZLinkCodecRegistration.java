package systems.zlink.framework.runtime.internal.configuration;

import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicInteger;
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
    private final Map<String, RegisteredSerializer> serializers = new LinkedHashMap<>();
    private final Map<Class<?>, String> contentTypeCache = new ConcurrentHashMap<>();
    private final AtomicInteger contentTypeCacheSize = new AtomicInteger();
    private final Map<String, ZLinkStreamCodec> streamCodecsByContentType = new LinkedHashMap<>();
    private final Map<ZLinkStreamCodec, String> contentTypesByStreamCodec = new LinkedHashMap<>();

    @Override
    public void use(systems.zlink.framework.configuration.ZLinkCodecExtension extension) {
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

    private void addSerializer(
        String contentType,
        ZLinkMessageSerializer serializer,
        Predicate<Class<?>> canSerialize,
        boolean fallbackSerializer) {
        Objects.requireNonNull(contentType, "contentType");
        Objects.requireNonNull(serializer, "serializer");
        Objects.requireNonNull(canSerialize, "canSerialize");
        String normalized = contentType.trim();
        if (normalized.isEmpty()) {
            throw new ZLinkConfigurationException("custom serializer content type must not be blank");
        }
        serializers.put(normalized, new RegisteredSerializer(serializer, canSerialize, fallbackSerializer));
        contentTypeCache.clear();
        contentTypeCacheSize.set(0);
    }

    @Override
    public void addStreamCodec(String contentType, ZLinkStreamCodec codec) {
        Objects.requireNonNull(contentType, "contentType");
        Objects.requireNonNull(codec, "codec");
        String normalized = contentType.trim();
        if (normalized.isEmpty()) {
            throw new ZLinkConfigurationException("stream codec content type must not be blank");
        }
        streamCodecsByContentType.put(normalized, codec);
        contentTypesByStreamCodec.put(codec, normalized);
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
            streamCodecsByContentType.get(contentType.trim()));
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
        String normalized = contentType.trim();
        if (DEFAULT_JSON_CONTENT_TYPE.equalsIgnoreCase(normalized)
            || LEGACY_JSON_CONTENT_TYPE.equalsIgnoreCase(normalized)) {
            return Optional.of(ZLinkStreamCodec.JSON);
        }
        return streamCodec(normalized);
    }

    public Optional<String> streamContentType(ZLinkStreamCodec codec) {
        return Optional.ofNullable(contentTypesByStreamCodec.get(codec));
    }

    public Optional<ZLinkStreamCodec> streamCodecForCustomSerializer() {
        Optional<Map.Entry<String, RegisteredSerializer>> fallbackSerializer =
            singleFallbackSerializer();
        if (fallbackSerializer.isEmpty()) {
            if (serializers.size() == 1) {
                return streamCodec(serializers.keySet().iterator().next());
            }
            return Optional.empty();
        }
        return streamCodec(fallbackSerializer.get().getKey());
    }

    /**
     * Returns the single registered custom serializer, if any. Throws when more
     * than one custom serializer is registered because the payload serializer is
     * then ambiguous.
     */
    public Optional<ZLinkMessageSerializer> customSerializer() {
        return singleFallbackSerializer()
            .map(entry -> entry.getValue().serializer());
    }

    private Optional<Map.Entry<String, RegisteredSerializer>> singleFallbackSerializer() {
        Map<String, RegisteredSerializer> fallbackSerializers = new LinkedHashMap<>();
        serializers.forEach((contentType, serializer) -> {
            if (serializer.fallbackSerializer()) {
                fallbackSerializers.put(contentType, serializer);
            }
        });

        if (fallbackSerializers.isEmpty()) {
            return Optional.empty();
        }
        if (fallbackSerializers.size() > 1) {
            throw new ZLinkConfigurationException(
                "payload serializer is ambiguous because more than one custom serializer is registered: "
                    + fallbackSerializers.keySet());
        }
        return Optional.of(fallbackSerializers.entrySet().iterator().next());
    }

    public ZLinkMessageSerializer serializerWithFallback(ZLinkMessageSerializer fallback) {
        Objects.requireNonNull(fallback, "fallback");
        if (serializers.isEmpty()) {
            return fallback;
        }
        return new CompositeSerializer(serializers, fallback);
    }

    public String contentTypeFor(Class<?> type) {
        if (type == null) {
            return DEFAULT_JSON_CONTENT_TYPE;
        }
        String cached = contentTypeCache.get(type);
        if (cached != null) {
            return cached;
        }
        String contentType = singleSerializerFor(serializers, type)
            .map(Map.Entry::getKey)
            .orElse(DEFAULT_JSON_CONTENT_TYPE);
        cacheContentType(type, contentType);
        return contentType;
    }

    /**
     * Resolves the serializer selected by an incoming wire content type.
     * Incoming non-JSON content types are strict: the JSON fallback is not
     * allowed to reinterpret a payload whose envelope selected another type.
     */
    public ZLinkMessageSerializer serializerForReceivedContentType(
        String contentType,
        ZLinkMessageSerializer jsonFallback) {
        Objects.requireNonNull(contentType, "contentType");
        Objects.requireNonNull(jsonFallback, "jsonFallback");
        String normalized = contentType.trim();
        if (normalized.isEmpty()) {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                "received payload content type must not be blank");
        }
        if (DEFAULT_JSON_CONTENT_TYPE.equalsIgnoreCase(normalized)
            || LEGACY_JSON_CONTENT_TYPE.equalsIgnoreCase(normalized)) {
            return jsonFallback;
        }
        RegisteredSerializer registered = serializers.get(normalized);
        if (registered == null) {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                "No payload serializer is registered for received content type '"
                    + normalized + "'");
        }
        return registered.serializer();
    }

    private static Optional<Map.Entry<String, RegisteredSerializer>> singleSerializerFor(
        Map<String, RegisteredSerializer> serializers,
        Class<?> type) {
        Map.Entry<String, RegisteredSerializer> match = null;
        StringBuilder ambiguousTypes = null;
        for (Map.Entry<String, RegisteredSerializer> entry : serializers.entrySet()) {
            if (!entry.getValue().canSerialize().test(type)) {
                continue;
            }
            if (match == null) {
                match = entry;
                continue;
            }
            if (ambiguousTypes == null) {
                ambiguousTypes = new StringBuilder(match.getKey());
            }
            ambiguousTypes.append(", ").append(entry.getKey());
        }
        if (match == null) {
            return Optional.empty();
        }
        if (ambiguousTypes != null) {
            throw new ZLinkConfigurationException(
                "payload serializer is ambiguous for type " + type.getName() + ": "
                    + "[" + ambiguousTypes + "]");
        }
        return Optional.of(match);
    }

    private void cacheContentType(Class<?> type, String contentType) {
        int reserved = contentTypeCacheSize.get();
        while (reserved < MAX_TYPE_CACHE_ENTRIES
            && !contentTypeCacheSize.compareAndSet(reserved, reserved + 1)) {
            reserved = contentTypeCacheSize.get();
        }
        if (reserved >= MAX_TYPE_CACHE_ENTRIES) {
            return;
        }
        String previous = contentTypeCache.putIfAbsent(type, contentType);
        if (previous != null) {
            contentTypeCacheSize.decrementAndGet();
        }
    }

    private record RegisteredSerializer(
        ZLinkMessageSerializer serializer,
        Predicate<Class<?>> canSerialize,
        boolean fallbackSerializer) {
    }

    private static final class CompositeSerializer implements ZLinkMessageSerializer {
        private final Map<String, RegisteredSerializer> serializers;
        private final ZLinkMessageSerializer fallback;
        private final Map<Class<?>, ZLinkMessageSerializer> serializerCache =
            new ConcurrentHashMap<>();
        private final AtomicInteger serializerCacheSize = new AtomicInteger();

        CompositeSerializer(
            Map<String, RegisteredSerializer> serializers,
            ZLinkMessageSerializer fallback) {
            this.serializers = Map.copyOf(serializers);
            this.fallback = fallback;
        }

        @Override
        public <T> systems.zlink.framework.ZLinkEncodedPayload serialize(T value) {
            if (value != null) {
                return serializerFor(value.getClass()).serialize(value);
            }
            return fallback.serialize(value);
        }

        @Override
        public <T> T deserialize(systems.zlink.framework.ZLinkEncodedPayload payload, Class<T> type) {
            return serializerFor(type).deserialize(payload, type);
        }

        @Override
        public void prepare(Class<?> type) {
            serializerFor(type).prepare(type);
        }

        private ZLinkMessageSerializer serializerFor(Class<?> type) {
            ZLinkMessageSerializer cached = serializerCache.get(type);
            if (cached != null) {
                return cached;
            }
            ZLinkMessageSerializer selected = singleSerializerFor(serializers, type)
                .map(entry -> entry.getValue().serializer())
                .orElse(fallback);
            int reserved = serializerCacheSize.get();
            while (reserved < MAX_TYPE_CACHE_ENTRIES
                && !serializerCacheSize.compareAndSet(reserved, reserved + 1)) {
                reserved = serializerCacheSize.get();
            }
            if (reserved < MAX_TYPE_CACHE_ENTRIES) {
                ZLinkMessageSerializer previous = serializerCache.putIfAbsent(type, selected);
                if (previous != null) {
                    serializerCacheSize.decrementAndGet();
                    return previous;
                }
            }
            return selected;
        }
    }
}
