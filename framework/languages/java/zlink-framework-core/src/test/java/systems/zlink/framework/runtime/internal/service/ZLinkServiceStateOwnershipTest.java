package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

final class ZLinkServiceStateOwnershipTest {
    @Test
    void livenessStateWorkHasAnActualLaneOwnerAndRejectsReentry() {
        var lane = new ZLinkStateLane(Runnable::run);
        var registry =
                new ZLinkServiceLivenessRegistry(
                        ZLinkServiceLivenessRegistry.DEFAULT_PROBE_INTERVAL,
                        ZLinkServiceLivenessRegistry.DEFAULT_PEER_TIMEOUT,
                        lane);
        assertEquals(0, registry.size());
        var reentry =
                lane.runAsync(
                        () -> {
                            assertSame(lane, ZLinkStateLane.current());
                            assertThrows(IllegalStateException.class, registry::size);
                            return null;
                        });
        reentry.toCompletableFuture().join();
        assertEquals(0, registry.size());
        lane.closeAsync().toCompletableFuture().join();
    }
}
