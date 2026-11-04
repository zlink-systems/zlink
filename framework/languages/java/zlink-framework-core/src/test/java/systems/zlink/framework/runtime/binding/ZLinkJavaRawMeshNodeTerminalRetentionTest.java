package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceLivenessRegistry;

import java.time.Duration;
import java.util.concurrent.atomic.AtomicLong;

final class ZLinkJavaRawMeshNodeTerminalRetentionTest {
    @ParameterizedTest
    @ValueSource(longs = {0, Long.MAX_VALUE - 1_000_000_000L})
    void terminalRetentionIgnoresWallClockJumpsAndExpiresAfterOriginalDeadline(long startNanos)
            throws Exception {
        var wall = new AtomicLong(1_000_000);
        var monotonic = new AtomicLong(startNanos);
        try (var context = Zlink.createContext();
                var node =
                        new ZLinkJavaRawMeshNode(
                                context,
                                "mesh",
                                wall::get,
                                monotonic::get,
                                ZLinkServiceLivenessRegistry.DEFAULT_PROBE_INTERVAL,
                                ZLinkServiceLivenessRegistry.DEFAULT_PEER_TIMEOUT)) {
            long wireDeadline = wall.get() + 10_000;
            var first = admit(node, 1, new byte[] {1}, wireDeadline);
            assertTrue(first.owner());
            var slot = first.slot();
            complete(slot);

            wall.addAndGet(Duration.ofDays(1).toMillis());
            monotonic.addAndGet(Duration.ofSeconds(1).toNanos());
            var replay = admit(node, 1, new byte[] {1}, wireDeadline);
            assertSame(slot, replay.slot(), "wall jump must not evict a terminal");
            assertFalse(replay.owner());
            assertTrue(admit(node, 1, new byte[] {2}, wireDeadline).fingerprintMismatch());
            assertTrue(
                    admit(node, 2, new byte[] {1}, wireDeadline).expiredNew(),
                    "a new wire deadline is still compared to wall time");

            wall.addAndGet(-Duration.ofDays(2).toMillis());
            monotonic.set(startNanos + Duration.ofSeconds(310).toNanos());
            assertSame(
                    slot,
                    admit(node, 1, new byte[] {1}, wireDeadline).slot(),
                    "retain through the original deadline plus five minutes");
            monotonic.incrementAndGet();
            var replacement = admit(node, 1, new byte[] {1}, wireDeadline);
            assertTrue(replacement.owner(), "backward wall jump must not extend local retention");
            assertNotSame(slot, replacement.slot());

            var pending = replacement.slot();
            monotonic.addAndGet(Duration.ofDays(3).toNanos());
            assertSame(
                    pending,
                    admit(node, 1, new byte[] {1}, wireDeadline).slot(),
                    "retention must not evict an unfinished operation");
        }
    }

    private static ZLinkJavaRawMeshNode.UserSpotTerminalAdmission admit(
            ZLinkJavaRawMeshNode node, long operation, byte[] fingerprint, long deadline) {
        var key =
                new ZLinkJavaRawMeshNode.UserSpotOperationKey(
                        RoutingId.from("source"), 1L, 1L, operation);
        return node.admitUserSpotOperation(key, fingerprint, deadline);
    }

    private static void complete(ZLinkJavaRawMeshNode.UserSpotTerminalSlot slot) {
        slot.terminal.complete(null);
    }
}
