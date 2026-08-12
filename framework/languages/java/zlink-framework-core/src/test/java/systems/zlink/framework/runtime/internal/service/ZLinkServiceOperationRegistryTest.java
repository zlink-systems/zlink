package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
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
import java.util.concurrent.CountDownLatch;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
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
    void invalidFailureDoesNotConsumeThePendingOperation() {
        ScheduledExecutorService scheduler =
            Executors.newSingleThreadScheduledExecutor();
        try (ZLinkServiceOperationRegistry registry =
                 new ZLinkServiceOperationRegistry(scheduler)) {
            var operation = registry.register(Duration.ofSeconds(1));

            assertThrows(
                NullPointerException.class,
                () -> registry.completeExceptionally(operation.id(), null));
            assertEquals(1, registry.pendingCount());
            assertTrue(registry.complete(operation.id(), "reply"));
            assertEquals("reply", operation.completion().join());
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
            Executors.newSingleThreadScheduledExecutor();
        try (ZLinkServiceOperationRegistry registry =
                 new ZLinkServiceOperationRegistry(scheduler)) {
            var operation = registry.register(Duration.ofSeconds(1));
            var callbackThread = new java.util.concurrent.CompletableFuture<String>();
            String callerThread = Thread.currentThread().getName();
            operation.completion().whenComplete((ignored, failure) ->
                callbackThread.complete(Thread.currentThread().getName()));

            assertTrue(registry.complete(operation.id(), "reply"));
            assertEquals("reply", operation.completion().join());
            String actualThread =
                callbackThread.orTimeout(1, TimeUnit.SECONDS).join();
            assertNotEquals(callerThread, actualThread);
            assertEquals("zlink-jvm-service-completion", actualThread);
        } finally {
            scheduler.shutdownNow();
        }
    }

    @Test
    void capacityIsSharedAcrossRegistriesUntilTerminalCallbacksReturn() {
        ScheduledExecutorService scheduler =
            Executors.newSingleThreadScheduledExecutor();
        try (ZLinkServiceOperationRegistry first =
                 new ZLinkServiceOperationRegistry(scheduler);
             ZLinkServiceOperationRegistry second =
                 new ZLinkServiceOperationRegistry(scheduler)) {
            List<UUID> firstIds = new ArrayList<>();
            List<UUID> secondIds = new ArrayList<>();
            for (int index = 0; index < 2_048; index++) {
                firstIds.add(first.register(Duration.ofHours(1)).id());
                secondIds.add(second.register(Duration.ofHours(1)).id());
            }

            ZLinkFrameworkException failure = assertThrows(
                ZLinkFrameworkException.class,
                () -> second.register(Duration.ofHours(1)));
            assertEquals(ZLinkFrameworkErrorKind.CAPACITY_EXCEEDED, failure.kind());

            assertTrue(first.discard(firstIds.removeLast()));
            UUID admittedAfterRelease =
                second.register(Duration.ofHours(1)).id();
            firstIds.forEach(id -> assertTrue(first.discard(id)));
            secondIds.forEach(id -> assertTrue(second.discard(id)));
            assertTrue(second.discard(admittedAfterRelease));
        } finally {
            scheduler.shutdownNow();
        }
    }

    @Test
    void runningCallbackKeepsItsProcessReservation() throws Exception {
        ScheduledExecutorService scheduler =
            Executors.newSingleThreadScheduledExecutor();
        try (ZLinkServiceOperationRegistry first =
                 new ZLinkServiceOperationRegistry(scheduler);
             ZLinkServiceOperationRegistry second =
                 new ZLinkServiceOperationRegistry(scheduler)) {
            var running = first.register(Duration.ofHours(1));
            CountDownLatch callbackEntered = new CountDownLatch(1);
            CountDownLatch releaseCallback = new CountDownLatch(1);
            running.completion().whenComplete((ignored, failure) -> {
                callbackEntered.countDown();
                try {
                    releaseCallback.await();
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                }
            });
            List<UUID> pending = new ArrayList<>();
            for (int index = 1;
                 index < ZLinkServiceOperationRegistry.DEFAULT_MAX_PENDING_OPERATIONS;
                 index++) {
                pending.add(second.register(Duration.ofHours(1)).id());
            }

            try {
                assertTrue(first.complete(running.id(), "reply"));
                assertTrue(callbackEntered.await(1, TimeUnit.SECONDS));
                ZLinkFrameworkException failure = assertThrows(
                    ZLinkFrameworkException.class,
                    () -> second.register(Duration.ofHours(1)));
                assertEquals(
                    ZLinkFrameworkErrorKind.CAPACITY_EXCEEDED,
                    failure.kind());
                pending.forEach(id -> assertTrue(second.discard(id)));
                pending.clear();
            } finally {
                pending.forEach(second::discard);
                releaseCallback.countDown();
            }
            assertEquals(
                "reply",
                running.completion().orTimeout(1, TimeUnit.SECONDS).join());
        } finally {
            scheduler.shutdownNow();
        }
    }

    @Test
    void closeDispatchesEveryAcceptedTerminalInRegistrationOrder() throws Exception {
        ScheduledExecutorService scheduler =
            Executors.newSingleThreadScheduledExecutor();
        ZLinkServiceOperationRegistry registry =
            new ZLinkServiceOperationRegistry(scheduler);
        try {
            List<Integer> observed = new java.util.concurrent.CopyOnWriteArrayList<>();
            CountDownLatch callbacks = new CountDownLatch(3);
            for (int index = 0; index < 3; index++) {
                int expected = index;
                registry.register(Duration.ofHours(1)).completion()
                    .whenComplete((ignored, failure) -> {
                        observed.add(expected);
                        callbacks.countDown();
                    });
            }

            registry.close();

            assertTrue(callbacks.await(1, TimeUnit.SECONDS));
            assertEquals(List.of(0, 1, 2), observed);
            assertEquals(0, registry.pendingCount());
        } finally {
            registry.close();
            scheduler.shutdownNow();
        }
    }

    @Test
    void acceptedTerminalDoesNotDependOnTheDeadlineSchedulerRemainingOpen() {
        ScheduledExecutorService scheduler =
            Executors.newSingleThreadScheduledExecutor();
        try (ZLinkServiceOperationRegistry registry =
                 new ZLinkServiceOperationRegistry(scheduler)) {
            var operation = registry.register(Duration.ofHours(1));
            scheduler.shutdownNow();

            assertTrue(registry.complete(operation.id(), "reply"));
            assertEquals(
                "reply",
                operation.completion().orTimeout(1, TimeUnit.SECONDS).join());
        } finally {
            scheduler.shutdownNow();
        }
    }
}
