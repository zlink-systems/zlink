package systems.zlink.framework.runtime.messaging;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.nio.charset.StandardCharsets;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ActorRefSnapshot;
import systems.zlink.framework.spots.SpotRef;

final class ZLinkJsonMessageSerializerTest {
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
}
