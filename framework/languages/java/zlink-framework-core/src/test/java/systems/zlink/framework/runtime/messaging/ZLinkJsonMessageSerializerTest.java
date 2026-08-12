package systems.zlink.framework.runtime.messaging;
import java.util.stream.IntStream;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.nio.charset.StandardCharsets;
import java.io.InputStream;
import java.util.List;
import java.util.Map;
import com.fasterxml.jackson.databind.ObjectMapper;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ActorRefSnapshot;
import systems.zlink.framework.spots.SpotRef;

final class ZLinkJsonMessageSerializerTest {
    @Test
    void consumesEveryFrameworkJsonV1GoldenCase() throws Exception {
        ObjectMapper fixtureMapper = new ObjectMapper();
        GoldenFixture fixture;
        try (InputStream input = getClass().getResourceAsStream(
            "/framework-json-v1.json")) {
            if (input == null) {
                throw new AssertionError("framework-json-v1 fixture is missing");
            }
            fixture = fixtureMapper.readValue(input, GoldenFixture.class);
        }
        assertEquals("framework-json-v1", fixture.format());

        ZLinkJsonMessageSerializer serializer = new ZLinkJsonMessageSerializer();
        for (GoldenCase valid : fixture.valid()) {
            GoldenValue decoded = serializer.deserialize(
                ZLinkEncodedPayload.from(valid.jsonUtf8().getBytes(StandardCharsets.UTF_8)),
                GoldenValue.class);
            assertEquals(Long.MIN_VALUE, decoded.signed64());
            assertEquals("18446744073709551615", decoded.unsigned64());
            assertEquals(GoldenState.Ready, decoded.state());
            assertEquals(List.of((byte) 1, (byte) 2),
                IntStream.range(0, decoded.bytes().length)
                    .mapToObj(index -> decoded.bytes()[index])
                    .toList());
        }
        for (GoldenCase invalid : fixture.invalid()) {
            assertThrows(IllegalArgumentException.class, () -> serializer.deserialize(
                ZLinkEncodedPayload.from(
                    invalid.jsonUtf8().getBytes(StandardCharsets.UTF_8)),
                GoldenValue.class), invalid.name());
        }
    }

    @Test
    void serializesAndDeserializesRecordContracts() {
        ZLinkJsonMessageSerializer serializer = new ZLinkJsonMessageSerializer();

        ProfileReply reply = serializer.deserialize(
            serializer.serialize(new ProfileReply("profile:42")),
            ProfileReply.class);

        assertEquals(new ProfileReply("profile:42"), reply);
    }

    @Test
    void preservesNullRecordFields() {
        ZLinkJsonMessageSerializer serializer = new ZLinkJsonMessageSerializer();

        CourierActorFound reply = serializer.deserialize(
            serializer.serialize(new CourierActorFound("courier-a", null)),
            CourierActorFound.class);

        assertEquals("courier-a", reply.courierId());
        assertNull(reply.actor());
    }

    @Test
    void preservesFrameworkActorReferences() {
        ZLinkJsonMessageSerializer serializer = new ZLinkJsonMessageSerializer();
        ActorRefSnapshot expected = new ActorRefSnapshot(
            "courier-a",
            7L,
            "game",
            RoutingId.from(new byte[] {0, 65, 66}));

        ActorRefSnapshot actual = serializer.deserialize(
            serializer.serialize(expected),
            ActorRefSnapshot.class);

        assertEquals(expected, actual);
    }

    @Test
    void actorRefUsesTheExactTypedJsonContract() {
        ZLinkJsonMessageSerializer serializer = new ZLinkJsonMessageSerializer();
        ActorRef expected = new ActorRef(
            "courier-a",
            7,
            "game",
            RoutingId.from(new byte[] {0, 65, 66}));

        ZLinkEncodedPayload encoded = serializer.serialize(expected);
        assertEquals(
            "{\"actorId\":\"courier-a\",\"objectGeneration\":\"7\","
                + "\"meshName\":\"game\",\"nodeRid\":\"004142\"}",
            new String(encoded.bytes(), StandardCharsets.UTF_8));
        assertEquals(expected, serializer.deserialize(encoded, ActorRef.class));

        assertThrows(IllegalArgumentException.class, () -> serializer.deserialize(
            ZLinkEncodedPayload.from((
                "{\"actorId\":\"courier-a\",\"objectGeneration\":7,"
                    + "\"meshName\":\"game\",\"nodeRid\":\"004142\"}")
                .getBytes(StandardCharsets.UTF_8)),
            ActorRef.class));
        assertThrows(IllegalArgumentException.class, () -> serializer.deserialize(
            ZLinkEncodedPayload.from((
                "{\"actorId\":\"courier-a\",\"actorId\":\"duplicate\","
                    + "\"objectGeneration\":\"7\",\"meshName\":\"game\","
                    + "\"nodeRid\":\"004142\"}")
                .getBytes(StandardCharsets.UTF_8)),
            ActorRef.class));
        assertThrows(IllegalArgumentException.class, () -> serializer.deserialize(
            ZLinkEncodedPayload.from((
                "{\"actorId\":\"courier-a\",\"objectGeneration\":\"7\","
                    + "\"meshName\":\"game\",\"nodeRid\":\"004142\","
                    + "\"unknown\":true}")
                .getBytes(StandardCharsets.UTF_8)),
            ActorRef.class));
    }

    @Test
    void spotRefUsesTheExactTypedJsonContract() {
        ZLinkJsonMessageSerializer serializer = new ZLinkJsonMessageSerializer();
        SpotRef expected = new SpotRef(
            "room-a",
            11,
            "game",
            RoutingId.from(new byte[] {0, 65, 66}));

        ZLinkEncodedPayload encoded = serializer.serialize(expected);
        assertEquals(
            "{\"spotId\":\"room-a\",\"objectGeneration\":\"11\","
                + "\"meshName\":\"game\",\"nodeRid\":\"004142\"}",
            new String(encoded.bytes(), StandardCharsets.UTF_8));
        assertEquals(expected, serializer.deserialize(encoded, SpotRef.class));

        assertThrows(IllegalArgumentException.class, () -> serializer.deserialize(
            ZLinkEncodedPayload.from((
                "{\"spotId\":\"room-a\",\"objectGeneration\":\"11\","
                    + "\"meshName\":\"game\",\"nodeRid\":\"004142\","
                    + "\"unknown\":true}")
                .getBytes(StandardCharsets.UTF_8)),
            SpotRef.class));
    }

    record ProfileReply(String value) {
    }

    record CourierActorFound(String courierId, ActorRefWire actor) {
    }

    record ActorRefWire(String nodeRid, String actorId, long generation) {
    }

    record GoldenFixture(
        String format,
        List<String> consumers,
        Map<String, Object> contract,
        List<GoldenCase> valid,
        List<GoldenCase> invalid) {
    }

    record GoldenCase(String name, String jsonUtf8, String reason) {
    }

    record GoldenValue(
        long signed64,
        String unsigned64,
        int int32,
        double ratio,
        GoldenState state,
        byte[] bytes,
        Object nullable,
        Map<String, Integer> labels) {
    }

    enum GoldenState {
        Ready,
        Closed
    }
}
