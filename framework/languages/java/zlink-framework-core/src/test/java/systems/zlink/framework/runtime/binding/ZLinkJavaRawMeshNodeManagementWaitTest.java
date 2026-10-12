package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceLivenessRegistry;

import java.time.Duration;

final class ZLinkJavaRawMeshNodeManagementWaitTest {
    private static final Duration NO_DEADLINE = Duration.ofMillis(-1);

    @Test
    void readinessWaitUsesTheNextManagementDeadline() throws Exception {
        try (var context = Zlink.createContext();
                var node = new ZLinkJavaRawMeshNode(context, "management-wait")) {
            var field = ZLinkJavaRawMeshNode.class.getDeclaredField("liveness");
            field.setAccessible(true);
            var liveness = (ZLinkServiceLivenessRegistry) field.get(node);
            liveness.admit(RoutingId.from("deadline-peer"), "connection", 0);
            liveness.tick(0);
            assertEquals(ZLinkServiceLivenessRegistry.DEFAULT_PROBE_INTERVAL, waitTimeout(node, 0));
        }
    }

    @Test
    void expiryBeforeTheNextProbeOwnsTheWaitAndSubMillisecondWaitsStayBlocking() {
        var liveness = new ZLinkServiceLivenessRegistry();
        liveness.admit(RoutingId.from("expiry-peer"), "connection", 0);
        liveness.tick(Duration.ofSeconds(14).toNanos());
        assertEquals(Duration.ofSeconds(1), liveness.waitTimeout(Duration.ofSeconds(14).toNanos()));
        assertEquals(
                Duration.ofMillis(1), liveness.waitTimeout(Duration.ofSeconds(15).toNanos() - 1));
        assertEquals(Duration.ZERO, liveness.waitTimeout(Duration.ofSeconds(15).toNanos()));
    }

    @Test
    void noManagementDeadlineDoesNotSchedulePeriodicDataChecks() throws Exception {
        try (var context = Zlink.createContext();
                var node = new ZLinkJavaRawMeshNode(context, "no-deadline")) {
            assertEquals(NO_DEADLINE, waitTimeout(node, 0));
        }
    }

    private static Duration waitTimeout(ZLinkJavaRawMeshNode node, long now) throws Exception {
        var field = ZLinkJavaRawMeshNode.class.getDeclaredField("liveness");
        field.setAccessible(true);
        return ((ZLinkServiceLivenessRegistry) field.get(node)).waitTimeout(now);
    }
}
