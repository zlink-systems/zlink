package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import java.lang.reflect.Method;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.atomic.AtomicLong;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceLivenessRegistry;

final class ZLinkJavaRawMeshNodeTerminalRetentionTest {
    @ParameterizedTest
    @ValueSource(longs = {0, Long.MAX_VALUE - 1_000_000_000L})
    void terminalRetentionIgnoresWallClockJumpsAndExpiresAfterOriginalDeadline(long startNanos)
        throws Exception {
        var wall = new AtomicLong(1_000_000);
        var monotonic = new AtomicLong(startNanos);
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh", wall::get, monotonic::get,
                 ZLinkServiceLivenessRegistry.DEFAULT_PROBE_INTERVAL,
                 ZLinkServiceLivenessRegistry.DEFAULT_PEER_TIMEOUT)) {
            long wireDeadline = wall.get() + 10_000;
            Object first = admit(node, 1, new byte[] {1}, wireDeadline);
            assertTrue((boolean) property(first, "owner"));
            Object slot = property(first, "slot");
            complete(slot);

            wall.addAndGet(Duration.ofDays(1).toMillis());
            monotonic.addAndGet(Duration.ofSeconds(1).toNanos());
            Object replay = admit(node, 1, new byte[] {1}, wireDeadline);
            assertSame(slot, property(replay, "slot"), "wall jump must not evict a terminal");
            assertFalse((boolean) property(replay, "owner"));
            assertTrue((boolean) property(admit(node, 1, new byte[] {2}, wireDeadline),
                "fingerprintMismatch"));
            assertTrue((boolean) property(admit(node, 2, new byte[] {1}, wireDeadline),
                "expiredNew"), "a new wire deadline is still compared to wall time");

            wall.addAndGet(-Duration.ofDays(2).toMillis());
            monotonic.set(startNanos + Duration.ofSeconds(310).toNanos());
            assertSame(slot, property(admit(node, 1, new byte[] {1}, wireDeadline), "slot"),
                "retain through the original deadline plus five minutes");
            monotonic.incrementAndGet();
            Object replacement = admit(node, 1, new byte[] {1}, wireDeadline);
            assertTrue((boolean) property(replacement, "owner"),
                "backward wall jump must not extend local retention");
            assertNotSame(slot, property(replacement, "slot"));

            Object pending = property(replacement, "slot");
            monotonic.addAndGet(Duration.ofDays(3).toNanos());
            assertSame(pending, property(admit(node, 1, new byte[] {1}, wireDeadline), "slot"),
                "retention must not evict an unfinished operation");
        }
    }

    private static Object admit(ZLinkJavaRawMeshNode node, long operation,
        byte[] fingerprint, long deadline) throws Exception {
        Class<?> keyType = Class.forName(ZLinkJavaRawMeshNode.class.getName()
            + "$UserSpotOperationKey");
        var constructor = keyType.getDeclaredConstructor(
            RoutingId.class, long.class, long.class, long.class);
        constructor.setAccessible(true);
        Object key = constructor.newInstance(RoutingId.from("source"), 1L, 1L, operation);
        Method admit = ZLinkJavaRawMeshNode.class.getDeclaredMethod(
            "admitUserSpotOperation", keyType, byte[].class, long.class);
        admit.setAccessible(true);
        return admit.invoke(node, key, fingerprint, deadline);
    }

    private static Object property(Object owner, String name) throws Exception {
        Method method = owner.getClass().getDeclaredMethod(name);
        method.setAccessible(true);
        return method.invoke(owner);
    }

    private static void complete(Object slot) throws Exception {
        var field = slot.getClass().getDeclaredField("terminal");
        field.setAccessible(true);
        ((CompletableFuture<?>) field.get(slot)).complete(null);
    }
}
