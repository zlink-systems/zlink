package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import java.time.Instant;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * A resource-closing teardown stage runs after the stage that uses the resource has stopped, even
 * when the host shutdown deadline has already expired: the expired deadline must not start the next
 * stage while the previous one is still running.
 */
final class ZLinkFrameworkShutdownOrderTest {
    private static final long OVERLAP_WINDOW_SECONDS = 2;
    private static final long WAIT_SECONDS = 10;

    @Test
    void expiredDeadlineWaitsForAnAsynchronousResourceClose() throws Exception {
        var resourceStopped = new CompletableFuture<Void>();
        var contextCloseStarted = new CompletableFuture<Void>();
        var shutdown = new ZLinkFrameworkShutdown(Instant.now().minusSeconds(1));
        shutdown.deferCloseStage(
                "context_close",
                () -> {
                    contextCloseStarted.complete(null);
                    return CompletableFuture.completedFuture(null);
                });
        shutdown.deferCloseStage("resource_close", () -> resourceStopped);
        var closing = shutdown.closeAsync();
        try {
            assertFalse(contextCloseStarted.isDone(), "resource close has not completed");
            assertFalse(closing.toCompletableFuture().isDone());
        } finally {
            resourceStopped.complete(null);
            closing.handle((ignored, failure) -> null)
                    .toCompletableFuture()
                    .get(WAIT_SECONDS, TimeUnit.SECONDS);
        }
        contextCloseStarted.get(WAIT_SECONDS, TimeUnit.SECONDS);
    }

    @Test
    void expiredDeadlineClosesTheContextOnlyAfterTheMeshPumpStopped() throws Exception {
        List<String> events = new CopyOnWriteArrayList<>();
        CountDownLatch contextCloseStarted = new CountDownLatch(1);
        AtomicBoolean overlapped = new AtomicBoolean();
        var shutdown = new ZLinkFrameworkShutdown(Instant.now().minusSeconds(1));
        // Stages run in reverse registration order, as in ZLinkFrameworkRuntime.closeCoreAsync.
        shutdown.defer(
                "context_close",
                () -> {
                    contextCloseStarted.countDown();
                    events.add("context closed");
                });
        shutdown.defer(
                "route_mesh_close",
                () -> {
                    try {
                        overlapped.set(
                                contextCloseStarted.await(
                                        OVERLAP_WINDOW_SECONDS, TimeUnit.SECONDS));
                    } catch (InterruptedException interrupted) {
                        Thread.currentThread().interrupt();
                    }
                    events.add("pump stopped");
                });

        shutdown.closeAsync()
                .handle((ignored, failure) -> null)
                .toCompletableFuture()
                .get(WAIT_SECONDS, TimeUnit.SECONDS);
        assertTrue(contextCloseStarted.await(WAIT_SECONDS, TimeUnit.SECONDS));

        assertFalse(overlapped.get(), "context_close started while route_mesh_close ran");
        assertEquals(List.of("pump stopped", "context closed"), events);
    }
}
