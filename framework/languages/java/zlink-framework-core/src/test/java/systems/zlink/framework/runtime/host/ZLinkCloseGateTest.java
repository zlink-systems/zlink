package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;

final class ZLinkCloseGateTest {
    @Test
    void concurrentCloseSharesNonblockingOwnershipCleanupStage() throws Exception {
        ZLinkCloseGate gate = new ZLinkCloseGate();
        CompletableFuture<Void> cleanup = new CompletableFuture<>();
        AtomicInteger cleanupCalls = new AtomicInteger();

        var first = gate.close(() -> {
            cleanupCalls.incrementAndGet();
            return cleanup;
        });
        var second = gate.close(() -> {
            cleanupCalls.incrementAndGet();
            return CompletableFuture.completedFuture(null);
        });

        assertTrue(first == second);
        assertTrue(!second.toCompletableFuture().isDone());

        cleanup.complete(null);
        second.toCompletableFuture().get(1, TimeUnit.SECONDS);

        assertEquals(1, cleanupCalls.get());
    }
}
