package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceLivenessRegistry;

import java.time.Duration;

final class ZLinkJavaRawMeshIdleWaitTest {
    @Test
    void idleWithoutManagementHasNoDeadline() {
        try (var context = Zlink.createContext();
                var node = new ZLinkJavaRawMeshNode(context, "idle-wait")) {
            node.livenessForTest().tick(0);
            assertEquals(Long.MAX_VALUE, node.managementDeadlineNanos(0));
        }
    }

    @Test
    void admittedPeerUsesItsActualProbeDeadline() {
        try (var context = Zlink.createContext();
                var node = new ZLinkJavaRawMeshNode(context, "peer-wait")) {
            var liveness = node.livenessForTest();
            liveness.admit(RoutingId.from("peer"), "connection", 0);
            liveness.tick(0);
            assertEquals(Duration.ofSeconds(5).toNanos(), node.managementDeadlineNanos(0));
            assertEquals(
                    Duration.ofSeconds(4).toNanos(),
                    node.managementDeadlineNanos(1_000_000_000L) - 1_000_000_000L);
        }
    }

    @Test
    void absentDisconnectedAndExpiredPeersHaveNoDeadline() {
        var registry = new ZLinkServiceLivenessRegistry();
        var peer = RoutingId.from("peer");
        assertEquals(Long.MAX_VALUE, registry.nextDeadlineNanos(0));
        registry.admit(peer, "first", 0);
        registry.tick(0);
        registry.disconnect(peer, "first");
        assertEquals(Long.MAX_VALUE, registry.nextDeadlineNanos(0));
        registry.admit(peer, "second", 0);
        registry.tick(Duration.ofSeconds(15).toNanos());
        assertEquals(Long.MAX_VALUE, registry.nextDeadlineNanos(Duration.ofSeconds(15).toNanos()));
    }

    @Test
    void admissionDeadlineRemainsRealWithNegativeMonotonicTime() {
        var registry = new ZLinkServiceLivenessRegistry();
        long now = -Duration.ofSeconds(10).toNanos();
        registry.tick(now);
        assertEquals(Long.MAX_VALUE, registry.nextDeadlineNanos(now));
        registry.setAdmissionPending(true, now);
        assertEquals(now + Duration.ofSeconds(5).toNanos(), registry.nextDeadlineNanos(now));
        registry.setAdmissionPending(false, now);
        assertEquals(Long.MAX_VALUE, registry.nextDeadlineNanos(now));
    }
}
