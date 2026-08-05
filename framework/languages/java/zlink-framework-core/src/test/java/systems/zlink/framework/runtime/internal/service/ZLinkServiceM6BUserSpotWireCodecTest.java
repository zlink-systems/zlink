package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.Arrays;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;

final class ZLinkServiceM6BUserSpotWireCodecTest {
    private final ZLinkServiceM6BWireCodec codec =
        new ZLinkServiceM6BWireCodec();

    @Test
    void command47RoundTripsEveryReservationAndLifecycleFence() {
        var command = new ZLinkServiceM6BWireCodec.UserSpotCreate(
            11,
            12,
            13,
            RoutingId.from("source"),
            14,
            "room",
            "room-v1",
            reservation(),
            1_900_000_000_000L);

        byte[] encoded = codec.encodeUserSpotCreateHeader(command);

        assertEquals(command, codec.decodeUserSpotCreateHeader(encoded));
        assertArrayEquals(
            encoded,
            codec.encodeUserSpotCreateHeader(
                codec.decodeUserSpotCreateHeader(encoded)));
    }

    @Test
    void command48RoundTripsExactSpotAndStoreFence() {
        var command = new ZLinkServiceM6BWireCodec.UserSpotClose(
            21,
            22,
            23,
            RoutingId.from("source"),
            24,
            new ZLinkServiceM6BWireCodec.UserSpotCloseFence(
                "room",
                25,
                RoutingId.from("target"),
                26,
                27,
                "store-28"),
            1_900_000_000_100L);

        byte[] encoded = codec.encodeUserSpotCloseHeader(command);

        assertEquals(command, codec.decodeUserSpotCloseHeader(encoded));
        assertArrayEquals(
            encoded,
            codec.encodeUserSpotCloseHeader(
                codec.decodeUserSpotCloseHeader(encoded)));
    }

    @Test
    void userSpotReplyTailIsPresentOnlyForSuccessfulMatchingOperation() {
        var created = new ZLinkServiceM6BWireCodec.UserSpotCreateTerminal(
            ZLinkServiceM6BWireCodec.UserSpotCreateResult.CREATED,
            "room",
            31);
        var createReply = codec.decodeUserSpotCreateReply(
            codec.encodeUserSpotCreateReply(30, 0, 0, created));
        var closeTrue = codec.decodeUserSpotCloseReply(
            codec.encodeUserSpotCloseReply(32, 0, 0, true));
        var closeFalse = codec.decodeUserSpotCloseReply(
            codec.encodeUserSpotCloseReply(33, 0, 0, false));
        var failed = codec.decodeUserSpotCreateReply(
            codec.encodeUserSpotCreateReply(34, 107, 33, null));

        assertEquals(created, createReply.success());
        assertTrue(closeTrue.closed());
        assertFalse(closeFalse.closed());
        assertEquals(107, failed.terminalResult());
        assertEquals(33, failed.failureCode());
        assertEquals(null, failed.success());
        assertThrows(
            ZLinkServiceWireException.class,
            () -> codec.encodeUserSpotCreateReply(1, 0, 0, null));
        assertThrows(
            ZLinkServiceWireException.class,
            () -> codec.encodeUserSpotCloseReply(1, 107, 33, true));
    }

    @Test
    void malformedUserSpotCommandsAreRejectedBeforeDispatch() {
        byte[] create = codec.encodeUserSpotCreateHeader(
            new ZLinkServiceM6BWireCodec.UserSpotCreate(
                1,
                2,
                3,
                RoutingId.from("source"),
                4,
                "room",
                "room-v1",
                reservation(),
                5));
        byte[] close = codec.encodeUserSpotCloseHeader(
            new ZLinkServiceM6BWireCodec.UserSpotClose(
                1,
                2,
                3,
                RoutingId.from("source"),
                4,
                new ZLinkServiceM6BWireCodec.UserSpotCloseFence(
                    "room",
                    5,
                    RoutingId.from("target"),
                    6,
                    7,
                    "store"),
                8));

        assertThrows(
            ZLinkServiceWireException.class,
            () -> codec.decodeUserSpotCreateHeader(
                Arrays.copyOf(create, create.length - 1)));
        assertThrows(
            ZLinkServiceWireException.class,
            () -> codec.decodeUserSpotCloseHeader(
                Arrays.copyOf(close, close.length - 1)));

        byte[] wrongVersion = close.clone();
        int versionOffset = 5 + 8 + 8 + 8
            + 1 + "source".length() + 8;
        wrongVersion[versionOffset] = 2;
        assertThrows(
            ZLinkServiceWireException.class,
            () -> codec.decodeUserSpotCloseHeader(wrongVersion));
    }

    private static ZLinkServiceM6BWireCodec.ReservationFence
        reservation() {
        return new ZLinkServiceM6BWireCodec.ReservationFence(
            "reservation",
            "store-version",
            15,
            16,
            RoutingId.from("target"),
            17,
            "owner",
            18,
            1);
    }
}
