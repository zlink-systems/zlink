package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.internal.calls.ZLinkBlockingCalls;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;
import systems.zlink.framework.runtime.internal.drain.ZLinkMeshDrainCoordinator;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext;

import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Supplier;

final class ZLinkApplicationExecutionSchedulerTest {
    private static final CompletableFuture<Void> DONE = CompletableFuture.completedFuture(null);

    @Test
    void fifoClaimsStayUniqueAndReadyUntilLastHead() throws Exception {
        var executor = new ManualExecutor();
        var scheduler = new ZLinkApplicationExecutionScheduler(executor, 1);
        var owner = scheduler.register();
        owner.enqueue(() -> DONE, null); // Occupies the only reservation, without running it.
        var operations = new ArrayList<Supplier<CompletionStage<Void>>>();
        for (int i = 0; i < 20; i++) {
            final int id = i;
            Supplier<CompletionStage<Void>> operation =
                    () -> {
                        assertTrue(id >= 0);
                        return DONE;
                    };
            operations.add(operation);
            owner.enqueue(operation, null);
        }
        var ready = scheduler.ready;
        assertEquals(List.of(owner), List.copyOf(ready));
        var claims = new ArrayList<Object>();
        for (int i = 0; i < 20; i++) {
            var claim = scheduler.takeLocked();
            assertSame(operations.get(i), claim.operation, "FIFO is checked at take");
            claims.add(claim);
            assertEquals(i == 19 ? 0 : 1, ready.size());
        }
        assertUnique(claims);
        claims.add(claims.getFirst());
        assertThrows(AssertionError.class, () -> assertUnique(claims), "duplicate claim control");
        assertNull(scheduler.takeLocked());
        owner.enqueue(() -> DONE, null);
        assertEquals(
                List.of(owner),
                List.copyOf(ready),
                "enqueue after last take republishes the owner");
        executor.drain();
        assertTrue(ready.isEmpty());
        assertEquals(1, scheduler.idle.size());
    }

    private static void assertUnique(List<Object> claims) {
        Set<Object> unique = Collections.newSetFromMap(new IdentityHashMap<>());
        for (Object claim : claims) assertTrue(unique.add(claim), "record must have one claimant");
    }

    @Test
    void reservationsAreBoundedAndDrainsReuseSlotRunnables() throws Exception {
        var executor = new ManualExecutor();
        var scheduler = new ZLinkApplicationExecutionScheduler(executor, 2);
        var owner = scheduler.register();
        var seen = new BitSet();
        var terminals = new ArrayList<CompletableFuture<Void>>();
        for (int i = 0; i < 500; i++) {
            int id = i;
            terminals.add(
                    owner.enqueue(
                                    () -> {
                                        assertFalse(seen.get(id));
                                        seen.set(id);
                                        return DONE;
                                    },
                                    null)
                            .toCompletableFuture());
        }
        assertEquals(2, executor.pending.size(), "only unreserved idle slots are submitted");
        assertEquals(1, scheduler.ready.size());
        executor.drain();
        CompletableFuture.allOf(terminals.toArray(CompletableFuture[]::new)).join();
        assertEquals(500, seen.cardinality());
        assertEquals(2, executor.identities.size(), "records reuse the same two slot Runnables");
        assertTrue(executor.submissions < 500, "detect per-record executor wrappers");
        assertThrows(AssertionError.class, () -> assertBatchSubmissions(500, 500));
        assertBatchSubmissions(executor.submissions, 500);
        assertEquals(2, scheduler.idle.size());
    }

    private static void assertBatchSubmissions(int submissions, int records) {
        assertTrue(submissions < records, "per-record submission is forbidden");
    }

    @Test
    void enqueueOnBothSidesOfIdleTransitionNeverLosesWake() throws Exception {
        try (var executor = Executors.newFixedThreadPool(2)) {
            var scheduler = new ZLinkApplicationExecutionScheduler(executor, 2);
            var owner = scheduler.register();
            var count = new AtomicInteger();
            for (int i = 0; i < 300; i++) {
                var done =
                        owner.enqueue(
                                        () -> {
                                            count.incrementAndGet();
                                            return DONE;
                                        },
                                        null)
                                .toCompletableFuture();
                // Terminal completion races the slot's next-take/idle transition.
                done.get(2, TimeUnit.SECONDS);
            }
            assertEquals(300, count.get());
        }
    }

    @Test
    void readyAndIdleHaveNoDuplicateMembershipEvenUnderConcurrentProducers() throws Exception {
        try (var workers = Executors.newFixedThreadPool(2);
                var producers = Executors.newFixedThreadPool(4)) {
            var scheduler = new ZLinkApplicationExecutionScheduler(workers, 2);
            var owner = scheduler.register();
            var calls = new AtomicInteger();
            var results = new ConcurrentLinkedQueue<CompletionStage<Void>>();
            var futures = new ArrayList<Future<?>>();
            for (int i = 0; i < 4; i++)
                futures.add(
                        producers.submit(
                                () -> {
                                    for (int j = 0; j < 250; j++)
                                        results.add(
                                                owner.enqueue(
                                                        () -> {
                                                            calls.incrementAndGet();
                                                            return DONE;
                                                        },
                                                        null));
                                }));
            for (var future : futures) future.get(2, TimeUnit.SECONDS);
            CompletableFuture.allOf(
                            results.stream()
                                    .map(CompletionStage::toCompletableFuture)
                                    .toArray(CompletableFuture[]::new))
                    .get(2, TimeUnit.SECONDS);
            assertEquals(1000, calls.get());
        }
    }

    @Test
    void permitSurvivesUnfinishedPrefixAndDrainButReturnsAtHandlerEntry() throws Exception {
        var executor = new ManualExecutor();
        var scheduler = new ZLinkApplicationExecutionScheduler(executor, 1);
        var owner = scheduler.register();
        try (var jobs = jobs()) {
            var prefix = new CompletableFuture<Void>();
            var handlerTerminal = new CompletableFuture<Void>();
            var recordTerminal = new CompletableFuture<Void>();
            var drains = new ZLinkMeshDrainCoordinator(List.of("test"));
            var permit =
                    jobs.acquire(ZLinkApplicationJobQueue.Origin.LOCAL)
                            .toCompletableFuture()
                            .join();
            final ZLinkSuspendInvocationContext.ApplicationTurn[] turn =
                    new ZLinkSuspendInvocationContext.ApplicationTurn[1];
            try (var context = ZLinkApplicationJobContext.enter(permit)) {
                ZLinkHandlerDispatch.enqueue(
                        owner,
                        () -> {
                            assertEquals(1, jobs.snapshot().permitsInUse());
                            turn[0] =
                                    (ZLinkSuspendInvocationContext.ApplicationTurn)
                                            ZLinkSuspendInvocationContext
                                                    .currentSerialExecutionTurn();
                            return prefix.thenCompose(ignored -> turn[0].manage(handlerTerminal));
                        },
                        null,
                        drains.tryClaim("test"),
                        failure -> recordTerminal.complete(null));
            } finally {
                permit.abandonReservation();
            }
            executor.drain();
            assertEquals(
                    1, jobs.snapshot().permitsInUse(), "STARTED must not return prefix permit");
            assertEquals(1, jobs.snapshot().queuedApplicationJobs());
            var stopping = drains.sealAndAwaitZero("test").toCompletableFuture();
            assertFalse(stopping.isDone(), "idle slots are not terminal zero");
            turn[0].execute(
                    () -> {
                        assertTrue(ZLinkBlockingCalls.isRuntimeExecutionContext());
                        assertThrows(RuntimeException.class, ZLinkBlockingCalls::requireMayBlock);
                        ZLinkApplicationJobContext.beforeFirstApplicationInstruction();
                        assertEquals(0, jobs.snapshot().permitsInUse());
                        prefix.complete(null);
                    });
            assertFalse(stopping.isDone());
            handlerTerminal.complete(null);
            stopping.get(2, TimeUnit.SECONDS);
            assertTrue(recordTerminal.isDone());
            assertEquals(0, jobs.snapshot().permitsInUse());
        }
    }

    @Test
    void preHandlerFailureCancellationAndRejectionReturnPermitExactlyOnce() throws Exception {
        for (int mode = 0; mode < 5; mode++) {
            var executor = new ManualExecutor();
            executor.reject = mode == 3;
            var scheduler = new ZLinkApplicationExecutionScheduler(executor, 1);
            var owner = scheduler.register();
            try (var jobs = jobs()) {
                var pending = new CompletableFuture<Void>();
                int selected = mode;
                var permit =
                        jobs.acquire(ZLinkApplicationJobQueue.Origin.LOCAL)
                                .toCompletableFuture()
                                .join();
                CompletionStage<Void> terminal;
                try (var context = ZLinkApplicationJobContext.enter(permit)) {
                    terminal =
                            owner.enqueue(
                                    () -> {
                                        if (selected == 0)
                                            throw new IllegalStateException("decode failure");
                                        return selected == 4 ? DONE : pending;
                                    },
                                    null);
                } finally {
                    permit.abandonReservation();
                }
                executor.drain();
                if (mode == 1)
                    pending.completeExceptionally(new IllegalStateException("filter failure"));
                if (mode == 2) pending.cancel(false);
                assertEquals(mode != 4, terminal.toCompletableFuture().isCompletedExceptionally());
                assertTrue(terminal.toCompletableFuture().isDone());
                assertEquals(0, jobs.snapshot().permitsInUse());
                assertEquals(0, jobs.snapshot().queuedApplicationJobs());
            }
        }
    }

    @Test
    void slotRestoresFlowPermitAndRuntimeContextAcrossRecordsAndCompletions() throws Exception {
        var executor = new ManualExecutor();
        var owner = new ZLinkApplicationExecutionScheduler(executor, 1).register();
        var first = new CompletableFuture<Void>();
        var observed = new CompletableFuture<Void>();
        String firstFlow;
        try (var flow = ZLinkFlowContext.enterOrCreate(null, ZLinkFlowOrigin.APPLICATION)) {
            firstFlow = ZLinkFlowContext.current().flowId();
            String expected = firstFlow;
            owner.enqueue(
                    () -> {
                        assertEquals(expected, ZLinkFlowContext.current().flowId());
                        return ZLinkSerialExecutionQueue.manageCurrent(first)
                                .thenRun(
                                        () -> {
                                            assertEquals(
                                                    expected, ZLinkFlowContext.current().flowId());
                                            assertTrue(
                                                    ZLinkBlockingCalls.isRuntimeExecutionContext());
                                            observed.complete(null);
                                        });
                    },
                    null);
        }
        try (var flow = ZLinkFlowContext.enterOrCreate(null, ZLinkFlowOrigin.APPLICATION)) {
            String expected = ZLinkFlowContext.current().flowId();
            assertNotEquals(firstFlow, expected);
            owner.enqueue(
                    () -> {
                        assertEquals(expected, ZLinkFlowContext.current().flowId());
                        assertTrue(ZLinkApplicationJobContext.current().isEmpty());
                        return DONE;
                    },
                    null);
        }
        executor.drain();
        assertNull(ZLinkFlowContext.current());
        assertFalse(ZLinkBlockingCalls.isRuntimeExecutionContext());
        first.complete(null);
        observed.get(2, TimeUnit.SECONDS);
        assertNull(ZLinkFlowContext.current());
        assertFalse(ZLinkBlockingCalls.isRuntimeExecutionContext());
    }

    @Test
    void closingAdmissionRejectsNewRecordsWithoutDiscardingAcceptedTerminal() throws Exception {
        var executor = new ManualExecutor();
        var owner = new ZLinkApplicationExecutionScheduler(executor, 1).register();
        var pending = new CompletableFuture<Void>();
        var accepted = owner.enqueue(() -> pending, null).toCompletableFuture();
        owner.sealClosingAdmission();
        var admission = new CompletableFuture<Void>();
        assertTrue(
                owner.enqueue(() -> DONE, admission)
                        .toCompletableFuture()
                        .isCompletedExceptionally());
        assertTrue(admission.isCompletedExceptionally());
        assertNull(owner.tryEnqueue(() -> DONE));
        executor.drain();
        assertFalse(accepted.isDone());
        pending.complete(null);
        accepted.get(2, TimeUnit.SECONDS);
    }

    @Test
    void lostReadyAndDuplicateMembershipMutationsAreDetected() throws Exception {
        var executor = new ManualExecutor();
        var scheduler = new ZLinkApplicationExecutionScheduler(executor, 1);
        var owner = scheduler.register();
        owner.enqueue(() -> DONE, null);
        var pending = owner.enqueue(() -> DONE, null).toCompletableFuture();
        var ready = scheduler.ready;
        ready.addLast(owner);
        assertThrows(
                AssertionError.class,
                () -> assertUnique(new ArrayList<>(ready)),
                "duplicate ready membership mutation must be detected");
        ready.clear();
        executor.drain();
        assertThrows(
                AssertionError.class,
                () -> assertTrue(pending.isDone()),
                "lost ready mutation leaves a runnable record stranded");
        ready.addLast(owner);
        scheduler.register().enqueue(() -> DONE, null);
        executor.drain();
        assertTrue(pending.isDone());
        assertTrue(ready.isEmpty());
    }

    @Test
    void timeBudgetBelongsToSlotAndGivesOtherOwnersAnExecutionOpportunity() throws Exception {
        var executor = new ManualExecutor();
        var scheduler = new ZLinkApplicationExecutionScheduler(executor, 1);
        var first = scheduler.register();
        var second = scheduler.register();
        var events = new ArrayList<String>();
        first.enqueue(
                () -> {
                    // A synchronous record consumes the drain's budget without changing owner
                    // state.
                    long until =
                            System.nanoTime()
                                    + ZLinkSerialExecutionQueue.DEFAULT_OWNER_TIME_BUDGET.toNanos()
                                            * 2;
                    while (System.nanoTime() < until) Thread.onSpinWait();
                    events.add("first");
                    return DONE;
                },
                null);
        first.enqueue(
                () -> {
                    events.add("tail");
                    return DONE;
                },
                null);
        second.enqueue(
                () -> {
                    events.add("other");
                    return DONE;
                },
                null);
        Runnable slot = executor.pending.removeFirst();
        slot.run();
        assertEquals(List.of("first"), events);
        assertSame(
                slot,
                executor.pending.getFirst(),
                "budget resubmits the slot, not a record wrapper");
        executor.drain();
        assertTrue(events.contains("other"));
        assertEquals(3, events.size());
        assertEquals(1, executor.identities.size());
    }

    @Test
    void terminalCleanupRunsOnceForRejectedSubmissionAndSealedRegistry() throws Exception {
        for (boolean sealed : List.of(false, true)) {
            var executor = new ManualExecutor();
            executor.reject = !sealed;
            var registry = new ZLinkChannelDispatchRegistry(executor);
            registry.registerClientServer("test", Map.of(), Map.of());
            if (sealed) registry.sealClosingAdmission();
            var closed = new AtomicInteger();
            var terminal =
                    registry.enqueueChannel(
                                    registry.clientServerQueue("test"),
                                    () -> {
                                        fail("unstarted record must not run");
                                        return DONE;
                                    },
                                    new CompletableFuture<>(),
                                    failure -> closed.incrementAndGet())
                            .toCompletableFuture();
            assertTrue(terminal.isCompletedExceptionally());
            assertEquals(1, closed.get());
            registry.sealClosingAdmission();
            registry.awaitQuiescence().toCompletableFuture().get(2, TimeUnit.SECONDS);
        }
    }

    @Test
    void configuredWorkerCountBoundsSlots() throws Exception {
        try (var executor = Executors.newFixedThreadPool(8)) {
            var scheduler = new ZLinkApplicationExecutionScheduler(executor, 2);
            assertEquals(2, scheduler.idle.size());
        }
    }

    @Test
    void processOwnersShareTheConfiguredExecutionResource() {
        var executor = new ManualExecutor();
        assertSame(
                ZLinkApplicationExecutionScheduler.shared(executor, 2),
                ZLinkApplicationExecutionScheduler.shared(executor, 2));
        assertNotSame(
                ZLinkApplicationExecutionScheduler.shared(executor, 2),
                ZLinkApplicationExecutionScheduler.shared(new ManualExecutor(), 2));
    }

    @Test
    void schedulerMonitorProtectsItsOwnersWithoutBlockingOtherExecutionResources()
            throws Exception {
        var firstExecutor = new ManualExecutor();
        var secondExecutor = new ManualExecutor();
        var first = ZLinkApplicationExecutionScheduler.shared(firstExecutor, 1);
        var second = ZLinkApplicationExecutionScheduler.shared(secondExecutor, 1);
        var firstOwner = first.register();
        var secondOwner = second.register();
        try (var producers = Executors.newFixedThreadPool(2)) {
            Future<?> blocked;
            synchronized (first) {
                var entering = new CountDownLatch(1);
                blocked =
                        producers.submit(
                                () -> {
                                    entering.countDown();
                                    firstOwner.enqueue(() -> DONE, null);
                                });
                assertTrue(entering.await(2, TimeUnit.SECONDS));
                assertThrows(TimeoutException.class, () -> blocked.get(100, TimeUnit.MILLISECONDS));
                producers
                        .submit(
                                () -> {
                                    var terminal = secondOwner.enqueue(() -> DONE, null);
                                    secondOwner.sealClosingAdmission();
                                    secondExecutor.drain();
                                    terminal.toCompletableFuture().join();
                                })
                        .get(2, TimeUnit.SECONDS);
            }
            blocked.get(2, TimeUnit.SECONDS);
            firstExecutor.drain();
            synchronized (ZLinkApplicationExecutionScheduler.SHARED) {
                producers
                        .submit(
                                () -> {
                                    var terminal =
                                            firstOwner.enqueue(
                                                    () -> {
                                                        assertFalse(Thread.holdsLock(first));
                                                        return DONE;
                                                    },
                                                    null);
                                    firstOwner.sealClosingAdmission();
                                    firstExecutor.drain();
                                    terminal.toCompletableFuture().join();
                                })
                        .get(2, TimeUnit.SECONDS);
            }
            assertEquals(1, first.idle.size());
            assertEquals(1, second.idle.size());
        }
    }

    private static ZLinkApplicationJobQueue jobs() {
        return new ZLinkApplicationJobQueue(
                ZLinkApplicationJobQueueProfile.BALANCED,
                OptionalLong.of(2),
                new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null));
    }

    private static final class ManualExecutor extends AbstractExecutorService {
        final ArrayDeque<Runnable> pending = new ArrayDeque<>();
        final Set<Runnable> identities = Collections.newSetFromMap(new IdentityHashMap<>());
        int submissions;
        boolean reject;
        boolean shutdown;

        public void execute(Runnable command) {
            if (reject) throw new RejectedExecutionException("executor stopped");
            submissions++;
            identities.add(command);
            pending.addLast(command);
        }

        void drain() {
            while (!pending.isEmpty()) pending.removeFirst().run();
        }

        public void shutdown() {
            shutdown = true;
        }

        public List<Runnable> shutdownNow() {
            shutdown();
            return List.copyOf(pending);
        }

        public boolean isShutdown() {
            return shutdown;
        }

        public boolean isTerminated() {
            return shutdown && pending.isEmpty();
        }

        public boolean awaitTermination(long timeout, TimeUnit unit) {
            return isTerminated();
        }
    }
}
