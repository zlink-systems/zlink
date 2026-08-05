package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.util.Arrays;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;

final class ZLinkServiceMessageFollowWireCodecTest {
    private final ZLinkServiceMessageFollowWireCodec codec =
        new ZLinkServiceMessageFollowWireCodec();

    @Test
    void actorNoticeRoundTripsWithExactVersionedLength() {
        var source = new ZLinkServiceMessageFollowWireCodec.ActorRoute(
            "actor",
            11,
            RoutingId.from("source-node"),
            12,
            13,
            14);
        var target = new ZLinkServiceMessageFollowWireCodec.ActorRoute(
            "actor",
            11,
            RoutingId.from("target-node"),
            15,
            16,
            17);
        var notice = new ZLinkServiceMessageFollowWireCodec.Notice(
            source, target, 1, 3, 4096, 21, 22, 23);

        byte[] encoded = codec.encode(notice);
        assertEquals(0x5a, Byte.toUnsignedInt(encoded[0]));
        assertEquals(50, Byte.toUnsignedInt(encoded[3]));
        assertEquals(1, Byte.toUnsignedInt(encoded[5]));
        int bodyLength = ((encoded[6] & 0xff) << 24)
            | ((encoded[7] & 0xff) << 16)
            | ((encoded[8] & 0xff) << 8)
            | (encoded[9] & 0xff);
        assertEquals(encoded.length - 10, bodyLength);
        assertEquals(notice, codec.decode(encoded));
        assertArrayEquals(encoded, codec.encode(codec.decode(encoded)));
    }

    @Test
    void spotNoticeRoundTripsAndRejectsMixedRouteKinds() {
        var spot = new ZLinkServiceMessageFollowWireCodec.SpotRoute(
            "spot", 3, RoutingId.from("node"), 4, 5, 6);
        var notice = new ZLinkServiceMessageFollowWireCodec.Notice(
            spot, spot, 8, 1024, 16L * 1024L * 1024L, 1, 0, 0);
        assertEquals(notice, codec.decode(codec.encode(notice)));

        var actor = new ZLinkServiceMessageFollowWireCodec.ActorRoute(
            "actor", 3, RoutingId.from("node"), 4, 5, 6);
        assertThrows(
            ZLinkServiceWireException.class,
            () -> new ZLinkServiceMessageFollowWireCodec.Notice(
                actor, spot, 1, 0, 0, 1, 0, 0));
    }

    @Test
    void rejectsWrongVersionLengthTrailingBytesAndZeroOperation() {
        var route = new ZLinkServiceMessageFollowWireCodec.SpotRoute(
            "spot", 3, RoutingId.from("node"), 4, 5, 6);
        byte[] encoded = codec.encode(
            new ZLinkServiceMessageFollowWireCodec.Notice(
                route, route, 1, 0, 0, 7, 8, 0));

        byte[] wrongVersion = encoded.clone();
        wrongVersion[5] = 2;
        assertThrows(
            ZLinkServiceWireException.class,
            () -> codec.decode(wrongVersion));

        byte[] trailing = Arrays.copyOf(encoded, encoded.length + 1);
        assertThrows(
            ZLinkServiceWireException.class,
            () -> codec.decode(trailing));

        byte[] shortBody = encoded.clone();
        shortBody[9]--;
        assertThrows(
            ZLinkServiceWireException.class,
            () -> codec.decode(shortBody));

        assertThrows(
            ZLinkServiceWireException.class,
            () -> new ZLinkServiceMessageFollowWireCodec.Notice(
                route, route, 1, 0, 0, 0, 0, 0));
    }
}
