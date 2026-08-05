package systems.zlink.framework.testkit;

import java.util.ArrayList;
import java.util.List;
import java.util.function.Predicate;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.configuration.ZLinkCodecExtension;
import systems.zlink.framework.configuration.ZLinkCodecRegistrar;
import systems.zlink.framework.configuration.ZLinkCodecRegistryBuilder;
import systems.zlink.framework.streams.ZLinkStreamCodec;

/** Test-only public-SPI codec registry used by fake-backend contract tests. */
final class PublicCodecTestRegistry
    implements ZLinkCodecRegistryBuilder, ZLinkCodecRegistrar {
    private final List<RegisteredSerializer> serializers =
        new ArrayList<>();

    @Override
    public void use(ZLinkCodecExtension extension) {
        extension.register(this);
    }

    @Override
    public void addSerializer(
        String contentType,
        ZLinkMessageSerializer serializer) {
        addSerializer(contentType, serializer, ignored -> true);
    }

    @Override
    public void addSerializer(
        String contentType,
        ZLinkMessageSerializer serializer,
        Predicate<Class<?>> canSerialize) {
        serializers.add(
            new RegisteredSerializer(serializer, canSerialize));
    }

    @Override
    public void addStreamCodec(
        String contentType,
        ZLinkStreamCodec codec) {
        // Fake-backend tests exercise framework payload serialization only.
    }

    ZLinkMessageSerializer serializerWithFallback(
        ZLinkMessageSerializer fallback) {
        return new ZLinkMessageSerializer() {
            @Override
            public <T> ZLinkEncodedPayload serialize(T value) {
                return serializerFor(
                    value == null ? null : value.getClass(),
                    fallback).serialize(value);
            }

            @Override
            public <T> T deserialize(
                ZLinkEncodedPayload payload,
                Class<T> type) {
                return serializerFor(type, fallback)
                    .deserialize(payload, type);
            }

            @Override
            public void prepare(Class<?> type) {
                serializerFor(type, fallback).prepare(type);
            }
        };
    }

    private ZLinkMessageSerializer serializerFor(
        Class<?> type,
        ZLinkMessageSerializer fallback) {
        return serializers.stream()
            .filter(candidate ->
                type != null && candidate.canSerialize().test(type))
            .map(RegisteredSerializer::serializer)
            .findFirst()
            .orElse(fallback);
    }

    private record RegisteredSerializer(
        ZLinkMessageSerializer serializer,
        Predicate<Class<?>> canSerialize) {
    }
}
