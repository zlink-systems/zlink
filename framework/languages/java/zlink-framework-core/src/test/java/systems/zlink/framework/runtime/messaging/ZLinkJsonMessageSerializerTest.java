package systems.zlink.framework.runtime.messaging;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasterxml.jackson.core.JsonParser;
import com.fasterxml.jackson.databind.DeserializationContext;
import com.fasterxml.jackson.databind.JsonDeserializer;
import com.fasterxml.jackson.databind.JsonMappingException;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.annotation.JsonDeserialize;
import com.fasterxml.jackson.databind.exc.InvalidFormatException;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ActorRefSnapshot;
import systems.zlink.framework.runtime.internal.json.ZLinkFrameworkJsonProfile;
import systems.zlink.framework.spots.SpotRef;

import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Map;
import java.util.stream.IntStream;

final class ZLinkJsonMessageSerializerTest {
    @Test
    void rawArrayDecodeRespectsHeapBufferRange() {
        ZLinkJsonMessageSerializer serializer = new ZLinkJsonMessageSerializer();
        byte[] backing = {9, 1, 2, 3, 8};
        ByteBuffer range = ByteBuffer.wrap(backing, 1, 3);
        for (ByteBuffer input : List.of(range, range.slice(), range.asReadOnlyBuffer())) {
            assertArrayEquals(new byte[] {1, 2, 3}, serializer.decodeJson(input, byte[].class));
        }
        assertArrayEquals(backing, serializer.decodeJson(ByteBuffer.wrap(backing), byte[].class));
    }

    @Test
    void borrowedTypedJsonPreservesTheArrayDecoderResultsAndErrors() throws Exception {
        ZLinkJsonMessageSerializer serializer = new ZLinkJsonMessageSerializer();
        for (String json : List.of("{\"value\":\"한글😀\"}", "null", "{\"value\":\"first\"} {}")) {
            try (Message payload = Message.from(json)) {
                assertEquals(
                        ZLinkFrameworkJsonProfile.mapper()
                                .readValue(
                                        json.getBytes(StandardCharsets.UTF_8), ProfileReply.class),
                        serializer.deserializeBorrowed(payload, ProfileReply.class));
            }
        }
        for (String json : List.of("", "{", "{\"value\":", "{\"value\":[]}")) {
            try (Message payload = Message.from(json)) {
                assertThrows(
                        IllegalArgumentException.class,
                        () -> serializer.deserializeBorrowed(payload, ProfileReply.class));
            }
        }
        ActorRef expected = new ActorRef("actor", 7, "mesh", RoutingId.from(new byte[] {1}));
        try (Message payload = serializer.serializeOwned(expected)) {
            assertEquals(expected, serializer.deserializeBorrowed(payload, ActorRef.class));
        }
        SpotRef spot = new SpotRef("spot", 9, "mesh", RoutingId.from(new byte[] {2}));
        try (Message payload = serializer.serializeOwned(spot)) {
            assertEquals(spot, serializer.deserializeBorrowed(payload, SpotRef.class));
        }
    }

    @Test
    void nativeAndOwnedJsonKeepTheProfilesBomRejection() {
        ZLinkJsonMessageSerializer serializer = new ZLinkJsonMessageSerializer();
        byte[] bomJson = "\uFEFF{\"value\":\"blocked\"}".getBytes(StandardCharsets.UTF_8);
        assertThrows(
                IllegalArgumentException.class,
                () ->
                        serializer.deserialize(
                                ZLinkEncodedPayload.from(bomJson), ProfileReply.class));
        try (Message payload = Message.from(bomJson)) {
            assertThrows(
                    IllegalArgumentException.class,
                    () -> serializer.deserializeBorrowed(payload, ProfileReply.class));
            assertArrayEquals(bomJson, serializer.deserializeBorrowed(payload, byte[].class));
        }
    }

    @Test
    void borrowedRawValuesOwnTheirStorageAfterTheInputCloses() {
        ZLinkJsonMessageSerializer serializer = new ZLinkJsonMessageSerializer();
        byte[] bytes;
        Message copied;
        try (Message payload = Message.from("raw-value")) {
            bytes = serializer.deserializeBorrowed(payload, byte[].class);
            copied = serializer.deserializeBorrowed(payload, Message.class);
        }
        try (copied) {
            assertEquals("raw-value", copied.toUtf8String());
            assertEquals("raw-value", new String(bytes, StandardCharsets.UTF_8));
        }
    }

    @Test
    void borrowedTypedJsonPreservesParserCodecForAnnotatedDeserializers() {
        ZLinkJsonMessageSerializer serializer = new ZLinkJsonMessageSerializer();
        try (Message payload = Message.from("{\"value\":\"custom\"}")) {
            assertEquals(
                    new CodecAwareValue("custom"),
                    serializer.deserializeBorrowed(payload, CodecAwareValue.class));
            assertEquals(
                    new CodecAwareRecord("custom"),
                    serializer.deserializeBorrowed(payload, CodecAwareRecord.class));
        }
    }

    @JsonDeserialize(using = CodecAwareDeserializer.class)
    record CodecAwareValue(String value) {}

    public static final class CodecAwareDeserializer extends JsonDeserializer<CodecAwareValue> {
        @Override
        public CodecAwareValue deserialize(JsonParser parser, DeserializationContext context)
                throws IOException {
            JsonNode node = parser.readValueAsTree();
            return new CodecAwareValue(node.get("value").textValue());
        }
    }

    @JsonDeserialize(using = CodecAwareRecordDeserializer.class)
    record CodecAwareRecord(String value) {}

    public static final class CodecAwareRecordDeserializer
            extends JsonDeserializer<CodecAwareRecord> {
        @Override
        public CodecAwareRecord deserialize(JsonParser parser, DeserializationContext context)
                throws IOException {
            return new CodecAwareRecord(parser.readValueAs(ProfileReply.class).value());
        }
    }

    @Test
    void signed64AcceptsCanonicalBoundariesAndEscapedAscii() throws Exception {
        ObjectMapper mapper = ZLinkFrameworkJsonProfile.mapper();
        for (long expected : new long[] {0, 1, -1, Long.MIN_VALUE, Long.MAX_VALUE}) {
            String json = "\"" + expected + "\"";
            assertEquals(expected, mapper.readValue(json, Long.class).longValue());
            assertEquals(expected, mapper.readValue(json, Long.TYPE).longValue());
        }
        assertEquals(12L, mapper.readValue("\"\\u0031\\u0032\"", Long.class).longValue());
        assertEquals(0L, mapper.readValue("\"\\u0030\"", Long.class).longValue());
    }

    @Test
    void signed64PreservesTokenCanonicalAndRangeErrorCategories() throws Exception {
        ObjectMapper mapper = ZLinkFrameworkJsonProfile.mapper();
        for (String invalid :
                List.of(
                        "-0", "+1", "00", "", " ", " 1", "1 ", "1e2", "-", "１", "١", "1\n",
                        "1.0")) {
            JsonMappingException failure =
                    assertThrows(
                            JsonMappingException.class,
                            () -> mapper.readValue(mapper.writeValueAsString(invalid), Long.class),
                            invalid);
            assertEquals(JsonMappingException.class, failure.getClass(), invalid);
            assertEquals(
                    "64-bit integer is not a canonical decimal string",
                    failure.getOriginalMessage(),
                    invalid);
        }
        for (String overflow : List.of("9223372036854775808", "-9223372036854775809")) {
            InvalidFormatException failure =
                    assertThrows(
                            InvalidFormatException.class,
                            () -> mapper.readValue("\"" + overflow + "\"", Long.class),
                            overflow);
            assertTrue(
                    failure.getOriginalMessage()
                            .contains("64-bit integer is outside its supported range"));
        }
        for (String token : List.of("0", "1.0", "true", "{}", "[]")) {
            JsonMappingException failure =
                    assertThrows(
                            JsonMappingException.class,
                            () -> mapper.readValue(token, Long.class),
                            token);
            assertEquals(
                    "64-bit integers must be decimal JSON strings", failure.getOriginalMessage());
        }
        JsonMappingException escapedNoncanonical =
                assertThrows(
                        JsonMappingException.class,
                        () -> mapper.readValue("\"-\\u0030\"", Long.class));
        assertEquals(
                "64-bit integer is not a canonical decimal string",
                escapedNoncanonical.getOriginalMessage());
    }

    @Test
    void consumesEveryFrameworkJsonV1GoldenCase() throws Exception {
        ObjectMapper fixtureMapper = new ObjectMapper();
        GoldenFixture fixture;
        try (InputStream input = getClass().getResourceAsStream("/framework-json-v1.json")) {
            if (input == null) {
                throw new AssertionError("framework-json-v1 fixture is missing");
            }
            fixture = fixtureMapper.readValue(input, GoldenFixture.class);
        }
        assertEquals("framework-json-v1", fixture.format());

        ZLinkJsonMessageSerializer serializer = new ZLinkJsonMessageSerializer();
        for (GoldenCase valid : fixture.valid()) {
            GoldenValue decoded =
                    serializer.deserialize(
                            ZLinkEncodedPayload.from(
                                    valid.jsonUtf8().getBytes(StandardCharsets.UTF_8)),
                            GoldenValue.class);
            assertEquals(Long.MIN_VALUE, decoded.signed64());
            assertEquals("18446744073709551615", decoded.unsigned64());
            assertEquals(GoldenState.Ready, decoded.state());
            assertEquals(
                    List.of((byte) 1, (byte) 2),
                    IntStream.range(0, decoded.bytes().length)
                            .mapToObj(index -> decoded.bytes()[index])
                            .toList());
        }
        for (GoldenCase invalid : fixture.invalid()) {
            assertThrows(
                    IllegalArgumentException.class,
                    () ->
                            serializer.deserialize(
                                    ZLinkEncodedPayload.from(
                                            invalid.jsonUtf8().getBytes(StandardCharsets.UTF_8)),
                                    GoldenValue.class),
                    invalid.name());
        }
    }

    @Test
    void serializesAndDeserializesRecordContracts() {
        ZLinkJsonMessageSerializer serializer = new ZLinkJsonMessageSerializer();

        ProfileReply reply =
                serializer.deserialize(
                        serializer.serialize(new ProfileReply("profile:42")), ProfileReply.class);

        assertEquals(new ProfileReply("profile:42"), reply);
    }

    @Test
    void enumFieldsUseNamesAndRejectOrdinals() {
        ZLinkJsonMessageSerializer serializer = new ZLinkJsonMessageSerializer();
        EnumPayload expected = new EnumPayload(ProfileReportReason.profile);

        ZLinkEncodedPayload encoded = serializer.serialize(expected);
        assertEquals(
                "{\"reasonId\":\"profile\"}", new String(encoded.bytes(), StandardCharsets.UTF_8));
        assertEquals(expected, serializer.deserialize(encoded, EnumPayload.class));

        for (String json : List.of("{\"reasonId\":1}", "{\"reasonId\":\"1\"}")) {
            assertThrows(
                    IllegalArgumentException.class,
                    () ->
                            serializer.deserialize(
                                    ZLinkEncodedPayload.from(json.getBytes(StandardCharsets.UTF_8)),
                                    EnumPayload.class),
                    json);
        }
    }

    @Test
    void preservesNullRecordFields() {
        ZLinkJsonMessageSerializer serializer = new ZLinkJsonMessageSerializer();

        CourierActorFound reply =
                serializer.deserialize(
                        serializer.serialize(new CourierActorFound("courier-a", null)),
                        CourierActorFound.class);

        assertEquals("courier-a", reply.courierId());
        assertNull(reply.actor());
    }

    @Test
    void preservesFrameworkActorReferences() {
        ZLinkJsonMessageSerializer serializer = new ZLinkJsonMessageSerializer();
        ActorRefSnapshot expected =
                new ActorRefSnapshot(
                        "courier-a", 7L, "game", RoutingId.from(new byte[] {0, 65, 66}));

        ActorRefSnapshot actual =
                serializer.deserialize(serializer.serialize(expected), ActorRefSnapshot.class);

        assertEquals(expected, actual);
    }

    @Test
    void actorRefUsesTheExactTypedJsonContract() {
        ZLinkJsonMessageSerializer serializer = new ZLinkJsonMessageSerializer();
        ActorRef expected =
                new ActorRef("courier-a", 7, "game", RoutingId.from(new byte[] {0, 65, 66}));

        ZLinkEncodedPayload encoded = serializer.serialize(expected);
        assertEquals(
                "{\"actorId\":\"courier-a\",\"objectGeneration\":\"7\","
                        + "\"meshName\":\"game\",\"nodeRid\":\"004142\"}",
                new String(encoded.bytes(), StandardCharsets.UTF_8));
        assertEquals(expected, serializer.deserialize(encoded, ActorRef.class));

        assertThrows(
                IllegalArgumentException.class,
                () ->
                        serializer.deserialize(
                                ZLinkEncodedPayload.from(
                                        ("{\"actorId\":\"courier-a\",\"objectGeneration\":7,"
                                                        + "\"meshName\":\"game\",\"nodeRid\":\"004142\"}")
                                                .getBytes(StandardCharsets.UTF_8)),
                                ActorRef.class));
        assertThrows(
                IllegalArgumentException.class,
                () ->
                        serializer.deserialize(
                                ZLinkEncodedPayload.from(
                                        ("{\"actorId\":\"courier-a\",\"actorId\":\"duplicate\","
                                                        + "\"objectGeneration\":\"7\",\"meshName\":\"game\","
                                                        + "\"nodeRid\":\"004142\"}")
                                                .getBytes(StandardCharsets.UTF_8)),
                                ActorRef.class));
        assertThrows(
                IllegalArgumentException.class,
                () ->
                        serializer.deserialize(
                                ZLinkEncodedPayload.from(
                                        ("{\"actorId\":\"courier-a\",\"objectGeneration\":\"7\","
                                                        + "\"meshName\":\"game\",\"nodeRid\":\"004142\","
                                                        + "\"unknown\":true}")
                                                .getBytes(StandardCharsets.UTF_8)),
                                ActorRef.class));
    }

    @Test
    void spotRefUsesTheExactTypedJsonContract() {
        ZLinkJsonMessageSerializer serializer = new ZLinkJsonMessageSerializer();
        SpotRef expected =
                new SpotRef("room-a", 11, "game", RoutingId.from(new byte[] {0, 65, 66}));

        ZLinkEncodedPayload encoded = serializer.serialize(expected);
        assertEquals(
                "{\"spotId\":\"room-a\",\"objectGeneration\":\"11\","
                        + "\"meshName\":\"game\",\"nodeRid\":\"004142\"}",
                new String(encoded.bytes(), StandardCharsets.UTF_8));
        assertEquals(expected, serializer.deserialize(encoded, SpotRef.class));

        assertThrows(
                IllegalArgumentException.class,
                () ->
                        serializer.deserialize(
                                ZLinkEncodedPayload.from(
                                        ("{\"spotId\":\"room-a\",\"objectGeneration\":\"11\","
                                                        + "\"meshName\":\"game\",\"nodeRid\":\"004142\","
                                                        + "\"unknown\":true}")
                                                .getBytes(StandardCharsets.UTF_8)),
                                SpotRef.class));
    }

    record ProfileReply(String value) {}

    record EnumPayload(ProfileReportReason reasonId) {}

    record CourierActorFound(String courierId, ActorRefWire actor) {}

    record ActorRefWire(String nodeRid, String actorId, long generation) {}

    record GoldenFixture(
            String format,
            List<String> consumers,
            Map<String, Object> contract,
            List<GoldenCase> valid,
            List<GoldenCase> invalid) {}

    record GoldenCase(String name, String jsonUtf8, String reason) {}

    record GoldenValue(
            long signed64,
            String unsigned64,
            int int32,
            double ratio,
            GoldenState state,
            byte[] bytes,
            Object nullable,
            Map<String, Integer> labels) {}

    enum GoldenState {
        Ready,
        Closed
    }

    enum ProfileReportReason {
        nickname,
        profile,
        harassment,
        other
    }
}
