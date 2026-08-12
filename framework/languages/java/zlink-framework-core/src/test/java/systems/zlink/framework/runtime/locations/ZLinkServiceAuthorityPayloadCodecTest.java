package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;

import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;

final class ZLinkServiceAuthorityPayloadCodecTest {
    @Test
    void readyUserSpotRoundTripsIntoDurableRouteFields() {
        var codec = new ZLinkServiceAuthorityPayloadCodec();
        String spotId = "spot-17";
        RoutingId nodeRid = RoutingId.from("node-b");

        byte[] payload = codec.encodeUser(
            ZLinkServiceAuthorityPayloadCodec.State.READY,
            "game.player",
            spotId,
            "owner-b",
            31,
            "game",
            nodeRid,
            17);

        var decoded = codec.decode(payload).orElseThrow();
        assertInstanceOf(
            ZLinkServiceAuthorityPayloadCodec.UserSpotAuthority.class,
            decoded);
        assertTrue(decoded.user().isPresent());
        assertTrue(decoded.instance().isEmpty());
        assertEquals(ZLinkServiceAuthorityPayloadCodec.State.READY, decoded.state());
        assertEquals("game.player", decoded.stableType());
        assertEquals(spotId, decoded.spotId());
        assertEquals("owner-b", decoded.ownerId());
        assertEquals(31, decoded.ownerLeaseGeneration());
        assertEquals("game", decoded.meshName());
        assertEquals(nodeRid, decoded.nodeRid());
        assertEquals(17, decoded.nodeGeneration());
    }

    @Test
    void closingInstanceSpotRoundTripsAsAClosingAuthority() {
        var codec = new ZLinkServiceAuthorityPayloadCodec();

        var decoded = codec.decode(codec.encodeInstance(
            ZLinkServiceAuthorityPayloadCodec.State.CLOSING,
            "game.room",
            "room-17",
            "owner-b",
            31,
            "game",
            RoutingId.from("node-b"),
            17)).orElseThrow();

        assertInstanceOf(
            ZLinkServiceAuthorityPayloadCodec.InstanceSpotAuthority.class,
            decoded);
        assertTrue(decoded.instance().isPresent());
        assertTrue(decoded.user().isEmpty());
        assertEquals(
            ZLinkServiceAuthorityPayloadCodec.State.CLOSING,
            decoded.state());
        assertEquals("room-17", decoded.spotId());
    }

    @Test
    void spotIdUsesUtf8TextRatherThanTransportRoutingIdentity() {
        var codec = new ZLinkServiceAuthorityPayloadCodec();
        String spotId = "room/서울:alpha";

        var decoded = codec.decode(codec.encodeUser(
            ZLinkServiceAuthorityPayloadCodec.State.READY,
            "game.room",
            spotId,
            "owner-b",
            31,
            "game",
            RoutingId.from("node-b"),
            17)).orElseThrow();

        assertEquals(spotId, decoded.spotId());
    }

    @Test
    void corruptedAuthorityNeverEntersDurableRouteCache() {
        var codec = new ZLinkServiceAuthorityPayloadCodec();
        byte[] payload = codec.encodeUser(
            ZLinkServiceAuthorityPayloadCodec.State.CREATING,
            "game.player",
            "spot-17",
            "owner-b",
            31,
            "game",
            RoutingId.from("node-b"),
            17);
        payload[payload.length - 1] ^= 1;

        assertTrue(codec.decode(payload).isEmpty());
    }

    @Test
    void spotAuthorityKeyUsesCanonicalLengthAndEscaping() {
        assertEquals(
            "zla1:s:3:a%3Ab",
            ZLinkAuthorityKeyCodec.spot("a:b"));
    }

    @Test
    void spotIdUsesExactUtf8IdentityAndEnforcesByteBoundary() {
        assertNotEquals(
            ZLinkAuthorityKeyCodec.spot("Room"),
            ZLinkAuthorityKeyCodec.spot("room"));
        assertNotEquals(
            ZLinkAuthorityKeyCodec.spot("\u00e9"),
            ZLinkAuthorityKeyCodec.spot("e\u0301"));
        assertEquals(
            "zla1:s:255:" + "a".repeat(255),
            ZLinkAuthorityKeyCodec.spot("a".repeat(255)));
        assertThrows(
            IllegalArgumentException.class,
            () -> ZLinkAuthorityKeyCodec.spot(""));
        assertThrows(
            IllegalArgumentException.class,
            () -> ZLinkAuthorityKeyCodec.spot("a".repeat(256)));
        assertThrows(
            IllegalArgumentException.class,
            () -> ZLinkAuthorityKeyCodec.spot("\u0000"));
    }

    @Test
    void everyLifecycleStateDecodesIntoTheSemanticSpotVariant() {
        var codec = new ZLinkServiceAuthorityPayloadCodec();
        for (ZLinkServiceAuthorityPayloadCodec.State state
            : ZLinkServiceAuthorityPayloadCodec.State.values()) {
            var user = codec.decode(codec.encodeUser(
                state, "game.user", "user", "owner", 1,
                "mesh", RoutingId.from("node"), 2)).orElseThrow();
            var instance = codec.decode(codec.encodeInstance(
                state, "game.instance", "instance", "owner", 1,
                "mesh", RoutingId.from("node"), 2)).orElseThrow();

            assertEquals(state, user.state());
            assertTrue(user.user().isPresent());
            assertTrue(user.instance().isEmpty());
            assertEquals(state, instance.state());
            assertTrue(instance.instance().isPresent());
            assertTrue(instance.user().isEmpty());
        }
    }
}
