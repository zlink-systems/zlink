package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.Arrays;
import java.util.HexFormat;
import java.util.zip.CRC32C;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;

final class ZLinkActorAuthorityPayloadCodecTest {
    @Test
    void actorAuthorityMatchesTheCrossLanguageByteVector() {
        var codec = new ZLinkActorAuthorityPayloadCodec();
        byte[] encoded = codec.encode(
            ZLinkActorAuthorityPayloadCodec.State.READY,
            "A",
            "B",
            "C",
            2,
            1,
            "D",
            3,
            "E",
            RoutingId.from("F"),
            4);

        assertArrayEquals(HexFormat.of().parseHex(
            "5a4c4155010000000000340001001001410142010143"
                + "0000000000000002010144000000000000000301450146"
                + "000000000000000400000000000000000000b2374797"),
            encoded);
        var decoded = codec.decode(encoded).orElseThrow();
        assertEquals(ZLinkActorAuthorityPayloadCodec.State.READY,
            decoded.state());
        assertEquals("A", decoded.stableType());
        assertEquals("B", decoded.actorId());
        assertEquals("C", decoded.currentSpotId());
        assertEquals(2, decoded.currentSpotGeneration());
        assertEquals(1, decoded.currentSpotKind());
        assertEquals("D", decoded.ownerId());
        assertEquals(3, decoded.ownerLeaseGeneration());
        assertEquals("E", decoded.meshName());
        assertEquals(RoutingId.from("F"), decoded.nodeRid());
        assertEquals(4, decoded.nodeGeneration());
    }

    @Test
    void opaqueNodeGenerationAcceptsTheUnsignedHighBitFixedVector() {
        var codec = new ZLinkActorAuthorityPayloadCodec();
        byte[] encoded = codec.encode(
            ZLinkActorAuthorityPayloadCodec.State.READY,
            "A", "B", "C", 2, 1, "D", 3, "E",
            RoutingId.from("F"), Long.MIN_VALUE);

        assertArrayEquals(HexFormat.of().parseHex("8000000000000000"),
            Arrays.copyOfRange(encoded, 45, 53));
        assertEquals(Long.MIN_VALUE,
            codec.decode(encoded).orElseThrow().nodeGeneration());
    }

    @Test
    void opaqueCurrentSpotGenerationAcceptsTheUnsignedHighBitFixedVector() {
        var codec = new ZLinkActorAuthorityPayloadCodec();
        long generation = 0xa70186055079275aL;
        byte[] encoded = codec.encode(
            ZLinkActorAuthorityPayloadCodec.State.READY,
            "A", "B", "C", generation, 1, "D", 3, "E",
            RoutingId.from("F"), 4);

        assertArrayEquals(HexFormat.of().parseHex("a70186055079275a"),
            Arrays.copyOfRange(encoded, 22, 30));
        assertEquals(generation,
            codec.decode(encoded).orElseThrow().currentSpotGeneration());
    }

    @Test
    void opaqueNodeGenerationRejectsOnlyZero() {
        var codec = new ZLinkActorAuthorityPayloadCodec();
        assertThrows(IllegalArgumentException.class, () -> codec.encode(
            ZLinkActorAuthorityPayloadCodec.State.READY,
            "A", "B", "C", 2, 1, "D", 3, "E",
            RoutingId.from("F"), 0));

        byte[] zeroNodeGeneration = codec.encode(
            ZLinkActorAuthorityPayloadCodec.State.READY,
            "A", "B", "C", 2, 1, "D", 3, "E",
            RoutingId.from("F"), 1);
        Arrays.fill(zeroNodeGeneration, 45, 53, (byte) 0);
        updateChecksum(zeroNodeGeneration);
        assertTrue(codec.decode(zeroNodeGeneration).isEmpty());
    }

    @Test
    void boundedGenerationsRetainTheirSignedPositiveValidation() {
        var codec = new ZLinkActorAuthorityPayloadCodec();
        assertThrows(IllegalArgumentException.class, () -> codec.encode(
            ZLinkActorAuthorityPayloadCodec.State.READY,
            "A", "B", "C", 2, 1, "D", Long.MIN_VALUE, "E",
            RoutingId.from("F"), 1));
    }

    private static void updateChecksum(byte[] payload) {
        var checksum = new CRC32C();
        int checksumOffset = payload.length - Integer.BYTES;
        checksum.update(payload, 0, checksumOffset);
        long value = checksum.getValue();
        for (int shift = 24; shift >= 0; shift -= 8) {
            payload[checksumOffset++] = (byte) (value >>> shift);
        }
    }
}
