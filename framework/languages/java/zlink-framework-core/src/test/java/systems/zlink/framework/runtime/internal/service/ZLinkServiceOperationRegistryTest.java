package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.concurrent.CompletionException;
import java.util.concurrent.Callable;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CancellationException;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.CountDownLatch;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

final class ZLinkServiceOperationRegistryTest {
    @Test
    void oneMaintenanceTaskServesEveryPendingDeadlineAndExpiresAtTheBoundary() {
        AtomicLong clock = new AtomicLong(100);
        try (var scheduler = new CountingScheduler();
             var registry = new ZLinkServiceOperationRegistry(
                 scheduler,
                 new IllegalStateException("closed"),
                 clock::get)) {
            var first = registry.register(Duration.ofNanos(10));
            var second = registry.register(Duration.ofNanos(10));

            assertEquals(1, scheduler.fixedRateSchedules.get());
            assertEquals(0, scheduler.otherSchedules.get());
            assertEquals(0, registry.expire(109));
            assertEquals(2, registry.expire(110));
            assertEquals(0, registry.pendingCount());
            assertTrue(assertThrows(
                CompletionException.class,
                first.completion()::join).getCause() instanceof TimeoutException);
            assertTrue(assertThrows(
                CompletionException.class,
                second.completion()::join).getCause() instanceof TimeoutException);
        }
    }

    @Test
    void deadlineComparisonSurvivesNanoTimeWraparound() {
        AtomicLong clock = new AtomicLong(Long.MAX_VALUE - 2);
        try (var scheduler = new CountingScheduler();
             var registry = new ZLinkServiceOperationRegistry(
                 scheduler, new IllegalStateException("closed"), clock::get)) {
            var operation = registry.register(Duration.ofNanos(5));

            assertEquals(0, registry.expire(Long.MAX_VALUE));
            assertEquals(1, registry.expire(Long.MIN_VALUE + 2));
            assertTrue(assertThrows(CompletionException.class,
                operation.completion()::join).getCause() instanceof TimeoutException);
        }
    }

    @Test
    void registrationsDoNotCreateAdditionalScheduledTasks() {
        try (var scheduler = new CountingScheduler();
             var registry = new ZLinkServiceOperationRegistry(scheduler)) {
            for (int index = 0; index < 100; index++) {
                var operation = registry.register(Duration.ofHours(1));
                assertTrue(registry.discard(operation.id()));
            }

            assertEquals(1, scheduler.fixedRateSchedules.get());
            assertEquals(0, scheduler.otherSchedules.get());
        }
    }

    @Test
    void submitSerializesRegistrationAndBindingStartAgainstClose() throws Exception {
        var closeFailure = new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.SHUTTING_DOWN,
            "channel runtime is closed");
        CountDownLatch submissionEntered = new CountDownLatch(1);
        CountDownLatch releaseSubmission = new CountDownLatch(1);
        CountDownLatch closeStarted = new CountDownLatch(1);
        CountDownLatch closeReturned = new CountDownLatch(1);
        AtomicReference<CompletableFuture<String>> submittedResult =
            new AtomicReference<>();
        AtomicReference<String> discarded = new AtomicReference<>();
        CompletableFuture<String> transport = new CompletableFuture<>();
        try (var scheduler = Executors.newSingleThreadScheduledExecutor();
             var registry = new ZLinkServiceOperationRegistry(
                 scheduler, closeFailure)) {
            Thread submitter = Thread.ofPlatform().start(() ->
                submittedResult.set(registry.submit(
                    ZLinkServiceOperationIds.next(),
                    Duration.ofHours(1),
                    () -> {
                        submissionEntered.countDown();
                        await(releaseSubmission);
                        return transport;
                    },
                    discarded::set)));
            assertTrue(submissionEntered.await(1, TimeUnit.SECONDS));
            Thread closer = Thread.ofPlatform().start(() -> {
                closeStarted.countDown();
                registry.close();
                closeReturned.countDown();
            });
            assertTrue(closeStarted.await(1, TimeUnit.SECONDS));
            assertFalse(closeReturned.await(20, TimeUnit.MILLISECONDS));

            releaseSubmission.countDown();
            submitter.join();
            closer.join();

            CompletionException closed = assertThrows(
                CompletionException.class,
                submittedResult.get()::join);
            assertSame(closeFailure, closed.getCause());
            assertTrue(transport.complete("late reply"));
            assertEquals("late reply", discarded.get());
            assertEquals(0, registry.pendingCount());
        }
    }

    @Test
    void reentrantCloseWinsOverASubsequentSubmissionFailure() {
        var closed = new IllegalStateException("closed during binding submission");
        try (var scheduler = Executors.newSingleThreadScheduledExecutor();
             var registry = new ZLinkServiceOperationRegistry(scheduler, closed)) {
            CompletableFuture<String> result = registry.submit(
                ZLinkServiceOperationIds.next(), Duration.ofSeconds(1), () -> {
                    registry.close();
                    throw new IllegalArgumentException("late submission failure");
                }, ignored -> { });
            assertSame(closed, assertThrows(CompletionException.class, result::join).getCause());
            assertEquals(0, registry.pendingCount());
        }
    }

    @Test
    void synchronousSubmissionFailureUsesTheReservedCompletion() {
        RuntimeException expected = new RuntimeException("submit failed");
        try (var scheduler = Executors.newSingleThreadScheduledExecutor();
             var registry = new ZLinkServiceOperationRegistry(scheduler)) {
            CompletableFuture<String> result = registry.submit(
                ZLinkServiceOperationIds.next(),
                Duration.ofSeconds(1),
                () -> {
                    throw expected;
                },
                ignored -> { });

            CompletionException failure = assertThrows(
                CompletionException.class,
                result::join);
            assertSame(expected, failure.getCause());
            assertEquals(0, registry.pendingCount());
        }
    }

    @Test
    void submitAfterCloseReturnsTheConfiguredFailureWithoutStartingBinding() {
        var closeFailure = new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.SHUTTING_DOWN,
            "channel runtime is closed");
        AtomicInteger starts = new AtomicInteger();
        try (var scheduler = Executors.newSingleThreadScheduledExecutor();
             var registry = new ZLinkServiceOperationRegistry(
                 scheduler, closeFailure)) {
            registry.close();

            CompletableFuture<String> result = registry.submit(
                ZLinkServiceOperationIds.next(),
                Duration.ofSeconds(1),
                () -> {
                    starts.incrementAndGet();
                    return CompletableFuture.completedFuture("unexpected");
                },
                ignored -> { });

            CompletionException failure = assertThrows(
                CompletionException.class,
                result::join);
            assertSame(closeFailure, failure.getCause());
            assertEquals(0, starts.get());
        }
    }

    @Test
    void wireIdentityIsTheRegistryKeyAndInvalidIdentityDoesNotRegister() {
        try (var scheduler = Executors.newSingleThreadScheduledExecutor();
             var registry = new ZLinkServiceOperationRegistry(scheduler)) {
            UUID firstId = new UUID(Long.MIN_VALUE, 7);
            UUID secondId = new UUID(Long.MAX_VALUE, 7);
            var first = registry.register(firstId, Duration.ofSeconds(1));
            assertSame(firstId, first.id());
            assertThrows(IllegalArgumentException.class,
                () -> registry.register(firstId, Duration.ofSeconds(1)));
            assertThrows(IllegalArgumentException.class,
                () -> registry.register(new UUID(0, 0), Duration.ofSeconds(1)));
            assertEquals(1, registry.pendingCount());
            var second = registry.register(secondId, Duration.ofSeconds(1));
            assertTrue(registry.complete(secondId, "second"));
            assertTrue(registry.complete(firstId, "first"));
            assertEquals("first", first.completion().join());
            assertEquals("second", second.completion().join());
        }
    }

    @Test
    void pendingEntryDoesNotAllocateAnExceptionForASuccessfulRequest() throws Exception {
        try (var scheduler = Executors.newSingleThreadScheduledExecutor();
             var registry = new ZLinkServiceOperationRegistry(scheduler)) {
            var operation = registry.register(Duration.ofSeconds(1));
            var entriesField = ZLinkServiceOperationRegistry.class.getDeclaredField("entries");
            entriesField.setAccessible(true);
            var entries = (java.util.Map<?, ?>) entriesField.get(registry);
            Object entry = entries.get(operation.id());
            for (var field : entry.getClass().getDeclaredFields()) {
                if (Throwable.class.isAssignableFrom(field.getType())) {
                    field.setAccessible(true);
                    assertNull(field.get(entry), field.getName());
                }
            }
            assertTrue(registry.complete(operation.id(), "success"));
            assertEquals("success", operation.completion().join());
        }
    }

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
    void registersMoreThanTheFormerPendingCapacity() {
        ScheduledExecutorService scheduler =
            Executors.newSingleThreadScheduledExecutor();
        try (ZLinkServiceOperationRegistry registry =
                 new ZLinkServiceOperationRegistry(scheduler)) {
            List<UUID> pending = new ArrayList<>();
            for (int index = 0; index <= 4_096; index++) {
                pending.add(registry.register(Duration.ofSeconds(1)).id());
            }
            assertEquals(4_097, registry.pendingCount());
            pending.forEach(registry::discard);
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
            // The terminal runs on the completion lane (spec 01 §11). join()
            // on the source stage returns when the source completes, not
            // when its dependents have run; the dependent stage returned by
            // whenComplete is what completes after the callback returned.
            var observed = operation.completion().whenComplete((ignored, failure) ->
                callbacks.incrementAndGet());

            assertTrue(operation.completion().cancel(false));
            assertEquals(0, registry.pendingCount());
            assertThrows(CancellationException.class, operation.completion()::join);
            CompletionException relayed =
                assertThrows(CompletionException.class, observed::join);
            assertTrue(relayed.getCause() instanceof CancellationException);
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
    void registrationsAreNotCappedAcrossRegistries() {
        ScheduledExecutorService scheduler =
            Executors.newSingleThreadScheduledExecutor();
        try (ZLinkServiceOperationRegistry first =
                 new ZLinkServiceOperationRegistry(scheduler);
             ZLinkServiceOperationRegistry second =
                 new ZLinkServiceOperationRegistry(scheduler)) {
            List<UUID> firstIds = new ArrayList<>();
            List<UUID> secondIds = new ArrayList<>();
            for (int index = 0; index <= 2_048; index++) {
                firstIds.add(first.register(Duration.ofHours(1)).id());
                secondIds.add(second.register(Duration.ofHours(1)).id());
            }

            UUID admittedAfterFormerLimit =
                second.register(Duration.ofHours(1)).id();
            firstIds.forEach(id -> assertTrue(first.discard(id)));
            secondIds.forEach(id -> assertTrue(second.discard(id)));
            assertTrue(second.discard(admittedAfterFormerLimit));
        } finally {
            scheduler.shutdownNow();
        }
    }

    @Test
    void runningCallbackDoesNotCapFurtherRegistrations() throws Exception {
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
            for (int index = 1; index <= 4_096; index++) {
                pending.add(second.register(Duration.ofHours(1)).id());
            }

            try {
                assertTrue(first.complete(running.id(), "reply"));
                assertTrue(callbackEntered.await(1, TimeUnit.SECONDS));
                pending.add(second.register(Duration.ofHours(1)).id());
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

    private static void await(CountDownLatch latch) {
        try {
            latch.await();
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException(interrupted);
        }
    }

    private static final class CountingScheduler
        extends ScheduledThreadPoolExecutor {
        private final AtomicInteger fixedRateSchedules = new AtomicInteger();
        private final AtomicInteger otherSchedules = new AtomicInteger();

        private CountingScheduler() {
            super(1);
        }

        @Override
        public ScheduledFuture<?> scheduleAtFixedRate(
            Runnable command,
            long initialDelay,
            long period,
            TimeUnit unit) {
            fixedRateSchedules.incrementAndGet();
            return super.scheduleAtFixedRate(command, initialDelay, period, unit);
        }

        @Override
        public ScheduledFuture<?> schedule(Runnable command, long delay, TimeUnit unit) {
            otherSchedules.incrementAndGet();
            return super.schedule(command, delay, unit);
        }

        @Override
        public <V> ScheduledFuture<V> schedule(Callable<V> command, long delay, TimeUnit unit) {
            otherSchedules.incrementAndGet();
            return super.schedule(command, delay, unit);
        }

        @Override
        public ScheduledFuture<?> scheduleWithFixedDelay(
            Runnable command, long initialDelay, long delay, TimeUnit unit) {
            otherSchedules.incrementAndGet();
            return super.scheduleWithFixedDelay(command, initialDelay, delay, unit);
        }
    }
}
