package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CancellationException;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

final class ZLinkServiceOperationRegistryTest {
    @Test
    void firstTerminalPathWinsAndDetachesDeadline() {
        ScheduledExecutorService scheduler = Executors.newSingleThreadScheduledExecutor();
        try (ZLinkServiceOperationRegistry registry = new ZLinkServiceOperationRegistry(scheduler)) {
            ZLinkServiceOperationRegistry.Operation<String> operation =
                registry.register(Duration.ofSeconds(1));
            assertTrue(registry.complete(operation.id(), "reply"));
            assertFalse(registry.completeExceptionally(
                operation.id(), new TimeoutException("late timeout")));
            assertEquals("reply", operation.completion().join());
            assertEquals(0, registry.pendingCount());
        } finally {
            scheduler.shutdownNow();
        }
    }

    @Test
    void timeoutCompletesExactlyOnce() throws Exception {
        ScheduledExecutorService scheduler = Executors.newSingleThreadScheduledExecutor();
        try (ZLinkServiceOperationRegistry registry = new ZLinkServiceOperationRegistry(scheduler)) {
            ZLinkServiceOperationRegistry.Operation<String> operation =
                registry.register(Duration.ofMillis(10));
            CompletionException failure = assertThrows(
                CompletionException.class,
                () -> operation.completion().orTimeout(1, TimeUnit.SECONDS).join());
            assertTrue(failure.getCause() instanceof TimeoutException);
            assertFalse(registry.complete(operation.id(), "late reply"));
        } finally {
            scheduler.shutdownNow();
        }
    }

    @Test
    void rejectsRegistrationWhenPendingCapacityIsExhausted() {
        ScheduledExecutorService scheduler =
            Executors.newSingleThreadScheduledExecutor();
        try (ZLinkServiceOperationRegistry registry =
                 new ZLinkServiceOperationRegistry(scheduler, 1)) {
            registry.register(Duration.ofSeconds(1));
            ZLinkFrameworkException failure = assertThrows(
                ZLinkFrameworkException.class,
                () -> registry.register(Duration.ofSeconds(1)));
            assertEquals(ZLinkFrameworkErrorKind.CAPACITY_EXCEEDED, failure.kind());
            assertEquals(1, registry.pendingCount());
        } finally {
            scheduler.shutdownNow();
        }
    }

    @Test
    void cancellationAtomicallyTakesTheOperationBeforeClose() {
        ScheduledExecutorService scheduler =
            Executors.newSingleThreadScheduledExecutor();
        try (ZLinkServiceOperationRegistry registry =
                 new ZLinkServiceOperationRegistry(scheduler)) {
            var operation = registry.register(Duration.ofSeconds(1));
            AtomicInteger callbacks = new AtomicInteger();
            operation.completion().whenComplete((ignored, failure) ->
                callbacks.incrementAndGet());

            assertTrue(operation.completion().cancel(false));
            assertEquals(0, registry.pendingCount());
            assertThrows(CancellationException.class, operation.completion()::join);
            registry.close();
            assertEquals(1, callbacks.get());
            assertFalse(registry.complete(operation.id(), "late"));
        } finally {
            scheduler.shutdownNow();
        }
    }

    @Test
    void terminalCompletionRunsOnANewTurnOutsideTheRegistryGate() {
        ScheduledExecutorService scheduler =
            Executors.newSingleThreadScheduledExecutor(runnable -> {
                Thread thread = new Thread(runnable, "operation-completion-turn");
                thread.setDaemon(true);
                return thread;
            });
        try (ZLinkServiceOperationRegistry registry =
                 new ZLinkServiceOperationRegistry(scheduler)) {
            var operation = registry.register(Duration.ofSeconds(1));
            var callbackThread = new java.util.concurrent.CompletableFuture<String>();
            operation.completion().whenComplete((ignored, failure) ->
                callbackThread.complete(Thread.currentThread().getName()));

            assertTrue(registry.complete(operation.id(), "reply"));
            assertEquals("reply", operation.completion().join());
            assertEquals(
                "operation-completion-turn",
                callbackThread.orTimeout(1, TimeUnit.SECONDS).join());
        } finally {
            scheduler.shutdownNow();
        }
    }
}
