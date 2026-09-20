package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.io.IOException;
import java.io.InputStream;
import java.util.HexFormat;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;

final class ZLinkServiceAuthorityPayloadCodecTest {
    private static final ObjectMapper JSON = new ObjectMapper();

    @Test
    void durableAuthorityGoldenPreservesActivationRecoveryByteExactly()
        throws IOException {
        byte[] encoded = HexFormat.of().parseHex(goldenEncodedHex());
        var codec = new ZLinkServiceAuthorityPayloadCodec();

        var decoded = codec.decode(encoded).orElseThrow();

        var recovery = decoded.activationRecoveryState().orElseThrow();
        assertEquals("activation-1", recovery.reference());
        assertArrayEquals(HexFormat.of().parseHex(
            "d71bdc8539d184b7ea5a91006b49bee290fcd6a5811bb2061a29c5a09ec9399e"),
            recovery.sha256());
        assertEquals(175, recovery.encodedSize());
        assertEquals(1, recovery.inboxSequence());
        assertArrayEquals(encoded, codec.encode(decoded));
    }

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
    void recoveryBearingReadyInstanceDropsRecoveryWhenRewrittenClosing() {
        var codec = new ZLinkServiceAuthorityPayloadCodec();
        var recovery = new ZLinkServiceAuthorityPayloadCodec.ActivationRecoveryState(
            "activation-17",
            HexFormat.of().parseHex(
                "d71bdc8539d184b7ea5a91006b49bee290fcd6a5811bb2061a29c5a09ec9399e"),
            175,
            1);
        var ready = codec.decode(codec.encodeInstance(
            ZLinkServiceAuthorityPayloadCodec.State.READY,
            "game.room",
            "room-17",
            "owner-b",
            31,
            "game",
            RoutingId.from("node-b"),
            17,
            java.util.Optional.of(recovery))).orElseThrow();

        byte[] closing = codec.encodeInstance(
            ZLinkServiceAuthorityPayloadCodec.State.CLOSING,
            ready.stableType(),
            ready.spotId(),
            ready.ownerId(),
            ready.ownerLeaseGeneration(),
            ready.meshName(),
            ready.nodeRid(),
            ready.nodeGeneration());

        var rewritten = codec.decode(closing).orElseThrow();
        assertEquals(ZLinkServiceAuthorityPayloadCodec.State.CLOSING,
            rewritten.state());
        assertTrue(rewritten.activationRecoveryState().isEmpty());
    }

    @Test
    void recoveryBearingClosingInstanceIsRejected() {
        var codec = new ZLinkServiceAuthorityPayloadCodec();
        var recovery = new ZLinkServiceAuthorityPayloadCodec.ActivationRecoveryState(
            "activation-17",
            HexFormat.of().parseHex(
                "d71bdc8539d184b7ea5a91006b49bee290fcd6a5811bb2061a29c5a09ec9399e"),
            175,
            1);

        assertThrows(IllegalArgumentException.class, () -> codec.encodeInstance(
            ZLinkServiceAuthorityPayloadCodec.State.CLOSING,
            "game.room",
            "room-17",
            "owner-b",
            31,
            "game",
            RoutingId.from("node-b"),
            17,
            java.util.Optional.of(recovery)));

        byte[] closingWithRecovery = codec.encodeInstance(
            ZLinkServiceAuthorityPayloadCodec.State.READY,
            "game.room",
            "room-17",
            "owner-b",
            31,
            "game",
            RoutingId.from("node-b"),
            17,
            java.util.Optional.of(recovery));
        closingWithRecovery[11] = 3;
        closingWithRecovery[18] = 3;
        updateChecksum(closingWithRecovery);

        assertTrue(codec.decode(closingWithRecovery).isEmpty());
    }

    private static void updateChecksum(byte[] payload) {
        var checksum = new java.util.zip.CRC32C();
        int checksumOffset = payload.length - Integer.BYTES;
        checksum.update(payload, 0, checksumOffset);
        long value = checksum.getValue();
        for (int shift = 24; shift >= 0; shift -= 8) {
            payload[checksumOffset++] = (byte) (value >>> shift);
        }
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

    private static String goldenEncodedHex() throws IOException {
        try (InputStream resource =
                 ZLinkServiceAuthorityPayloadCodecTest.class.getResourceAsStream(
                     "/durable-authority-v1.json")) {
            if (resource == null) {
                throw new IllegalStateException(
                    "durable authority golden fixture was not found");
            }
            JsonNode fixture = JSON.readTree(resource);
            assertEquals("authority-payload-v1", fixture.path("format").asText(),
                "durable authority golden format");
            assertTrue(fixture.path("consumers").isArray(),
                "durable authority golden consumers must be an array");
            boolean hasJvmConsumer = false;
            for (JsonNode consumer : fixture.path("consumers")) {
                if ("jvm".equals(consumer.asText())) {
                    hasJvmConsumer = true;
                    break;
                }
            }
            assertTrue(hasJvmConsumer,
                "durable authority golden must include the jvm consumer");
            JsonNode encodedHex = fixture.path("encodedHex");
            if (!encodedHex.isTextual()) {
                throw new IllegalStateException(
                    "durable authority golden encodedHex is missing");
            }
            return encodedHex.textValue();
        }
    }
}
