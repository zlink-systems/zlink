package systems.zlink.framework.execution;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CompletionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.CopyOnWriteArrayList;
import java.time.Duration;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;

final class ZLinkAsyncSerialQueueTest {
    @Test
    void lifecycleBarrierRunsAfterActiveTurnAndBeforeQueuedApplicationTurns()
        throws Exception {
        ZLinkAsyncSerialQueue queue = new ZLinkAsyncSerialQueue();
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
        ZLinkAsyncSerialQueue actorLane = new ZLinkAsyncSerialQueue();
        ZLinkAsyncSerialQueue spotGate = new ZLinkAsyncSerialQueue();
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
                    return ZLinkAsyncSerialQueue.yieldCurrent(remote)
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
        ZLinkAsyncSerialQueue actorA = new ZLinkAsyncSerialQueue();
        ZLinkAsyncSerialQueue actorB = new ZLinkAsyncSerialQueue();
        ZLinkAsyncSerialQueue spot = new ZLinkAsyncSerialQueue();
        ZLinkAsyncSerialQueue timerA = new ZLinkAsyncSerialQueue();
        ZLinkAsyncSerialQueue timerB = new ZLinkAsyncSerialQueue();
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
        ZLinkAsyncSerialQueue queue = new ZLinkAsyncSerialQueue();
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
    void boundedQueueSignalsWhenOnePendingSlotBecomesAvailable() throws Exception {
        ZLinkAsyncSerialQueue queue = new ZLinkAsyncSerialQueue(false, 1);
        CompletableFuture<Void> firstGate = new CompletableFuture<>();
        CompletableFuture<Void> firstStarted = new CompletableFuture<>();
        CompletableFuture<Void> capacityAvailable = new CompletableFuture<>();
        queue.onCapacityAvailable(() -> capacityAvailable.complete(null));

        assertTrue(queue.tryEnqueue(() -> {
            firstStarted.complete(null);
            return firstGate;
        }));
        firstStarted.get(3, TimeUnit.SECONDS);
        assertFalse(queue.tryEnqueue(() -> CompletableFuture.completedFuture(null)));

        firstGate.complete(null);

        capacityAvailable.get(3, TimeUnit.SECONDS);
        assertTrue(queue.tryEnqueue(() -> CompletableFuture.completedFuture(null)));
    }

    @Test
    void byteBudgetRejectsLargeApplicationRecordAndReturnsCapacityAfterCompletion()
        throws Exception {
        ZLinkAsyncSerialQueue queue = new ZLinkAsyncSerialQueue(
            null,
            false,
            4,
            10,
            4,
            10,
            4,
            2,
            Duration.ofSeconds(1));
        CompletableFuture<Void> active = new CompletableFuture<>();

        queue.enqueueRelocatable(new byte[6], () -> active)
            .toCompletableFuture();
        assertFalse(queue.tryEnqueueRelocatable(
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
    void regularApplicationTurnsChargePayloadBytesAndEmptyTurnsUseCountCapacity()
        throws Exception {
        ZLinkAsyncSerialQueue queue = new ZLinkAsyncSerialQueue(
            null,
            false,
            2,
            10,
            2,
            10,
            4,
            2,
            Duration.ofSeconds(1));
        CompletableFuture<Void> active = new CompletableFuture<>();

        queue.enqueueWithPayloadBytes(6, () -> active);
        assertFalse(queue.tryEnqueueWithPayloadBytes(
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
    void unrepresentablePayloadCostCompletesWithCapacityExceeded() {
        ZLinkAsyncSerialQueue queue = new ZLinkAsyncSerialQueue(
            null,
            false,
            2,
            Long.MAX_VALUE,
            2,
            Long.MAX_VALUE,
            4,
            2,
            Duration.ofSeconds(1));

        CompletionException failure = assertThrows(
            CompletionException.class,
            () -> queue.enqueueWithPayloadBytes(
                Long.MAX_VALUE,
                () -> CompletableFuture.completedFuture(null))
                .toCompletableFuture()
                .join());
        ZLinkFrameworkException error = assertInstanceOf(
            ZLinkFrameworkException.class,
            failure.getCause());
        assertEquals(ZLinkFrameworkErrorKind.CAPACITY_EXCEEDED, error.kind());
        assertFalse(queue.tryEnqueueWithPayloadBytes(
            Long.MAX_VALUE,
            () -> CompletableFuture.completedFuture(null)));
    }

    @Test
    void lifecycleCapacityIsSeparateAndDoesNotBypassItsOwnLimit() {
        ZLinkAsyncSerialQueue queue = new ZLinkAsyncSerialQueue(
            null,
            false,
            4,
            64,
            1,
            4,
            4,
            2,
            Duration.ofSeconds(1));
        CompletableFuture<Void> active = new CompletableFuture<>();
        queue.enqueue(() -> active);

        assertFalse(queue.enqueueBarrierNext(
            () -> CompletableFuture.completedFuture(null))
            .toCompletableFuture().isCompletedExceptionally());
        assertTrue(queue.enqueueBarrierNext(
            () -> CompletableFuture.completedFuture(null))
            .toCompletableFuture().isCompletedExceptionally());
        active.complete(null);
    }

    @Test
    void lifecycleBurstYieldsToApplicationLane() throws Exception {
        ZLinkAsyncSerialQueue queue = new ZLinkAsyncSerialQueue(
            null,
            false,
            8,
            128,
            8,
            128,
            4,
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
        ZLinkAsyncSerialQueue queue = new ZLinkAsyncSerialQueue();
        CompletableFuture<Void> firstGate = new CompletableFuture<>();
        CompletableFuture<Void> firstStarted = new CompletableFuture<>();
        List<String> events = new ArrayList<>();

        CompletableFuture<Void> first = queue.enqueue(() -> {
            events.add("first-start");
            firstStarted.complete(null);
            return ZLinkAsyncSerialQueue.yieldCurrent(firstGate)
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
    void yieldRetainsTurnContextAcrossHandlerExecutor() throws Exception {
        ZLinkAsyncSerialQueue queue = new ZLinkAsyncSerialQueue();
        CompletableFuture<Void> remote = new CompletableFuture<>();
        CompletableFuture<Void> handlerStarted = new CompletableFuture<>();
        CompletableFuture<Void> probeStarted = new CompletableFuture<>();
        try (var handlerExecutor = java.util.concurrent.Executors.newSingleThreadExecutor()) {
            CompletableFuture<Void> first = queue.enqueue(() -> {
                CompletableFuture<Void> result = new CompletableFuture<>();
                ZLinkAsyncSerialQueue.propagateCurrent(handlerExecutor).execute(() -> {
                    handlerStarted.complete(null);
                    ZLinkAsyncSerialQueue.yieldCurrent(remote)
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
        ZLinkAsyncSerialQueue queue = new ZLinkAsyncSerialQueue();
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
        ZLinkAsyncSerialQueue queue = new ZLinkAsyncSerialQueue();
        CompletableFuture<Void> gate = new CompletableFuture<>();
        CompletableFuture<Void> started = new CompletableFuture<>();
        CompletableFuture<String> observed = new CompletableFuture<>();
        ZLinkFlowContext.State flow = ZLinkFlowContext.create(ZLinkFlowOrigin.INBOUND);

        queue.enqueue(() -> {
            try (ZLinkFlowContext.Scope ignored = ZLinkFlowContext.enter(flow)) {
                started.complete(null);
                return ZLinkAsyncSerialQueue.yieldCurrent(gate)
                    .thenRun(() -> observed.complete(ZLinkFlowContext.current().flowId()));
            }
        });

        started.get(3, TimeUnit.SECONDS);
        gate.complete(null);

        assertEquals(flow.flowId(), observed.get(3, TimeUnit.SECONDS));
    }

    @Test
    void startsQueuedOperationWithFlowCapturedAtEnqueue() throws Exception {
        ZLinkAsyncSerialQueue queue = new ZLinkAsyncSerialQueue();
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
        ZLinkAsyncSerialQueue queue = new ZLinkAsyncSerialQueue();
        CompletableFuture<Void> remote = new CompletableFuture<>();
        CompletableFuture<Void> afterYield = new CompletableFuture<>();
        CompletableFuture<Void> firstStarted = new CompletableFuture<>();
        CompletableFuture<Void> secondStarted = new CompletableFuture<>();

        CompletableFuture<Void> first = queue.enqueue(() -> {
            firstStarted.complete(null);
            return ZLinkAsyncSerialQueue.manageCurrent(remote)
                .thenCompose(ignored -> ZLinkAsyncSerialQueue.yieldCurrent(afterYield));
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
        ZLinkAsyncSerialQueue queue = new ZLinkAsyncSerialQueue();
        CompletableFuture<Void> sealNow = new CompletableFuture<>();
        CompletableFuture<Void> intentStarted = new CompletableFuture<>();
        CompletableFuture<ZLinkAsyncSerialQueue.RelocationSeal> sealed =
            new CompletableFuture<>();
        List<String> handled =
            new java.util.concurrent.CopyOnWriteArrayList<>();

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
        ZLinkAsyncSerialQueue.RelocationSeal seal =
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
        ZLinkAsyncSerialQueue queue = new ZLinkAsyncSerialQueue();
        CompletableFuture<Void> sealNow = new CompletableFuture<>();
        CompletableFuture<Void> intentStarted = new CompletableFuture<>();
        CompletableFuture<ZLinkAsyncSerialQueue.RelocationSeal> sealed =
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
        ZLinkAsyncSerialQueue.RelocationSeal seal =
            sealed.get(3, TimeUnit.SECONDS);
        CompletableFuture<Void> held = queue.enqueueRelocatable(
            new byte[] {2},
            () -> {
                ran.set(true);
                return CompletableFuture.completedFuture(null);
            }).toCompletableFuture();

        List<ZLinkAsyncSerialQueue.QueuedRecord> relay =
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
    }

    @Test
    void relocationCommitReleasesSourceResourcesWithoutRunningHandler()
        throws Exception {
        ZLinkAsyncSerialQueue queue = new ZLinkAsyncSerialQueue();
        AtomicReference<Boolean> released = new AtomicReference<>(false);
        AtomicReference<Boolean> ran = new AtomicReference<>(false);
        ZLinkAsyncSerialQueue.RelocationSeal seal =
            queue.trySealRelocation().orElseThrow();
        CompletableFuture<Void> held = queue.enqueueRelocatable(
            new byte[] {7},
            () -> {
                ran.set(true);
                return CompletableFuture.completedFuture(null);
            },
            () -> released.set(true)).toCompletableFuture();

        assertEquals(1, queue.commitRelocation(seal).orElseThrow().size());
        held.get(3, TimeUnit.SECONDS);
        assertTrue(released.get());
        assertFalse(ran.get());
    }

    @Test
    void relocationIngressFreezeFixesHeldHighWaterBeforeAuthorityPrepare()
        throws Exception {
        ZLinkAsyncSerialQueue queue = new ZLinkAsyncSerialQueue();
        ZLinkAsyncSerialQueue.RelocationSeal seal =
            queue.trySealRelocation().orElseThrow();
        CompletableFuture<Void> held = queue.enqueueRelocatable(
            new byte[] {7},
            () -> CompletableFuture.completedFuture(null))
            .toCompletableFuture();

        var frozen = queue.freezeRelocationIngress(seal).orElseThrow();
        assertEquals(1, frozen.size());
        assertArrayEquals(new byte[] {7}, frozen.getFirst().payload());
        assertTrue(queue.enqueueRelocatable(
            new byte[] {8},
            () -> CompletableFuture.completedFuture(null))
            .toCompletableFuture().isCompletedExceptionally());

        assertTrue(queue.abortRelocation(seal));
        held.get(3, TimeUnit.SECONDS);
    }

    @Test
    void relocationSealWaitsForYieldedContinuationToQuiesce()
        throws Exception {
        ZLinkAsyncSerialQueue queue = new ZLinkAsyncSerialQueue();
        CompletableFuture<Void> remote = new CompletableFuture<>();
        CompletableFuture<Void> yieldRegistered = new CompletableFuture<>();
        CompletableFuture<Void> continuationFinished =
            new CompletableFuture<>();

        CompletableFuture<Void> dispatch = queue.enqueue(() -> {
            CompletionStage<Void> yielded =
                ZLinkAsyncSerialQueue.yieldCurrent(remote);
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
        ZLinkAsyncSerialQueue queue = new ZLinkAsyncSerialQueue();
        CompletableFuture<Void> remote = new CompletableFuture<>();
        CompletableFuture<Void> yielded = new CompletableFuture<>();

        CompletableFuture<Void> dispatch = queue.enqueue(() -> {
            CompletionStage<Void> continuation =
                ZLinkAsyncSerialQueue.yieldCurrent(remote);
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
        ZLinkAsyncSerialQueue queue = new ZLinkAsyncSerialQueue();
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
        ZLinkAsyncSerialQueue queue = new ZLinkAsyncSerialQueue();
        CompletableFuture<Void> remote = new CompletableFuture<>();
        CompletableFuture<Boolean> sealed =
            new CompletableFuture<>();

        CompletableFuture<Void> dispatch = queue.enqueue(() ->
            ZLinkAsyncSerialQueue.yieldCurrent(remote))
            .toCompletableFuture();
        queue.enqueue(() -> {
            sealed.complete(queue.trySealRelocation().isPresent());
            return CompletableFuture.completedFuture(null);
        });

        assertFalse(sealed.get(3, TimeUnit.SECONDS));
        remote.complete(null);
        dispatch.get(3, TimeUnit.SECONDS);
        queue.enqueue(() -> CompletableFuture.completedFuture(null))
            .toCompletableFuture()
            .get(3, TimeUnit.SECONDS);
        assertTrue(queue.trySealRelocation().isPresent());
    }

    @Test
    void relocationAbortRequiresTheExactSealReferenceAndGeneration()
        throws Exception {
        ZLinkAsyncSerialQueue queue = new ZLinkAsyncSerialQueue();
        ZLinkAsyncSerialQueue.RelocationSeal first =
            queue.trySealRelocation().orElseThrow();
        var forged = new ZLinkAsyncSerialQueue.RelocationSeal(
            first.serial(),
            first.captured());

        assertFalse(queue.abortRelocation(forged));
        assertTrue(queue.abortRelocation(first));

        ZLinkAsyncSerialQueue.RelocationSeal second =
            queue.trySealRelocation().orElseThrow();
        assertFalse(queue.abortRelocation(first));
        assertTrue(queue.abortRelocation(second));
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
