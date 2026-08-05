package systems.zlink.framework.codecs.protobuf;

import static org.junit.jupiter.api.Assertions.assertEquals;

import com.google.protobuf.StringValue;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.function.Predicate;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.configuration.ZLinkCodecExtension;
import systems.zlink.framework.configuration.ZLinkCodecRegistrar;
import systems.zlink.framework.configuration.ZLinkCodecRegistryBuilder;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;
import systems.zlink.framework.streams.ZLinkStreamCodec;

final class ZLinkProtobufCodecTest {
    @Test
    void protobufExtensionUsesMessageLiteBytesAndKeepsJsonFallback() {
        PublicCodecRegistry registration = new PublicCodecRegistry();
        registration.use(ZLinkProtobufCodec.defaultCodec());
        ZLinkMessageSerializer serializer =
            registration.serializerWithFallback(new ZLinkJsonMessageSerializer());

        StringValue original = StringValue.of("profile:42");
        StringValue decoded = serializer.deserialize(
            serializer.serialize(original),
            StringValue.class);
        assertEquals(original, decoded);

        ProfileReply fallback = serializer.deserialize(
            serializer.serialize(new ProfileReply("json:42")),
            ProfileReply.class);
        assertEquals(new ProfileReply("json:42"), fallback);
    }

    record ProfileReply(String id) {
    }

    private static final class PublicCodecRegistry
        implements ZLinkCodecRegistryBuilder, ZLinkCodecRegistrar {
        private final Map<String, RegisteredSerializer> serializers =
            new LinkedHashMap<>();

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
            serializers.put(
                contentType,
                new RegisteredSerializer(serializer, canSerialize));
        }

        @Override
        public void addStreamCodec(
            String contentType,
            ZLinkStreamCodec codec) {
            // This test verifies typed framework payload serialization.
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
            return serializers.values().stream()
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
}
