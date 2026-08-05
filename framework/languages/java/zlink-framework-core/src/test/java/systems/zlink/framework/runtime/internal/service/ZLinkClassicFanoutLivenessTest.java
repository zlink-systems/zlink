package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.List;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;

final class ZLinkClassicFanoutLivenessTest {
    @Test
    void exactBeaconIsInternalAndSamePrefixTopicRemainsApplicationData() {
        var liveness = new ZLinkClassicFanoutLiveness(0);
        RoutingId publisher = RoutingId.from("fanout-a");
        liveness.connect(publisher, "pipe-a", 0);
        assertFalse(liveness.isReady(publisher));

        List<byte[]> beacon = ZLinkClassicFanoutLiveness.beaconRecord();
        assertArrayEquals(
            new byte[] {0x01, 0x5a, 0x4c, 0x46, 0x31},
            beacon.get(0));
        assertArrayEquals(
            new byte[] {0x5a, 0x46, 0x01, 0x01},
            beacon.get(1));
        assertEquals(
            ZLinkClassicFanoutLiveness.ReceiveKind.BEACON,
            liveness.receive(publisher, "pipe-a", beacon, 1));
        assertTrue(liveness.isReady(publisher));

        byte[] samePrefix = new byte[] {
            0x01, 0x5a, 0x4c, 0x46, 0x31, 0x00};
        assertFalse(ZLinkClassicFanoutLiveness.isReservedTopic(samePrefix));
        assertEquals(
            ZLinkClassicFanoutLiveness.ReceiveKind.APPLICATION,
            liveness.receive(
                publisher,
                "pipe-a",
                List.of(samePrefix, new byte[] {9}),
                2));
    }

    @Test
    void malformedReservedRecordImmediatelyRemovesOnlyItsPublisher() {
        var liveness = new ZLinkClassicFanoutLiveness(0);
        RoutingId malformed = RoutingId.from("fanout-a");
        RoutingId healthy = RoutingId.from("fanout-b");
        liveness.connect(malformed, "pipe-a", 0);
        liveness.connect(healthy, "pipe-b", 0);
        liveness.receive(
            healthy,
            "pipe-b",
            List.of(new byte[] {1}, new byte[] {2}),
            1);

        assertThrows(
            ZLinkServiceWireException.class,
            () -> liveness.receive(
                malformed,
                "pipe-a",
                List.of(
                    ZLinkClassicFanoutLiveness.reservedTopic(),
                    new byte[] {0}),
                2));
        assertFalse(liveness.isReady(malformed));
        assertTrue(liveness.isReady(healthy));
        assertEquals(1, liveness.size());

        liveness.connect(malformed, "pipe-a2", 3);
        assertThrows(
            ZLinkServiceWireException.class,
            () -> liveness.receive(
                malformed,
                "pipe-a2",
                List.of(
                    ZLinkClassicFanoutLiveness.reservedTopic(),
                    new byte[] {0x5a, 0x46, 0x01, 0x01},
                    new byte[] {0}),
                4));
        assertFalse(liveness.isReady(malformed));
        assertTrue(liveness.isReady(healthy));
    }

    @Test
    void beaconUsesFiveSecondCadenceAndReceiveUsesFifteenSecondDeadline() {
        var liveness = new ZLinkClassicFanoutLiveness(
            Duration.ofSeconds(5),
            Duration.ofSeconds(15),
            0);
        RoutingId publisher = RoutingId.from("fanout-a");
        liveness.connect(publisher, "pipe-a", 0);

        assertFalse(liveness.beaconDue(
            Duration.ofSeconds(5).toNanos() - 1));
        assertTrue(liveness.beaconDue(
            Duration.ofSeconds(5).toNanos()));
        assertFalse(liveness.beaconDue(
            Duration.ofSeconds(10).toNanos() - 1));
        assertTrue(liveness.beaconDue(
            Duration.ofSeconds(10).toNanos()));

        liveness.receive(
            publisher,
            "pipe-a",
            ZLinkClassicFanoutLiveness.beaconRecord(),
            Duration.ofSeconds(4).toNanos());
        assertTrue(liveness.expire(
            Duration.ofSeconds(19).toNanos() - 1).isEmpty());
        assertEquals(
            List.of(publisher),
            liveness.expire(Duration.ofSeconds(19).toNanos()));
        assertFalse(liveness.isReady(publisher));
    }

    @Test
    void staleConnectionCannotRefreshReplacementConnection() {
        var liveness = new ZLinkClassicFanoutLiveness(0);
        RoutingId publisher = RoutingId.from("fanout-a");
        liveness.connect(publisher, "old", 0);
        liveness.connect(publisher, "current", 1);

        assertEquals(
            ZLinkClassicFanoutLiveness.ReceiveKind.STALE_CONNECTION,
            liveness.receive(
                publisher,
                "old",
                ZLinkClassicFanoutLiveness.beaconRecord(),
                2));
        assertFalse(liveness.isReady(publisher));
        assertEquals(
            ZLinkClassicFanoutLiveness.ReceiveKind.BEACON,
            liveness.receive(
                publisher,
                "current",
                ZLinkClassicFanoutLiveness.beaconRecord(),
                3));
        assertTrue(liveness.isReady(publisher));
    }
}
