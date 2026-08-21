package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;

import java.util.HexFormat;
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
}
