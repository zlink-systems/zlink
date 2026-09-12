package systems.zlink.framework.execution;
import java.time.Duration;
import java.util.concurrent.Executors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.ArrayList;
import java.util.List;
import java.util.OptionalLong;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Executor;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.CopyOnWriteArrayList;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;

final class ZLinkSerialExecutionQueueTest {
    @Test
    void firstDrainDoesNotRunOnTheSubmitterStack() throws Exception {
        ZLinkSerialExecutionQueue queue = new ZLinkSerialExecutionQueue(
            Runnable::run, ZLinkExecutionLanePolicy.generic());
        AtomicBoolean ranOnSubmitterStack = new AtomicBoolean();
        CompletableFuture<Void> started = new CompletableFuture<>();
        Thread submitter = Thread.currentThread();

        queue.enqueue(() -> {
            ranOnSubmitterStack.set(Thread.currentThread() == submitter);
            started.complete(null);
            return CompletableFuture.completedFuture(null);
        });

        started.get(3, TimeUnit.SECONDS);
        assertFalse(ranOnSubmitterStack.get());
    }

    @Test
    void synchronousBacklogUsesOneConfiguredExecutorTask() throws Exception {
        AtomicInteger submissions = new AtomicInteger();
        CountDownLatch entered = new CountDownLatch(1);
        CountDownLatch release = new CountDownLatch(1);
        Executor executor = command -> {
            if (submissions.incrementAndGet() == 1) {
                entered.countDown();
                try {
                    assertTrue(release.await(3, TimeUnit.SECONDS));
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    throw new IllegalStateException(interrupted);
                }
            }
            command.run();
        };
        ZLinkSerialExecutionQueue queue = batchQueue(executor, Duration.ofSeconds(10));
        List<Integer> order = new ArrayList<>();
        List<CompletableFuture<Void>> results = new ArrayList<>();
        try {
            for (int index = 0; index < 64; index++) {
                int sequence = index;
                results.add(queue.enqueue(() -> {
                    order.add(sequence);
                    return CompletableFuture.completedFuture(null);
                }).toCompletableFuture());
                if (index == 0) {
                    assertTrue(entered.await(3, TimeUnit.SECONDS));
                }
            }
        } finally {
            release.countDown();
        }
        queue.awaitQuiescence().toCompletableFuture().get(3, TimeUnit.SECONDS);

        assertTrue(results.stream().allMatch(CompletableFuture::isDone));
        assertEquals(java.util.stream.IntStream.range(0, 64).boxed().toList(), order);
        assertEquals(1, submissions.get());
    }

    @Test
    void incompleteStageEndsTheBatchUntilItsGateCompletes() throws Exception {
        CountingExecutor executor = new CountingExecutor();
        ZLinkSerialExecutionQueue queue = batchQueue(executor, Duration.ofSeconds(10));
        CompletableFuture<Void> gate = new CompletableFuture<>();
        CompletableFuture<Void> first = queue.enqueue(() -> gate).toCompletableFuture();
        CompletableFuture<Void> next = queue.enqueue(
            () -> CompletableFuture.completedFuture(null)).toCompletableFuture();

        executor.take().run();
        assertFalse(first.isDone());
        assertFalse(next.isDone());
        assertEquals(1, executor.submissions.get());
        gate.complete(null);
        executor.take().run();

        assertTrue(first.isDone());
        assertTrue(next.isDone());
        assertEquals(2, executor.submissions.get());
    }

    @Test
    void expiredBatchLetsAnotherOwnerUseTheConfiguredExecutor() throws Exception {
        CountingExecutor executor = new CountingExecutor();
        ZLinkSerialExecutionQueue first = batchQueue(executor, Duration.ofNanos(1));
        ZLinkSerialExecutionQueue second = batchQueue(executor, Duration.ofSeconds(10));
        List<String> order = new ArrayList<>();
        first.enqueue(() -> {
            order.add("first-1");
            return CompletableFuture.completedFuture(null);
        });
        Runnable firstBatch = executor.take();
        first.enqueue(() -> {
            order.add("first-2");
            return CompletableFuture.completedFuture(null);
        });
        second.enqueue(() -> {
            order.add("second");
            return CompletableFuture.completedFuture(null);
        });
        Runnable secondOwner = executor.take();

        firstBatch.run();
        assertEquals(List.of("first-1"), order);
        secondOwner.run();
        executor.take().run();

        assertEquals(List.of("first-1", "second", "first-2"), order);
        assertEquals(3, executor.submissions.get());
    }

    @Test
    void oneOwnersSynchronousBatchDoesNotBlockAnotherOwner() throws Exception {
        CountingExecutor executor = new CountingExecutor();
        ZLinkSerialExecutionQueue first = batchQueue(executor, Duration.ofSeconds(10));
        ZLinkSerialExecutionQueue second = batchQueue(executor, Duration.ofSeconds(10));
        CountDownLatch entered = new CountDownLatch(1);
        CountDownLatch release = new CountDownLatch(1);
        CompletableFuture<Void> blocked = first.enqueue(() -> {
            entered.countDown();
            try {
                assertTrue(release.await(3, TimeUnit.SECONDS));
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
                throw new IllegalStateException(interrupted);
            }
            return CompletableFuture.completedFuture(null);
        }).toCompletableFuture();
        Thread worker = Thread.ofVirtual().start(executor.take());
        try {
            assertTrue(entered.await(3, TimeUnit.SECONDS));
            CompletableFuture<Void> independent = second.enqueue(
                () -> CompletableFuture.completedFuture(null)).toCompletableFuture();
            executor.take().run();
            assertTrue(independent.isDone());
            assertFalse(blocked.isDone());
        } finally {
            release.countDown();
            worker.join(3_000);
        }
        blocked.get(3, TimeUnit.SECONDS);
    }

    @Test
    void rejectedBatchReleasesTheClaimAndItsIngressPermit() throws Exception {
        RejectedExecutionException rejection = new RejectedExecutionException("rejected batch");
        AtomicBoolean reject = new AtomicBoolean(true);
        CountingExecutor accepted = new CountingExecutor();
        ZLinkSerialExecutionQueue queue = batchQueue(command -> {
            if (reject.get()) {
                throw rejection;
            }
            accepted.execute(command);
        }, Duration.ofSeconds(10));
        try (ZLinkApplicationJobQueue jobs = new ZLinkApplicationJobQueue(
                 ZLinkApplicationJobQueueProfile.BALANCED, OptionalLong.of(1),
                 new ZLinkApplicationJobQueue.ProcessorCandidates(1, 1, 1, 1))) {
            CompletableFuture<Void> result;
            var permit = jobs.acquireBlocking();
            try (var ignored = ZLinkApplicationJobContext.enter(permit)) {
                result = queue.enqueue(() -> {
                    throw new AssertionError("rejected work must not run");
                }).toCompletableFuture();
            }
            assertEquals(rejection, assertThrows(ExecutionException.class,
                () -> result.get(3, TimeUnit.SECONDS)).getCause());
            queue.awaitQuiescence().toCompletableFuture().get(3, TimeUnit.SECONDS);
            assertEquals(0, jobs.snapshot().permitsInUse());
            reject.set(false);
            CompletableFuture<Void> next = queue.enqueue(
                () -> CompletableFuture.completedFuture(null)).toCompletableFuture();
            accepted.take().run();
            assertTrue(next.isDone());
        }
    }

    @Test
    void lazyRelocationRecordIsNotMaterializedDuringNormalDispatch()
        throws Exception {
        ZLinkSerialExecutionQueue queue = new ZLinkSerialExecutionQueue();
        CompletableFuture<Void> active = new CompletableFuture<>();
        CompletableFuture<Void> started = new CompletableFuture<>();
        AtomicInteger materializations = new AtomicInteger();
        queue.enqueue(() -> {
            started.complete(null);
            return active;
        });
        started.get(3, TimeUnit.SECONDS);
        CompletableFuture<Void> queued = queue.enqueueRelocatableLazyRecord(
            () -> {
                materializations.incrementAndGet();
                return new byte[] {1};
            },
            1L,
            () -> CompletableFuture.completedFuture(null),
            () -> { }).toCompletableFuture();

        active.complete(null);
        queued.get(3, TimeUnit.SECONDS);

        assertEquals(0, materializations.get());
    }

    @Test
    void relocationSealMaterializesLazyRecordExactlyOnce() throws Exception {
        ZLinkSerialExecutionQueue queue = new ZLinkSerialExecutionQueue();
        CompletableFuture<Void> sealNow = new CompletableFuture<>();
        CompletableFuture<Void> activeStarted = new CompletableFuture<>();
        CompletableFuture<ZLinkSerialExecutionQueue.RelocationSeal> sealed =
            new CompletableFuture<>();
        AtomicInteger materializations = new AtomicInteger();
        queue.enqueue(() -> {
            activeStarted.complete(null);
            sealNow.join();
            sealed.complete(queue.trySealRelocation().orElseThrow());
            return CompletableFuture.completedFuture(null);
        });
        activeStarted.get(3, TimeUnit.SECONDS);
        queue.enqueueRelocatableLazyRecord(
            () -> {
                materializations.incrementAndGet();
                return new byte[] {4, 2};
            },
            2L,
            () -> CompletableFuture.completedFuture(null),
            () -> { });

        sealNow.complete(null);
        ZLinkSerialExecutionQueue.RelocationSeal seal =
            sealed.get(3, TimeUnit.SECONDS);

        assertEquals(1, materializations.get());
        assertArrayEquals(new byte[] {4, 2}, seal.captured().getFirst().payload());
        assertTrue(queue.abortRelocation(seal));
        assertEquals(1, materializations.get());
    }

    @Test
    void lifecycleBarrierRunsAfterActiveTurnAndBeforeQueuedApplicationTurns()
        throws Exception {
        ZLinkSerialExecutionQueue queue = new ZLinkSerialExecutionQueue();
        CompletableFuture<Void> activeGate = new CompletableFuture<>();
        CompletableFuture<Void> activeStarted = new CompletableFuture<>();
        List<String> order = new CopyOnWriteArrayList<>();

        queue.enqueue(() -> {
            order.add("active");
            activeStarted.complete(null);
            return activeGate;
        });
        activeStarted.get(3, TimeUnit.SECONDS);
        CompletionStage<Void> queued = queue.enqueue(() -> {
            order.add("queued");
            return CompletableFuture.completedFuture(null);
        });
        CompletionStage<Void> barrier = queue.enqueueBarrierNext(() -> {
            order.add("barrier");
            return CompletableFuture.completedFuture(null);
        });

        activeGate.complete(null);
        CompletableFuture.allOf(
            queued.toCompletableFuture(),
            barrier.toCompletableFuture()).get(3, TimeUnit.SECONDS);

        assertEquals(List.of("active", "barrier", "queued"), order);
    }

    @Test
    void spotWideYieldReleasesSpotGateButRetainsActorClaim() throws Exception {
        ZLinkSerialExecutionQueue actorLane = new ZLinkSerialExecutionQueue();
        ZLinkSerialExecutionQueue spotGate = new ZLinkSerialExecutionQueue();
        CompletableFuture<Void> remote = new CompletableFuture<>();
        CompletableFuture<Void> actorStarted = new CompletableFuture<>();
        CompletableFuture<Void> spotProbe = new CompletableFuture<>();
        CompletableFuture<Void> actorSecond = new CompletableFuture<>();
        List<String> events = new CopyOnWriteArrayList<>();

        CompletableFuture<Void> first = actorLane.enqueue(() ->
            spotGate.enqueue(() -> {
                var execution = new systems.zlink.framework.runtime.internal.handlers
                    .ZLinkSuspendInvocationContext.ApplicationExecution(
                        "room-1", "actor-a", true, true, ignored -> false);
                try (var ignored = systems.zlink.framework.runtime.internal.handlers
                         .ZLinkSuspendInvocationContext.enterApplicationExecution(execution)) {
                    events.add("actor-start");
                    actorStarted.complete(null);
                    return ZLinkSerialExecutionQueue.yieldCurrent(remote)
                        .thenRun(() -> events.add("actor-resume"));
                }
            })).toCompletableFuture();
        actorLane.enqueue(() -> {
            events.add("actor-next");
            actorSecond.complete(null);
            return CompletableFuture.completedFuture(null);
        });

        actorStarted.get(3, TimeUnit.SECONDS);
        spotGate.enqueue(() -> {
            events.add("spot-probe");
            spotProbe.complete(null);
            return CompletableFuture.completedFuture(null);
        });

        spotProbe.get(3, TimeUnit.SECONDS);
        assertFalse(actorSecond.isDone());
        assertEquals(List.of("actor-start", "spot-probe"), events);

        remote.complete(null);
        first.get(3, TimeUnit.SECONDS);
        actorSecond.get(3, TimeUnit.SECONDS);
        assertEquals(
            List.of("actor-start", "spot-probe", "actor-resume", "actor-next"),
            events);
    }

    @Test
    void perActorSpotAndTimerLanesRunIndependentlyAndKeepOwnFifo() throws Exception {
        ZLinkSerialExecutionQueue actorA = new ZLinkSerialExecutionQueue();
        ZLinkSerialExecutionQueue actorB = new ZLinkSerialExecutionQueue();
        ZLinkSerialExecutionQueue spot = new ZLinkSerialExecutionQueue();
        ZLinkSerialExecutionQueue timerA = new ZLinkSerialExecutionQueue();
        ZLinkSerialExecutionQueue timerB = new ZLinkSerialExecutionQueue();
        CompletableFuture<Void> actorAGate = new CompletableFuture<>();
        CompletableFuture<Void> timerAGate = new CompletableFuture<>();
        CompletableFuture<Void> actorAStarted = new CompletableFuture<>();
        CompletableFuture<Void> actorASecond = new CompletableFuture<>();
        CompletableFuture<Void> timerAStarted = new CompletableFuture<>();

        actorA.enqueue(() -> {
            actorAStarted.complete(null);
            return actorAGate;
        });
        actorA.enqueue(() -> {
            actorASecond.complete(null);
            return CompletableFuture.completedFuture(null);
        });
        timerA.enqueue(() -> {
            timerAStarted.complete(null);
            return timerAGate;
        });

        actorAStarted.get(3, TimeUnit.SECONDS);
        timerAStarted.get(3, TimeUnit.SECONDS);
        CompletableFuture.allOf(
            actorB.enqueue(() -> CompletableFuture.completedFuture(null))
                .toCompletableFuture(),
            spot.enqueue(() -> CompletableFuture.completedFuture(null))
                .toCompletableFuture(),
            timerB.enqueue(() -> CompletableFuture.completedFuture(null))
                .toCompletableFuture()).get(3, TimeUnit.SECONDS);
        assertFalse(actorASecond.isDone());

        actorAGate.complete(null);
        actorASecond.get(3, TimeUnit.SECONDS);
        timerAGate.complete(null);
    }

    @Test
    void submitKeepsTurnUntilIncompleteStageCompletes() throws Exception {
        ZLinkSerialExecutionQueue queue = new ZLinkSerialExecutionQueue();
        CompletableFuture<Void> firstGate = new CompletableFuture<>();
        CompletableFuture<Void> firstStarted = new CompletableFuture<>();
        List<String> events = new ArrayList<>();

        CompletableFuture<Void> first = queue.enqueue(() -> {
            events.add("first-start");
            firstStarted.complete(null);
            return firstGate
                .thenRun(() -> events.add("first-complete"));
        }).toCompletableFuture();
        CompletableFuture<Void> second = queue.enqueue(() -> {
            events.add("second-start");
            return CompletableFuture.completedFuture(null);
        }).toCompletableFuture();

        firstStarted.get(3, TimeUnit.SECONDS);
        assertFalse(second.isDone());
        assertEquals(List.of("first-start"), events);
        assertFalse(first.isDone());

        firstGate.complete(null);

        first.get(3, TimeUnit.SECONDS);
        second.get(3, TimeUnit.SECONDS);
        assertEquals(List.of("first-start", "first-complete", "second-start"), events);
    }

    @Test
    void ownerQueueAcceptsRecordsWhileAnEarlierTurnIsRunning()
        throws Exception {
        ZLinkSerialExecutionQueue queue = new ZLinkSerialExecutionQueue(
            null,
            ZLinkExecutionLanePolicy.generic(),
            2,
            Duration.ofSeconds(1));
        CompletableFuture<Void> active = new CompletableFuture<>();

        queue.enqueueRelocatable(new byte[6], () -> active)
            .toCompletableFuture();
        assertTrue(queue.tryEnqueueRelocatable(
            new byte[1],
            () -> CompletableFuture.completedFuture(null)));

        active.complete(null);
        queue.awaitQuiescence().toCompletableFuture()
            .get(3, TimeUnit.SECONDS);
        assertTrue(queue.tryEnqueueRelocatable(
            new byte[1],
            () -> CompletableFuture.completedFuture(null)));
    }

    @Test
    void relocationAcceptsLargePayloadHints() throws Exception {
        ZLinkSerialExecutionQueue queue = new ZLinkSerialExecutionQueue(
            null,
            ZLinkExecutionLanePolicy.generic(),
            2,
            Duration.ofSeconds(1));
        ZLinkSerialExecutionQueue.RelocationSeal seal =
            queue.trySealRelocation().orElseThrow();

        CompletableFuture<Void> first = queue.enqueueWithPayloadBytes(
            Long.MAX_VALUE - 2,
            () -> CompletableFuture.completedFuture(null)).toCompletableFuture();
        CompletableFuture<Void> lastRepresentable = queue.enqueue(
            () -> CompletableFuture.completedFuture(null)).toCompletableFuture();

        assertTrue(queue.tryEnqueue(
            () -> CompletableFuture.completedFuture(null)));
        assertFalse(queue.enqueue(
            () -> CompletableFuture.completedFuture(null)).toCompletableFuture()
            .isCompletedExceptionally());

        queue.commitRelocation(seal).orElseThrow();
        first.get(3, TimeUnit.SECONDS);
        lastRepresentable.get(3, TimeUnit.SECONDS);
    }

    @Test
    void ownerQueueAcceptsPayloadAndEmptyTurns()
        throws Exception {
        ZLinkSerialExecutionQueue queue = new ZLinkSerialExecutionQueue(
            null,
            ZLinkExecutionLanePolicy.generic(),
            2,
            Duration.ofSeconds(1));
        CompletableFuture<Void> active = new CompletableFuture<>();

        queue.enqueueWithPayloadBytes(6, () -> active);
        assertTrue(queue.tryEnqueueWithPayloadBytes(
            0,
            () -> CompletableFuture.completedFuture(null)));

        active.complete(null);
        queue.awaitQuiescence().toCompletableFuture()
            .get(3, TimeUnit.SECONDS);
        assertTrue(queue.tryEnqueueWithPayloadBytes(
            0,
            () -> CompletableFuture.completedFuture(null)));
    }

    @Test
    void lifecycleLaneRemainsSeparateWithoutAnAdmissionCap()
        throws Exception {
        ZLinkSerialExecutionQueue queue = new ZLinkSerialExecutionQueue(
            null,
            ZLinkExecutionLanePolicy.generic(),
            2,
            Duration.ofSeconds(1));
        CompletableFuture<Void> active = new CompletableFuture<>();
        CompletableFuture<Void> activeStarted = new CompletableFuture<>();
        queue.enqueue(() -> {
            activeStarted.complete(null);
            return active;
        });
        activeStarted.get(3, TimeUnit.SECONDS);

        CompletableFuture<Void> firstBarrier = queue.enqueueBarrierNext(
            () -> CompletableFuture.completedFuture(null))
            .toCompletableFuture();
        assertFalse(firstBarrier.isCompletedExceptionally());
        assertFalse(queue.enqueueBarrierNext(
            () -> CompletableFuture.completedFuture(null))
            .toCompletableFuture().isCompletedExceptionally());
        active.complete(null);
        firstBarrier.get(3, TimeUnit.SECONDS);
    }

    @Test
    void lifecycleBurstYieldsToApplicationLane() throws Exception {
        ZLinkSerialExecutionQueue queue = new ZLinkSerialExecutionQueue(
            null,
            ZLinkExecutionLanePolicy.generic(),
            2,
            Duration.ofSeconds(1));
        CompletableFuture<Void> active = new CompletableFuture<>();
        CompletableFuture<Void> started = new CompletableFuture<>();
        List<String> order = new CopyOnWriteArrayList<>();
        queue.enqueue(() -> {
            started.complete(null);
            return active;
        });
        started.get(3, TimeUnit.SECONDS);
        queue.enqueueLifecycleBarrier(() -> {
            order.add("lifecycle-1");
            return CompletableFuture.completedFuture(null);
        });
        queue.enqueueLifecycleBarrier(() -> {
            order.add("lifecycle-2");
            return CompletableFuture.completedFuture(null);
        });
        queue.enqueueLifecycleBarrier(() -> {
            order.add("lifecycle-3");
            return CompletableFuture.completedFuture(null);
        });
        queue.enqueue(() -> {
            order.add("application");
            return CompletableFuture.completedFuture(null);
        });

        active.complete(null);
        queue.awaitQuiescence().toCompletableFuture()
            .get(3, TimeUnit.SECONDS);
        assertEquals(
            List.of("lifecycle-1", "lifecycle-2", "application", "lifecycle-3"),
            order);
    }

    @Test
    void yieldReleasesWaitingTurnAndReentersContinuationInQueueOrder() throws Exception {
        ZLinkSerialExecutionQueue queue = new ZLinkSerialExecutionQueue();
        CompletableFuture<Void> firstGate = new CompletableFuture<>();
        CompletableFuture<Void> firstStarted = new CompletableFuture<>();
        List<String> events = new ArrayList<>();

        CompletableFuture<Void> first = queue.enqueue(() -> {
            events.add("first-start");
            firstStarted.complete(null);
            return ZLinkSerialExecutionQueue.yieldCurrent(firstGate)
                .thenRun(() -> events.add("first-complete"));
        }).toCompletableFuture();
        CompletableFuture<Void> second = queue.enqueue(() -> {
            events.add("second-start");
            return CompletableFuture.completedFuture(null);
        }).toCompletableFuture();

        firstStarted.get(3, TimeUnit.SECONDS);
        second.get(3, TimeUnit.SECONDS);
        assertEquals(List.of("first-start", "second-start"), events);
        assertFalse(first.isDone());

        firstGate.complete(null);

        first.get(3, TimeUnit.SECONDS);
        assertEquals(List.of("first-start", "second-start", "first-complete"), events);
    }

    @Test
    void releasedTurnIsNotCurrentUntilItsContinuationReenters() throws Exception {
        ZLinkSerialExecutionQueue queue = new ZLinkSerialExecutionQueue();
        CompletableFuture<Void> firstRemote = new CompletableFuture<>();
        CompletableFuture<Void> secondRemote = new CompletableFuture<>();
        CompletableFuture<Void> started = new CompletableFuture<>();
        AtomicBoolean currentAfterRelease = new AtomicBoolean(true);
        AtomicBoolean secondYieldWasManaged = new AtomicBoolean();
        AtomicBoolean continuationIsCurrent = new AtomicBoolean();

        CompletableFuture<Void> dispatch = queue.enqueue(() -> {
            CompletionStage<Void> firstYield =
                ZLinkSerialExecutionQueue.yieldCurrent(firstRemote);
            currentAfterRelease.set(queue.isCurrent());
            CompletionStage<Void> secondYield =
                ZLinkSerialExecutionQueue.yieldCurrent(secondRemote);
            secondYieldWasManaged.set(secondYield != secondRemote);
            started.complete(null);
            return firstYield.thenRun(() ->
                continuationIsCurrent.set(queue.isCurrent()));
        }).toCompletableFuture();

        started.get(3, TimeUnit.SECONDS);
        firstRemote.complete(null);
        dispatch.get(3, TimeUnit.SECONDS);
        secondRemote.complete(null);
        queue.awaitQuiescence().toCompletableFuture()
            .get(3, TimeUnit.SECONDS);

        assertFalse(currentAfterRelease.get());
        assertTrue(secondYieldWasManaged.get());
        assertTrue(continuationIsCurrent.get());
    }

    @Test
    void yieldRetainsTurnContextAcrossHandlerExecutor() throws Exception {
        ZLinkSerialExecutionQueue queue = new ZLinkSerialExecutionQueue();
        CompletableFuture<Void> remote = new CompletableFuture<>();
        CompletableFuture<Void> handlerStarted = new CompletableFuture<>();
        CompletableFuture<Void> probeStarted = new CompletableFuture<>();
        try (var handlerExecutor = Executors.newSingleThreadExecutor()) {
            CompletableFuture<Void> first = queue.enqueue(() -> {
                CompletableFuture<Void> result = new CompletableFuture<>();
                ZLinkSerialExecutionQueue.propagateCurrent(handlerExecutor).execute(() -> {
                    handlerStarted.complete(null);
                    ZLinkSerialExecutionQueue.yieldCurrent(remote)
                        .whenComplete((ignored, error) -> result.complete(null));
                });
                return result;
            }).toCompletableFuture();
            queue.enqueue(() -> {
                probeStarted.complete(null);
                return CompletableFuture.completedFuture(null);
            });

            handlerStarted.get(3, TimeUnit.SECONDS);
            probeStarted.get(3, TimeUnit.SECONDS);
            assertFalse(first.isDone());
            remote.complete(null);
            first.get(3, TimeUnit.SECONDS);
        }
    }

    @Test
    void continuesAfterPreviousFailure() {
        ZLinkSerialExecutionQueue queue = new ZLinkSerialExecutionQueue();
        List<String> events = new ArrayList<>();

        queue.enqueue(() -> {
            events.add("first-start");
            return CompletableFuture.failedFuture(new IllegalStateException("boom"));
        });
        queue.enqueue(() -> {
            events.add("second-start");
            return CompletableFuture.completedFuture(null);
        }).toCompletableFuture().join();

        assertEquals(List.of("first-start", "second-start"), events);
    }

    @Test
    void reentersManagedContinuationWithItsCapturedFlow() throws Exception {
        ZLinkSerialExecutionQueue queue = new ZLinkSerialExecutionQueue();
        CompletableFuture<Void> gate = new CompletableFuture<>();
        CompletableFuture<Void> started = new CompletableFuture<>();
        CompletableFuture<String> observed = new CompletableFuture<>();
        ZLinkFlowContext.State flow = ZLinkFlowContext.create(ZLinkFlowOrigin.INBOUND);

        queue.enqueue(() -> {
            try (ZLinkFlowContext.Scope ignored = ZLinkFlowContext.enter(flow)) {
                started.complete(null);
                return ZLinkSerialExecutionQueue.yieldCurrent(gate)
                    .thenRun(() -> observed.complete(ZLinkFlowContext.current().flowId()));
            }
        });

        started.get(3, TimeUnit.SECONDS);
        gate.complete(null);

        assertEquals(flow.flowId(), observed.get(3, TimeUnit.SECONDS));
    }

    @Test
    void startsQueuedOperationWithFlowCapturedAtEnqueue() throws Exception {
        ZLinkSerialExecutionQueue queue = new ZLinkSerialExecutionQueue();
        CompletableFuture<String> observed = new CompletableFuture<>();
        ZLinkFlowContext.State flow = ZLinkFlowContext.create(ZLinkFlowOrigin.INBOUND);

        try (ZLinkFlowContext.Scope ignored = ZLinkFlowContext.enter(flow)) {
            queue.enqueue(() -> {
                observed.complete(ZLinkFlowContext.current().flowId());
                return CompletableFuture.completedFuture(null);
            });
        }

        assertEquals(flow.flowId(), observed.get(3, TimeUnit.SECONDS));
    }

    @Test
    void retainedTurnContinuationCanExplicitlyYield() throws Exception {
        ZLinkSerialExecutionQueue queue = new ZLinkSerialExecutionQueue();
        CompletableFuture<Void> remote = new CompletableFuture<>();
        CompletableFuture<Void> afterYield = new CompletableFuture<>();
        CompletableFuture<Void> firstStarted = new CompletableFuture<>();
        CompletableFuture<Void> secondStarted = new CompletableFuture<>();

        CompletableFuture<Void> first = queue.enqueue(() -> {
            firstStarted.complete(null);
            return ZLinkSerialExecutionQueue.manageCurrent(remote)
                .thenCompose(ignored -> ZLinkSerialExecutionQueue.yieldCurrent(afterYield));
        }).toCompletableFuture();
        queue.enqueue(() -> {
            secondStarted.complete(null);
            return CompletableFuture.completedFuture(null);
        });

        firstStarted.get(3, TimeUnit.SECONDS);
        CompletableFuture.runAsync(() -> remote.complete(null)).join();
        secondStarted.get(3, TimeUnit.SECONDS);
        assertFalse(first.isDone());
        afterYield.complete(null);
        first.get(3, TimeUnit.SECONDS);
    }

    @Test
    void relocationSealHoldsIngressAndAbortRestoresArrivalOrder()
        throws Exception {
        ZLinkSerialExecutionQueue queue = new ZLinkSerialExecutionQueue();
        CompletableFuture<Void> sealNow = new CompletableFuture<>();
        CompletableFuture<Void> intentStarted = new CompletableFuture<>();
        CompletableFuture<ZLinkSerialExecutionQueue.RelocationSeal> sealed =
            new CompletableFuture<>();
        List<String> handled =
            new CopyOnWriteArrayList<>();

        queue.enqueue(() -> {
            intentStarted.complete(null);
            sealNow.join();
            sealed.complete(queue.trySealRelocation().orElseThrow());
            return CompletableFuture.completedFuture(null);
        });
        intentStarted.get(3, TimeUnit.SECONDS);
        queue.enqueueRelocatable(
            new byte[] {1},
            () -> {
                handled.add("one");
                return CompletableFuture.completedFuture(null);
            });
        queue.enqueueRelocatable(
            new byte[] {2},
            () -> {
                handled.add("two");
                return CompletableFuture.completedFuture(null);
        });
        sealNow.complete(null);
        ZLinkSerialExecutionQueue.RelocationSeal seal =
            sealed.get(3, TimeUnit.SECONDS);

        queue.enqueueRelocatable(
            new byte[] {3},
            () -> {
                handled.add("three");
                return CompletableFuture.completedFuture(null);
            });
        CompletableFuture<Void> infrastructure = queue.enqueue(() -> {
            handled.add("infrastructure");
            return CompletableFuture.completedFuture(null);
        }).toCompletableFuture();
        assertFalse(infrastructure.isDone());
        assertEquals(List.of(), handled);
        assertEquals(2, seal.captured().size());

        assertTrue(queue.abortRelocation(seal));
        waitForSize(handled, 4);
        assertEquals(
            List.of("one", "two", "three", "infrastructure"),
            handled);
    }

    @Test
    void relocationCommitReturnsOnlyHeldIngressAndRejectsNewOwnerWork()
        throws Exception {
        ZLinkSerialExecutionQueue queue = new ZLinkSerialExecutionQueue();
        CompletableFuture<Void> sealNow = new CompletableFuture<>();
        CompletableFuture<Void> intentStarted = new CompletableFuture<>();
        CompletableFuture<ZLinkSerialExecutionQueue.RelocationSeal> sealed =
            new CompletableFuture<>();
        AtomicReference<Boolean> ran = new AtomicReference<>(false);

        queue.enqueue(() -> {
            intentStarted.complete(null);
            sealNow.join();
            sealed.complete(queue.trySealRelocation().orElseThrow());
            return CompletableFuture.completedFuture(null);
        });
        intentStarted.get(3, TimeUnit.SECONDS);
        CompletableFuture<Void> captured = queue.enqueueRelocatable(
            new byte[] {1},
            () -> {
                ran.set(true);
                return CompletableFuture.completedFuture(null);
        }).toCompletableFuture();
        sealNow.complete(null);
        ZLinkSerialExecutionQueue.RelocationSeal seal =
            sealed.get(3, TimeUnit.SECONDS);
        CompletableFuture<Void> held = queue.enqueueRelocatable(
            new byte[] {2},
            () -> {
                ran.set(true);
                return CompletableFuture.completedFuture(null);
            }).toCompletableFuture();

        List<ZLinkSerialExecutionQueue.QueuedRecord> relay =
            queue.commitRelocation(seal).orElseThrow();
        captured.get(3, TimeUnit.SECONDS);
        held.get(3, TimeUnit.SECONDS);

        assertFalse(ran.get());
        assertEquals(1, relay.size());
        assertArrayEquals(new byte[] {2}, relay.getFirst().payload());
        assertTrue(queue.enqueueRelocatable(
            new byte[] {3},
            () -> CompletableFuture.completedFuture(null))
            .toCompletableFuture()
            .isCompletedExceptionally());
        assertTrue(queue.enqueue(
            () -> CompletableFuture.completedFuture(null))
            .toCompletableFuture()
            .isCompletedExceptionally());
    }

    @Test
    void relocationCommitReleasesSourceResourcesAndFencesSourceOwner()
        throws Exception {
        ZLinkSerialExecutionQueue queue = new ZLinkSerialExecutionQueue();
        AtomicInteger releases = new AtomicInteger();
        AtomicReference<Boolean> ran = new AtomicReference<>(false);
        ZLinkSerialExecutionQueue.RelocationSeal seal =
            queue.trySealRelocation().orElseThrow();
        CompletableFuture<Void> held = queue.enqueueRelocatable(
            new byte[] {7},
            () -> {
                ran.set(true);
                return CompletableFuture.completedFuture(null);
            },
            releases::incrementAndGet).toCompletableFuture();

        assertEquals(1, queue.commitRelocation(seal).orElseThrow().size());
        held.get(3, TimeUnit.SECONDS);
        assertEquals(1, releases.get());
        assertFalse(ran.get());
        ExecutionException sourceFenced = assertThrows(
            ExecutionException.class,
            () -> queue.enqueue(() -> CompletableFuture.completedFuture(null))
                .toCompletableFuture().get(3, TimeUnit.SECONDS));
        assertInstanceOf(IllegalStateException.class, sourceFenced.getCause());
        assertEquals(1, releases.get());
    }

    @Test
    void relocationIngressContinuesHoldingAfterFreezeUntilTargetAck()
        throws Exception {
        ZLinkSerialExecutionQueue queue = new ZLinkSerialExecutionQueue();
        ZLinkSerialExecutionQueue.RelocationSeal seal =
            queue.trySealRelocation().orElseThrow();
        CompletableFuture<Void> held = queue.enqueueRelocatable(
            new byte[] {7},
            () -> CompletableFuture.completedFuture(null))
            .toCompletableFuture();

        var frozen = queue.freezeRelocationIngress(seal).orElseThrow();
        assertEquals(1, frozen.size());
        assertArrayEquals(new byte[] {7}, frozen.getFirst().payload());
        CompletableFuture<Void> suffix = queue.enqueueRelocatable(
            new byte[] {8, 8, 8, 8, 8, 8, 8, 8},
            () -> CompletableFuture.completedFuture(null))
            .toCompletableFuture();

        systems.zlink.framework.runtime.internal.relocation
            .ZLinkRetainedSerialQueueCommit.Commit commit =
            systems.zlink.framework.runtime.internal.relocation
                .ZLinkRetainedSerialQueueCommit.retain(queue, seal)
                .orElseThrow();
        assertEquals(2, commit.records().size());
        assertFalse(held.isDone());
        assertFalse(suffix.isDone());
        var firstCut = commit.cut();
        CompletableFuture<Void> late = queue.enqueueRelocatable(
            new byte[] {9},
            () -> CompletableFuture.completedFuture(null))
            .toCompletableFuture();
        assertFalse(commit.tryEstablishDurableCut(firstCut));
        var durableCut = commit.cut();
        assertEquals(3, durableCut.records().size());
        assertTrue(commit.tryEstablishDurableCut(durableCut));
        CompletableFuture<Void> duringActivation = queue.enqueueRelocatable(
            new byte[] {10},
            () -> CompletableFuture.completedFuture(null))
            .toCompletableFuture();
        assertFalse(commit.tryFinishCapture(durableCut));
        var finalCut = commit.cut();
        assertEquals(4, finalCut.records().size());
        assertTrue(commit.tryFinishCapture(finalCut));
        commit.complete();
        held.get(3, TimeUnit.SECONDS);
        suffix.get(3, TimeUnit.SECONDS);
        late.get(3, TimeUnit.SECONDS);
        duringActivation.get(3, TimeUnit.SECONDS);
    }

    @Test
    void relocationHoldReleasesRecordsAndMakesPostReleaseProgress()
        throws Exception {
        ZLinkSerialExecutionQueue queue = new ZLinkSerialExecutionQueue();
        ZLinkSerialExecutionQueue.RelocationSeal seal =
            queue.trySealRelocation().orElseThrow();
        List<String> handled = new CopyOnWriteArrayList<>();

        CompletableFuture<Void> first = queue.enqueueRelocatable(
            new byte[] {1, 1},
            () -> {
                handled.add("first");
                return CompletableFuture.completedFuture(null);
            }).toCompletableFuture();
        assertEquals(1, queue.freezeRelocationIngress(seal)
            .orElseThrow().size());
        CompletableFuture<Void> second = queue.enqueueRelocatableLazyRecord(
            () -> new byte[] {2},
            1L,
            () -> {
                handled.add("second");
                return CompletableFuture.completedFuture(null);
            },
            () -> { }).toCompletableFuture();
        CompletableFuture<Void> third = queue.enqueue(
            () -> {
                handled.add("third");
                return CompletableFuture.completedFuture(null);
            }).toCompletableFuture();
        CompletableFuture<Void> fourth = queue.enqueueRelocatable(
            new byte[] {4, 4},
            () -> {
                handled.add("fourth");
                return CompletableFuture.completedFuture(null);
            }).toCompletableFuture();

        assertTrue(queue.abortRelocation(seal));
        CompletableFuture.allOf(first, second, third, fourth)
            .get(3, TimeUnit.SECONDS);
        waitForSize(handled, 4);
        assertEquals(
            List.of("first", "second", "third", "fourth"),
            handled);
    }

    @Test
    void relocationSealWaitsForYieldedContinuationToQuiesce()
        throws Exception {
        ZLinkSerialExecutionQueue queue = new ZLinkSerialExecutionQueue();
        CompletableFuture<Void> remote = new CompletableFuture<>();
        CompletableFuture<Void> yieldRegistered = new CompletableFuture<>();
        CompletableFuture<Void> continuationFinished =
            new CompletableFuture<>();

        CompletableFuture<Void> dispatch = queue.enqueue(() -> {
            CompletionStage<Void> yielded =
                ZLinkSerialExecutionQueue.yieldCurrent(remote);
            yieldRegistered.complete(null);
            return yielded.thenRun(() -> continuationFinished.complete(null));
        })
            .toCompletableFuture();

        yieldRegistered.get(3, TimeUnit.SECONDS);
        assertTrue(queue.trySealRelocation().isEmpty());
        remote.complete(null);
        continuationFinished.get(3, TimeUnit.SECONDS);
        dispatch.get(3, TimeUnit.SECONDS);
        queue.awaitQuiescence().toCompletableFuture()
            .get(3, TimeUnit.SECONDS);

        assertTrue(queue.trySealRelocation().isPresent());
    }

    @Test
    void quiescenceBarrierWaitsForYieldedTerminalContinuation()
        throws Exception {
        ZLinkSerialExecutionQueue queue = new ZLinkSerialExecutionQueue();
        CompletableFuture<Void> remote = new CompletableFuture<>();
        CompletableFuture<Void> yielded = new CompletableFuture<>();

        CompletableFuture<Void> dispatch = queue.enqueue(() -> {
            CompletionStage<Void> continuation =
                ZLinkSerialExecutionQueue.yieldCurrent(remote);
            yielded.complete(null);
            return continuation;
        }).toCompletableFuture();

        yielded.get(3, TimeUnit.SECONDS);
        CompletableFuture<Void> barrier =
            queue.awaitQuiescence().toCompletableFuture();
        assertFalse(barrier.isDone());

        remote.complete(null);
        dispatch.get(3, TimeUnit.SECONDS);
        barrier.get(3, TimeUnit.SECONDS);
    }

    @Test
    void quiescenceBarrierWaitsForEveryAcceptedTurn() throws Exception {
        ZLinkSerialExecutionQueue queue = new ZLinkSerialExecutionQueue();
        CompletableFuture<Void> active = new CompletableFuture<>();
        CompletableFuture<Void> started = new CompletableFuture<>();

        queue.enqueue(() -> {
            started.complete(null);
            return active;
        });
        CompletableFuture<Void> queued = queue.enqueue(
            () -> CompletableFuture.completedFuture(null))
            .toCompletableFuture();

        started.get(3, TimeUnit.SECONDS);
        CompletableFuture<Void> barrier =
            queue.awaitQuiescence().toCompletableFuture();
        assertFalse(barrier.isDone());

        active.complete(null);
        queued.get(3, TimeUnit.SECONDS);
        barrier.get(3, TimeUnit.SECONDS);
    }

    @Test
    void queuedRelocationIntentCannotRacePastYieldRegistration()
        throws Exception {
        ZLinkSerialExecutionQueue queue = new ZLinkSerialExecutionQueue();
        CompletableFuture<Void> remote = new CompletableFuture<>();
        CompletableFuture<Boolean> sealed =
            new CompletableFuture<>();

        CompletableFuture<Void> dispatch = queue.enqueue(() ->
            ZLinkSerialExecutionQueue.yieldCurrent(remote))
            .toCompletableFuture();
        queue.enqueue(() -> {
            sealed.complete(queue.trySealRelocation().isPresent());
            return CompletableFuture.completedFuture(null);
        });

        assertFalse(sealed.get(3, TimeUnit.SECONDS));
        remote.complete(null);
        dispatch.get(3, TimeUnit.SECONDS);
        queue.awaitQuiescence()
            .toCompletableFuture()
            .get(3, TimeUnit.SECONDS);
        assertTrue(queue.trySealRelocation().isPresent());
    }

    @Test
    void relocationAbortRequiresTheExactSealReferenceAndGeneration()
        throws Exception {
        ZLinkSerialExecutionQueue queue = new ZLinkSerialExecutionQueue();
        ZLinkSerialExecutionQueue.RelocationSeal first =
            queue.trySealRelocation().orElseThrow();
        var forged = new ZLinkSerialExecutionQueue.RelocationSeal(
            first.serial(),
            first.captured());

        assertFalse(queue.abortRelocation(forged));
        assertTrue(queue.abortRelocation(first));

        ZLinkSerialExecutionQueue.RelocationSeal second =
            queue.trySealRelocation().orElseThrow();
        assertFalse(queue.abortRelocation(first));
        assertTrue(queue.abortRelocation(second));
    }

    private static ZLinkSerialExecutionQueue batchQueue(
        Executor executor, Duration budget) {
        return new ZLinkSerialExecutionQueue(
            executor, ZLinkExecutionLanePolicy.generic(),
            ZLinkSerialExecutionQueue.DEFAULT_LIFECYCLE_BURST_LIMIT,
            budget);
    }

    private static final class CountingExecutor implements Executor {
        private final LinkedBlockingQueue<Runnable> pending = new LinkedBlockingQueue<>();
        private final AtomicInteger submissions = new AtomicInteger();

        @Override
        public void execute(Runnable command) {
            submissions.incrementAndGet();
            pending.add(command);
        }

        private Runnable take() throws InterruptedException {
            Runnable task = pending.poll(3, TimeUnit.SECONDS);
            assertTrue(task != null, "the configured executor must receive a drain task");
            return task;
        }
    }

    private static void waitForSize(
        List<String> values,
        int expected) throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(3);
        while (values.size() < expected && System.nanoTime() < deadline) {
            Thread.sleep(1);
        }
        assertEquals(expected, values.size());
    }
}
