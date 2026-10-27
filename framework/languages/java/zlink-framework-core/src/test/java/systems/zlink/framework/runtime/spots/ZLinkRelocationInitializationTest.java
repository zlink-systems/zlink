package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.RepeatedTest;
import org.junit.jupiter.api.Test;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.*;

import java.util.*;
import java.util.concurrent.*;

final class ZLinkRelocationInitializationTest {
    @Test
    void discardDuringInitializationKeepsLaterLifecycleBehindThePreparationResult() {
        var queue =
                new ZLinkSerialExecutionQueue(
                        Runnable::run,
                        systems.zlink.framework.execution.ZLinkExecutionLanePolicy.spot());
        var serials =
                new ZLinkSpotSerialExecutor(
                        queue,
                        Runnable::run,
                        Runnable::run,
                        systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode.SPOT_WIDE,
                        false);
        serials.prepareRelocationTarget(() -> CompletableFuture.completedFuture(null))
                .toCompletableFuture()
                .join();
        var resume = new CompletableFuture<Void>();
        var initialization =
                serials.continueActivationPreparation(
                                () -> ZLinkSerialExecutionQueue.yieldCurrent(resume))
                        .toCompletableFuture();
        serials.discardActivationPreparation().toCompletableFuture().join();
        serials.releaseRelocationPreparation().toCompletableFuture().join();
        var later =
                queue.enqueueLifecycleBarrier(() -> CompletableFuture.completedFuture(null))
                        .toCompletableFuture();
        assertFalse(initialization.isDone());
        assertFalse(later.isDone());
        resume.complete(null);
        initialization.join();
        later.join();
        queue.close();
    }

    @Test
    void preparationRetainsRecordAndAdmissionThroughInitializationAndPublication() {
        var queue =
                new ZLinkSerialExecutionQueue(
                        Runnable::run,
                        systems.zlink.framework.execution.ZLinkExecutionLanePolicy.spot());
        var serials =
                new ZLinkSpotSerialExecutor(
                        queue,
                        Runnable::run,
                        Runnable::run,
                        systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode.SPOT_WIDE,
                        false);
        serials.prepareRelocationTarget(
                        () -> {
                            assertTrue(queue.isCurrent());
                            return CompletableFuture.completedFuture(null);
                        })
                .toCompletableFuture()
                .join();
        var packetRan = new java.util.concurrent.atomic.AtomicBoolean();
        var packet =
                queue.enqueue(
                                () -> {
                                    packetRan.set(true);
                                    return CompletableFuture.completedFuture(null);
                                },
                                null)
                        .toCompletableFuture();
        var resume = new CompletableFuture<Void>();
        var initialization =
                serials.continueActivationPreparation(
                                () -> {
                                    assertTrue(queue.isCurrent());
                                    return ZLinkSerialExecutionQueue.yieldCurrent(resume)
                                            .thenRun(
                                                    () -> {
                                                        assertTrue(queue.isCurrent());
                                                        assertFalse(packetRan.get());
                                                    });
                                })
                        .toCompletableFuture();
        assertFalse(initialization.isDone());
        assertFalse(packet.isDone());
        resume.complete(null);
        initialization.join();
        assertFalse(packet.isDone());
        serials.finishActivationPreparation().toCompletableFuture().join();
        assertFalse(packet.isDone());
        serials.releaseRelocationPreparation().toCompletableFuture().join();
        packet.join();
        assertTrue(packetRan.get());
        queue.close();
    }

    @Test
    void parentLifecycleCanPrepareInitializeAndDiscardTargetWithoutWaitingOnItself()
            throws Exception {
        var parent =
                new ZLinkSerialExecutionQueue(
                        Runnable::run,
                        systems.zlink.framework.execution.ZLinkExecutionLanePolicy.spot());
        var target =
                new ZLinkSerialExecutionQueue(
                        Runnable::run,
                        systems.zlink.framework.execution.ZLinkExecutionLanePolicy.spot());
        var serials =
                new ZLinkSpotSerialExecutor(
                        target,
                        Runnable::run,
                        Runnable::run,
                        systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode.SPOT_WIDE,
                        false);
        var initialized = new java.util.concurrent.atomic.AtomicBoolean();
        var result =
                parent.enqueueLifecycleBarrier(
                                () ->
                                        serials.prepareRelocationTarget(
                                                        () ->
                                                                CompletableFuture.completedFuture(
                                                                        null))
                                                .thenCompose(
                                                        ignored ->
                                                                serials
                                                                        .continueActivationPreparation(
                                                                                () -> {
                                                                                    assertTrue(
                                                                                            target
                                                                                                    .isCurrent());
                                                                                    initialized.set(
                                                                                            true);
                                                                                    return CompletableFuture
                                                                                            .completedFuture(
                                                                                                    null);
                                                                                }))
                                                .thenCompose(
                                                        ignored ->
                                                                serials
                                                                        .discardActivationPreparation())
                                                .thenCompose(
                                                        ignored ->
                                                                serials
                                                                        .releaseRelocationPreparation()))
                        .toCompletableFuture();
        result.get(3, TimeUnit.SECONDS);
        assertTrue(initialized.get());
        target.enqueueLifecycleBarrier(() -> CompletableFuture.completedFuture(null))
                .toCompletableFuture()
                .join();
        parent.close();
        target.close();
    }

    @Test
    void discardBeforeInitializationTerminatesPreparationAndRejectsPendingPacket() {
        var queue =
                new ZLinkSerialExecutionQueue(
                        Runnable::run,
                        systems.zlink.framework.execution.ZLinkExecutionLanePolicy.spot());
        var serials =
                new ZLinkSpotSerialExecutor(
                        queue,
                        Runnable::run,
                        Runnable::run,
                        systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode.SPOT_WIDE,
                        false);
        serials.prepareRelocationTarget(() -> CompletableFuture.completedFuture(null))
                .toCompletableFuture()
                .join();
        var packet =
                queue.enqueue(
                                () -> {
                                    fail("discarded target must not execute a pending packet");
                                    return CompletableFuture.completedFuture(null);
                                },
                                null)
                        .toCompletableFuture();
        serials.discardActivationPreparation().toCompletableFuture().join();
        serials.releaseRelocationPreparation().toCompletableFuture().join();
        assertThrows(CompletionException.class, packet::join);
        queue.enqueueLifecycleBarrier(() -> CompletableFuture.completedFuture(null))
                .toCompletableFuture()
                .join();
        queue.close();
    }

    @Test
    void ordinaryInitializeOwnsRecordAndPublicYieldSucceeds() throws Exception {
        run(false, 1);
    }

    @Test
    void relocatedInitializeOwnsRecordAndPublicYieldSucceeds() throws Exception {
        run(true, 1);
    }

    @RepeatedTest(10)
    void relocatedInitializeRepeated() throws Exception {
        run(true, 1);
    }

    @Test
    void relocatedInitializeUnderEightWorkers() throws Exception {
        run(true, 16);
    }

    @Test
    void sealFirstHoldsLifecycleAndRejectsBarrier() {
        var queue =
                new ZLinkSerialExecutionQueue(
                        Runnable::run,
                        systems.zlink.framework.execution.ZLinkExecutionLanePolicy.spot());
        var seal = queue.trySealRelocation().toCompletableFuture().join().orElseThrow();
        var entered = new java.util.concurrent.atomic.AtomicBoolean();
        var held =
                queue.enqueueBarrierNext(
                                () -> {
                                    entered.set(true);
                                    return CompletableFuture.completedFuture(null);
                                },
                                null)
                        .toCompletableFuture();
        var barrier = queue.enqueueLifecycleBarrier(() -> CompletableFuture.completedFuture(null));
        var failure =
                assertThrows(CompletionException.class, () -> barrier.toCompletableFuture().join());
        assertEquals("queue relocation is in progress", failure.getCause().getMessage());
        assertFalse(held.isDone());
        assertFalse(entered.get());
        assertTrue(queue.abortRelocation(seal).toCompletableFuture().join());
        held.join();
        assertTrue(entered.get());
        queue.close();
    }

    @Test
    void recordFirstCanSealAndResumeItsLifecycleContinuation() {
        var queue =
                new ZLinkSerialExecutionQueue(
                        Runnable::run,
                        systems.zlink.framework.execution.ZLinkExecutionLanePolicy.spot());
        var trigger = new CompletableFuture<Void>();
        var seal =
                new java.util.concurrent.atomic.AtomicReference<
                        ZLinkSerialExecutionQueue.RelocationSeal>();
        var resumed = new java.util.concurrent.atomic.AtomicBoolean();
        var result =
                queue.enqueueLifecycleBarrier(
                                () -> {
                                    var handle = queue.captureActiveTurnSealHandle().orElseThrow();
                                    seal.set(
                                            queue.trySealRelocation(handle)
                                                    .toCompletableFuture()
                                                    .join()
                                                    .orElseThrow());
                                    return ZLinkSerialExecutionQueue.yieldCurrent(trigger)
                                            .thenRun(
                                                    () -> {
                                                        resumed.set(queue.isCurrent());
                                                    });
                                })
                        .toCompletableFuture();
        assertFalse(result.isDone());
        trigger.complete(null);
        result.join();
        assertTrue(resumed.get());
        assertTrue(queue.abortRelocation(seal.get()).toCompletableFuture().join());
        queue.close();
    }

    private void run(boolean target, int count) throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.DETAILED);
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        options.addRouteMesh("mesh")
                .listen("inproc://d2526-" + UUID.randomUUID())
                .objects()
                .server()
                .addSpotFactory("diagnostic", ProbeSpot.class, f -> f.disableRelocation());
        try (var framework = ZLinkFrameworkRuntimeTestAccess.start(options);
                var pool = Executors.newFixedThreadPool(Math.min(count, 8))) {
            var host = (ZLinkSpotRuntime) framework.spotManager();
            var field = ZLinkSpotRuntime.class.getDeclaredField("spotLifecycle");
            field.setAccessible(true);
            var lifecycle = (ZLinkSpotLifecycle) field.get(host);
            var envelope =
                    ZLinkSpotTimerRelocationEnvelope.encode(
                            new ZLinkSpotTimerRegistry.FrozenTimers(List.of()));
            var futures = new ArrayList<CompletableFuture<ProbeSpot>>();
            for (int i = 0; i < count; i++) {
                final var id = "probe-" + i + "-" + UUID.randomUUID();
                futures.add(
                        CompletableFuture.supplyAsync(
                                () -> {
                                    var prepared =
                                            (target
                                                            ? lifecycle.prepareRelocationReserved(
                                                                    "mesh",
                                                                    ProbeSpot.class,
                                                                    id,
                                                                    1,
                                                                    envelope)
                                                            : lifecycle.prepareReserved(
                                                                    "mesh",
                                                                    ProbeSpot.class,
                                                                    id,
                                                                    1,
                                                                    null))
                                                    .toCompletableFuture()
                                                    .join();
                                    try {
                                        var spot = (ProbeSpot) lifecycle.preparedSpot(prepared);
                                        if (target)
                                            lifecycle
                                                    .completeRelocationReady(prepared)
                                                    .toCompletableFuture()
                                                    .join();
                                        return spot;
                                    } finally {
                                        lifecycle
                                                .discardReserved(prepared)
                                                .toCompletableFuture()
                                                .join();
                                    }
                                },
                                pool));
            }
            var observations = futures.stream().map(CompletableFuture::join).toList();
            assertAll(
                    observations.stream()
                            .map(
                                    spot ->
                                            (org.junit.jupiter.api.function.Executable)
                                                    () -> {
                                                        assertTrue(
                                                                spot.application,
                                                                "onInitialize must have ApplicationExecution");
                                                        assertTrue(
                                                                spot.record,
                                                                "relocation onInitialize must own its Spot serial queue record");
                                                        assertNull(
                                                                spot.error,
                                                                "public CPU worker Yield must succeed: "
                                                                        + spot.error);
                                                        assertTrue(
                                                                spot.resumedRecord,
                                                                "Yield continuation must resume inside the record");
                                                    }));
        }
    }

    public static final class ProbeSpot implements ZLinkSpot<ZLinkActor> {
        final ZLinkSpotContext context;
        volatile boolean application, record, resumedRecord, ready;
        volatile Throwable error;

        public ProbeSpot(ZLinkSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onInitialize() {
            application = ZLinkSuspendInvocationContext.currentApplicationExecution() != null;
            record = ZLinkSerialExecutionQueue.captureCurrentActiveTurnSealHandle().isPresent();
            try {
                return context.runCpuWorker(cancellation -> 42)
                        .yield()
                        .thenCompose(
                                value -> {
                                    assertEquals(42, value);
                                    assertTrue(
                                            ZLinkSerialExecutionQueue
                                                    .captureCurrentActiveTurnSealHandle()
                                                    .isPresent());
                                    return context.runIoWorker(
                                                    cancellation ->
                                                            CompletableFuture.completedFuture(43))
                                            .yield();
                                })
                        .thenAccept(
                                value -> {
                                    assertEquals(43, value);
                                    resumedRecord =
                                            ZLinkSerialExecutionQueue
                                                    .captureCurrentActiveTurnSealHandle()
                                                    .isPresent();
                                })
                        .whenComplete((ignored, failure) -> error = failure);
            } catch (RuntimeException failure) {
                error = failure;
                return CompletableFuture.failedFuture(failure);
            }
        }

        @Override
        public CompletionStage<Void> onRelocationReadyCompleted(
                ZLinkSpotRelocationReadyCompletion completion) {
            ready = true;
            return CompletableFuture.completedFuture(null);
        }
    }
}
