package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.util.HexFormat;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;

final class ZLinkServiceRelocationWireCodecTest {
    private static final String RELAY_GOLDEN =
        "5a4d012100000000000000000100000000000000020000000000000003020054"
        + "0000000000000004000000000000000500000000000000060b636f6f7264696e"
        + "61746f720000000000000007066e6f64652d61000000000000000b000773746f"
        + "72652d33000000000000000800000000000000090000006500000000";
    private static final String ACK_GOLDEN =
        "5a4d012e00000000000000000400000000000000050b636f6f7264696e61746f"
        + "720000000000000007066e6f64652d61000000000000000b000773746f72652d"
        + "3300000000000000010000000000000002000000000000000306736f75726365000000000000000d"
        + "066e6f64652d73000000000000001102";

    @Test
    void command33MatchesSharedGoldenByteForByte() {
        var codec = new ZLinkServiceRelocationWireCodec();
        var coordinator = new ZLinkServiceRelocationWireCodec.CoordinatorFence(
            "coordinator", 7, RoutingId.from("node-a"), 11, "store-3");
        var expected = new ZLinkServiceRelocationWireCodec.ReplyRelay(
            new ZLinkServiceRelocationWireCodec.Operation(1, 2),
            3,
            new ZLinkServiceRelocationWireCodec.RelocationId(4, 5),
            6,
            coordinator,
            8,
            9,
            101,
            0);

        byte[] encoded = codec.encodeReplyRelay(expected);

        assertArrayEquals(HexFormat.of().parseHex(RELAY_GOLDEN), encoded);
        assertEquals(expected, codec.decodeReplyRelay(encoded));
        assertThrows(IllegalArgumentException.class,
            () -> codec.decodeReplyRelay(
                java.util.Arrays.copyOf(encoded, encoded.length - 1)));
        assertThrows(IllegalArgumentException.class,
            () -> codec.encodeReplyRelay(
                new ZLinkServiceRelocationWireCodec.ReplyRelay(
                    expected.operation(), expected.replyRouteId(),
                    expected.relocation(), expected.targetAttemptGeneration(),
                    expected.coordinator(), expected.participantId(),
                    expected.sequence(), 102, 2)));
    }

    @Test
    void command46MatchesSharedGoldenAndRejectsOpenStatus() {
        var codec = new ZLinkServiceRelocationWireCodec();
        var coordinator = new ZLinkServiceRelocationWireCodec.CoordinatorFence(
            "coordinator", 7, RoutingId.from("node-a"), 11, "store-3");
        var expected = new ZLinkServiceRelocationWireCodec.ReplyRelayAck(
            new ZLinkServiceRelocationWireCodec.RelocationId(4, 5),
            coordinator,
            new ZLinkServiceRelocationWireCodec.Operation(1, 2),
            3,
            new ZLinkServiceRelocationWireCodec.RequestSourceFence(
                "source", 13, RoutingId.from("node-s"), 17),
            2);

        byte[] encoded = codec.encodeReplyRelayAck(expected);

        assertArrayEquals(HexFormat.of().parseHex(ACK_GOLDEN), encoded);
        assertEquals(expected, codec.decodeReplyRelayAck(encoded));
        assertThrows(IllegalArgumentException.class,
            () -> codec.encodeReplyRelayAck(
                new ZLinkServiceRelocationWireCodec.ReplyRelayAck(
                    expected.relocation(), expected.coordinator(),
                    expected.operation(), expected.replyRouteId(),
                    expected.requestSource(), 0)));
        assertThrows(IllegalArgumentException.class,
            () -> codec.encodeReplyRelayAck(
                new ZLinkServiceRelocationWireCodec.ReplyRelayAck(
                    expected.relocation(), expected.coordinator(),
                    expected.operation(), 0,
                    expected.requestSource(), expected.status())));
    }
}
