package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import java.time.Instant;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicBoolean;

final class ZLinkForceStop2646Test {
    @Test
    void expiredStoreStageDoesNotStartButLocalCleanupRuns() {
        var shutdown = new ZLinkFrameworkShutdown(Instant.now().minusSeconds(1));
        var storeStarted = new AtomicBoolean();
        var localClosed = new AtomicBoolean();
        shutdown.deferCloseStage(
                "local",
                () -> {
                    localClosed.set(true);
                    return CompletableFuture.completedFuture(null);
                });
        shutdown.deferStage(
                "store",
                () -> {
                    storeStarted.set(true);
                    return CompletableFuture.completedFuture(null);
                });
        assertThrows(
                CompletionException.class,
                () -> shutdown.closeAsync().toCompletableFuture().join());
        assertFalse(storeStarted.get());
        assertTrue(localClosed.get());
    }

    @Test
    void inFlightStoreStageSettlesBeforeLocalCleanupAndTerminal() throws Exception {
        var pending = new CompletableFuture<Void>();
        var localClosed = new AtomicBoolean();
        var shutdown = new ZLinkFrameworkShutdown(Instant.now().plusMillis(20));
        shutdown.deferCloseStage(
                "local",
                () -> {
                    localClosed.set(true);
                    return CompletableFuture.completedFuture(null);
                });
        shutdown.deferStage("store", () -> pending);
        var stopping = shutdown.closeAsync().toCompletableFuture();
        assertThrows(TimeoutException.class, () -> stopping.get(40, TimeUnit.MILLISECONDS));
        assertFalse(localClosed.get());
        pending.complete(null);
        assertThrows(CompletionException.class, stopping::join);
        assertTrue(localClosed.get());
    }
}
