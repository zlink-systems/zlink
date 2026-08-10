package systems.zlink.framework.runtime.configuration;
import java.util.Optional;

import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.nio.charset.StandardCharsets;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.configuration.ZLinkCodecRegistrar;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.messaging.ZLinkPayloadEncoding;
import systems.zlink.framework.streams.ZLinkStreamCodec;

final class ZLinkCodecRegistrationTest {
    @Test
    void registersSingleCustomSerializerAndRoundTripsPayload() {
        ZLinkCodecRegistration registration = new ZLinkCodecRegistration();
        registration.addSerializer("application/avro", new MarkerSerializer());

        ZLinkMessageSerializer serializer = registration.customSerializer().orElseThrow();
        ZLinkPayloadEncoding.EncodedPayload encoded =
            ZLinkPayloadEncoding.encode(serializer, new Probe("hello"));

        assertEquals("Probe", encoded.packetName());
        assertEquals("AVRO:hello", new String(encoded.payload().toByteArray(), StandardCharsets.UTF_8));

        Probe decoded =
            serializer.deserialize(ZLinkEncodedPayload.from(encoded.payload().toByteArray()), Probe.class);
        assertEquals(new Probe("hello"), decoded);
    }

    @Test
    void rejectsBlankContentType() {
        ZLinkCodecRegistration registration = new ZLinkCodecRegistration();

        assertThrows(
            ZLinkConfigurationException.class,
            () -> registration.addSerializer("   ", new MarkerSerializer()));
    }

    @Test
    void codecExtensionCanRegisterCustomSerializer() {
        ZLinkCodecRegistration registration = new ZLinkCodecRegistration();

        registration.use(MarkerCodecExtension::register);

        ZLinkMessageSerializer serializer = registration.customSerializer().orElseThrow();
        ZLinkPayloadEncoding.EncodedPayload encoded =
            ZLinkPayloadEncoding.encode(serializer, new Probe("hello"));

        assertEquals("AVRO:hello", new String(encoded.payload().toByteArray(), StandardCharsets.UTF_8));
    }

    @Test
    void laterFallbackSerializerReplacesEarlierSelection() {
        ZLinkCodecRegistration registration = new ZLinkCodecRegistration();
        ZLinkMessageSerializer avro = new MarkerSerializer();
        ZLinkMessageSerializer thrift = new MarkerSerializer();
        registration.addSerializer("application/avro", avro);
        registration.addSerializer("application/thrift", thrift);

        assertSame(thrift, registration.customSerializer().orElseThrow());
        assertEquals("application/thrift", registration.contentTypeFor(Probe.class));
    }

    @Test
    void incomingJsonUsesFallbackAndRegisteredTypeUsesItsSerializer() {
        ZLinkCodecRegistration registration = new ZLinkCodecRegistration();
        ZLinkMessageSerializer json = new MarkerSerializer();
        ZLinkMessageSerializer avro = new MarkerSerializer();
        registration.addSerializer("application/avro", avro);

        assertSame(
            json,
            registration.serializerForReceivedContentType("application/json", json));
        assertSame(
            avro,
            registration.serializerForReceivedContentType("application/avro", json));
        assertSame(
            json,
            registration.serializerForReceivedContentType(
                "application/zlink-framework-json-v1", json));

        ZLinkMessageSerializer composite =
            registration.serializerWithFallback(json);
        assertSame(
            json,
            ZLinkCodecRegistration.serializerForReceivedContentType(
                composite, "application/json"));
        assertSame(
            avro,
            ZLinkCodecRegistration.serializerForReceivedContentType(
                composite, "application/avro"));
    }

    @Test
    void unknownIncomingNonJsonTypeIsProtocolError() {
        ZLinkCodecRegistration registration = new ZLinkCodecRegistration();
        ZLinkMessageSerializer json = new MarkerSerializer();

        ZLinkFrameworkException error = assertThrows(
            ZLinkFrameworkException.class,
            () -> registration.serializerForReceivedContentType(
                "application/x-unregistered", json));

        assertEquals(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, error.kind());
    }

    @Test
    void incomingStreamCodecUsesRegisteredTypeWithoutJsonFallback() {
        ZLinkCodecRegistration registration = new ZLinkCodecRegistration();
        registration.addStreamCodec(
            " application/x-protobuf ", ZLinkStreamCodec.PROTOBUF);

        assertEquals(
            Optional.empty(),
            registration.streamCodecForReceivedContentType("APPLICATION/JSON"));
        assertEquals(
            Optional.of(ZLinkStreamCodec.PROTOBUF),
            registration.streamCodecForReceivedContentType(
                "application/x-protobuf"));
        assertEquals(
            Optional.empty(),
            registration.streamCodecForReceivedContentType(
                "application/x-unregistered"));
    }

    @Test
    void incomingStreamCodecSelectsItsExactSerializerAndRejectsUnmappedMarkers() {
        ZLinkCodecRegistration registration = new ZLinkCodecRegistration();
        ZLinkMessageSerializer json = new MarkerSerializer();
        ZLinkMessageSerializer protobuf = new MarkerSerializer();
        registration.addSerializer(
            "application/x-protobuf", protobuf, Probe.class::equals);
        registration.addStreamCodec(
            "application/x-protobuf", ZLinkStreamCodec.PROTOBUF);
        registration.freeze();
        ZLinkMessageSerializer composite =
            registration.serializerWithFallback(json);

        assertSame(
            protobuf,
            ZLinkCodecRegistration.serializerForReceivedStreamCodec(
                composite, ZLinkStreamCodec.PROTOBUF));
        assertSame(
            json,
            ZLinkCodecRegistration.serializerForReceivedStreamCodec(
                composite, ZLinkStreamCodec.JSON));
        assertSame(
            json,
            ZLinkCodecRegistration.serializerForReceivedStreamCodec(
                composite, ZLinkStreamCodec.RAW));
        ZLinkFrameworkException failure = assertThrows(
            ZLinkFrameworkException.class,
            () -> ZLinkCodecRegistration.serializerForReceivedStreamCodec(
                composite, ZLinkStreamCodec.MESSAGE_PACK));
        assertEquals(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, failure.kind());
    }

    @Test
    void normalizedStreamReplacementKeepsForwardAndReverseMapsConsistent() {
        ZLinkCodecRegistration registration = new ZLinkCodecRegistration();
        registration.addStreamCodec(
            " \tApplication/X-Binary\t ", ZLinkStreamCodec.PROTOBUF);
        registration.addStreamCodec(
            "application/x-binary", ZLinkStreamCodec.MESSAGE_PACK);

        assertEquals(
            Optional.of(ZLinkStreamCodec.MESSAGE_PACK),
            registration.streamCodec("application/x-binary"));
        assertEquals(Optional.empty(), registration.streamContentType(ZLinkStreamCodec.PROTOBUF));

        registration.addStreamCodec(
            "application/x-message-pack", ZLinkStreamCodec.MESSAGE_PACK);

        assertEquals(Optional.empty(), registration.streamCodec("application/x-binary"));
        assertEquals(
            Optional.of("application/x-message-pack"),
            registration.streamContentType(ZLinkStreamCodec.MESSAGE_PACK));
    }

    record Probe(String text) {
    }

    static final class MarkerSerializer implements ZLinkMessageSerializer {
        @Override
        public <T> ZLinkEncodedPayload serialize(T value) {
            Probe probe = (Probe) value;
            return ZLinkEncodedPayload.from(("AVRO:" + probe.text()).getBytes(StandardCharsets.UTF_8));
        }

        @Override
        public <T> T deserialize(ZLinkEncodedPayload payload, Class<T> type) {
            String text = new String(payload.bytes(), StandardCharsets.UTF_8);
            String value = text.startsWith("AVRO:") ? text.substring("AVRO:".length()) : text;
            return type.cast(new Probe(value));
        }
    }

    static final class MarkerCodecExtension {
        static void register(ZLinkCodecRegistrar codecs) {
            codecs.addSerializer("application/avro", new MarkerSerializer());
        }
    }
}
