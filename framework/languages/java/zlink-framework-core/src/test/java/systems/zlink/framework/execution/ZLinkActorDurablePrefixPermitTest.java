package systems.zlink.framework.execution;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;

import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.OptionalLong;
import java.util.concurrent.AbstractExecutorService;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;

final class ZLinkActorDurablePrefixPermitTest {
    @ParameterizedTest
    @org.junit.jupiter.params.provider.MethodSource("booleanCases")
    void canonicalDurableOwnerReturnsInitialReservation(boolean acceptedBeforeSeal) {
        var executor = new ManualExecutor();
        var serial =
                new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.actorDelivery());
        try (var jobs =
                new ZLinkApplicationJobQueue(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        OptionalLong.of(1),
                        new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null))) {
            var seal =
                    acceptedBeforeSeal
                            ? null
                            : serial.trySealRelocation().toCompletableFuture().join().orElseThrow();
            var permit =
                    jobs.acquire(ZLinkApplicationJobQueue.Origin.REMOTE)
                            .toCompletableFuture()
                            .join();
            var events = new ArrayList<String>();
            CompletableFuture<Void> terminal;
            try (var ignored = ZLinkApplicationJobContext.enter(permit)) {
                terminal =
                        serial.enqueueRelocatable(
                                        new byte[] {1},
                                        () -> {
                                            ZLinkApplicationJobContext
                                                    .beforeFirstApplicationInstruction();
                                            events.add("P");
                                            return CompletableFuture.completedFuture(null);
                                        },
                                        () -> {},
                                        null)
                                .toCompletableFuture();
            } finally {
                permit.abandonReservation();
            }
            if (acceptedBeforeSeal)
                seal = serial.trySealRelocation().toCompletableFuture().join().orElseThrow();
            try {
                assertEquals(List.of(), events);
                assertFalse(terminal.isDone());
                assertEquals(acceptedBeforeSeal ? 1 : 0, seal.captured().size());
                assertEquals(
                        0,
                        jobs.snapshot().permitsInUse(),
                        "canonical durable payload owner must return initial reservation before execution");
            } finally {
                assertTrue(serial.abortRelocation(seal).toCompletableFuture().join());
                executor.drain();
                assertTrue(terminal.isDone());
                terminal.join();
                serial.close();
                assertEquals(List.of("P"), events);
                assertEquals(0, jobs.snapshot().permitsInUse());
            }
        }
    }

    @ParameterizedTest
    @org.junit.jupiter.params.provider.MethodSource("booleanCases")
    void cancellingDurableHeadReturnsPendingAndGrantedReservations(boolean granted) {
        var executor = new ManualExecutor();
        var spot = new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.spot());
        var gate = new ZLinkSerialExecutionQueue.SharedSpotGate(spot);
        var actor =
                new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.actorDelivery());
        actor.bindSharedSpotGate(gate);
        try (var jobs =
                new ZLinkApplicationJobQueue(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        OptionalLong.of(1),
                        new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null))) {
            var foreign =
                    jobs.acquire(ZLinkApplicationJobQueue.Origin.REMOTE)
                            .toCompletableFuture()
                            .join();
            var calls = new ArrayList<Integer>();
            var admitted =
                    actor.admitDurablePrefix(
                                    List.of(
                                            new ZLinkSerialExecutionQueue.QueuedRecord(
                                                    1, new byte[] {1}),
                                            new ZLinkSerialExecutionQueue.QueuedRecord(
                                                    2, new byte[] {2})),
                                    (record, terminalResult) ->
                                            () -> {
                                                ZLinkApplicationJobContext
                                                        .beforeFirstApplicationInstruction();
                                                calls.add((int) record.payload()[0]);
                                                return CompletableFuture.completedFuture(null);
                                            },
                                    jobs)
                            .toCompletableFuture();
            executor.drain();
            assertTrue(admitted.isDone());
            var terminal = admitted.join();
            assertEquals(1, jobs.snapshot().capacityWaiters());
            assertEquals(List.of(), calls);
            var control = gate.controlAsync(() -> "control").toCompletableFuture();
            executor.drain();
            assertTrue(control.isDone());
            assertEquals("control", control.join(), "a permit waiter cannot hold the shared gate");
            if (granted) foreign.close();
            terminal.forEach(entry -> entry.cancel(false));
            executor.drain();
            assertEquals(
                    List.of(),
                    calls,
                    "cancellation before the first instruction must prevent execution");
            assertEquals(0, jobs.snapshot().capacityWaiters());
            foreign.close();
            executor.drain();
            assertEquals(0, jobs.snapshot().permitsInUse());
            assertEquals(0, jobs.snapshot().queuedApplicationJobs());
            actor.close();
            spot.close();
        }
    }

    @org.junit.jupiter.api.RepeatedTest(20)
    void nextHandoffOwnsWaitingSuffixAfterOldTerminalCleanup() {
        var executor = new ManualExecutor();
        var source =
                new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.actorDelivery());
        var target =
                new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.actorDelivery());
        try (var jobs =
                new ZLinkApplicationJobQueue(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        OptionalLong.of(1),
                        new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null))) {
            var foreign =
                    jobs.acquire(ZLinkApplicationJobQueue.Origin.REMOTE)
                            .toCompletableFuture()
                            .join();
            var calls = new ArrayList<String>();
            var terminal =
                    source.admitDurablePrefix(
                                    List.of(
                                            new ZLinkSerialExecutionQueue.QueuedRecord(
                                                    1, new byte[] {1}),
                                            new ZLinkSerialExecutionQueue.QueuedRecord(
                                                    2, new byte[] {2})),
                                    (record, terminalResult) ->
                                            () -> {
                                                calls.add("old");
                                                return CompletableFuture.completedFuture(null);
                                            },
                                    jobs)
                            .toCompletableFuture()
                            .join();
            executor.drain();
            assertEquals(1, jobs.snapshot().capacityWaiters());
            var seal = source.trySealRelocation().toCompletableFuture().join().orElseThrow();
            assertEquals(
                    List.of(1, 2),
                    seal.captured().stream().map(record -> (int) record.payload()[0]).toList());
            assertEquals(0, jobs.snapshot().capacityWaiters());
            var next =
                    target.admitDurablePrefix(
                                    seal.captured(),
                                    (record, terminalResult) ->
                                            () -> {
                                                ZLinkApplicationJobContext
                                                        .beforeFirstApplicationInstruction();
                                                calls.add("new" + record.payload()[0]);
                                                return CompletableFuture.completedFuture(null);
                                            },
                                    jobs)
                            .toCompletableFuture()
                            .join();
            source.commitRelocation(seal).toCompletableFuture().join().orElseThrow();
            terminal.forEach(entry -> assertTrue(entry.isDone()));
            CompletableFuture.allOf(terminal.toArray(CompletableFuture[]::new)).join();
            executor.drain();
            assertEquals(
                    1,
                    jobs.snapshot().capacityWaiters(),
                    "old terminal cleanup must not cancel the new owner waiter");
            foreign.close();
            executor.drain();
            next.forEach(entry -> assertTrue(entry.isDone()));
            CompletableFuture.allOf(next.toArray(CompletableFuture[]::new)).join();
            assertEquals(List.of("new1", "new2"), calls);
            assertEquals(0, jobs.snapshot().permitsInUse());
            assertEquals(0, jobs.snapshot().capacityWaiters());
            source.close();
            target.close();
        }
    }

    @org.junit.jupiter.api.RepeatedTest(20)
    void durableOwnershipIsDetachedBeforeItsEntryBecomesVisible() throws Exception {
        var executor = new ManualExecutor();
        var actor =
                new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.actorDelivery());
        var ownershipField =
                Class.forName(ZLinkSerialExecutionQueue.class.getName() + "$Entry")
                        .getDeclaredField("applicationJobOwnership");
        ownershipField.setAccessible(true);
        var observations = new ArrayList<Boolean>();
        var intercepted =
                new ArrayDeque<Object>() {
                    @Override
                    public void addLast(Object entry) {
                        try {
                            var ownership =
                                    (ZLinkApplicationJobContext.QueuedOwnership)
                                            ownershipField.get(entry);
                            observations.add(
                                    ownership != null
                                            && ownership.isDurable()
                                            && !ownership.isRunnable());
                        } catch (IllegalAccessException failure) {
                            observations.add(false);
                        }
                        super.addLast(entry);
                    }
                };
        var pending = ZLinkSerialExecutionQueue.class.getDeclaredField("applicationPending");
        pending.setAccessible(true);
        pending.set(actor, intercepted);
        try (var jobs =
                new ZLinkApplicationJobQueue(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        OptionalLong.of(1),
                        new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null))) {
            var permit =
                    jobs.acquire(ZLinkApplicationJobQueue.Origin.REMOTE)
                            .toCompletableFuture()
                            .join();
            var calls = new ArrayList<Integer>();
            var prefix =
                    actor.admitDurablePrefix(
                                    List.of(
                                            new ZLinkSerialExecutionQueue.QueuedRecord(
                                                    1, new byte[] {1})),
                                    (record, terminal) ->
                                            () -> {
                                                ZLinkApplicationJobContext
                                                        .beforeFirstApplicationInstruction();
                                                calls.add(1);
                                                return CompletableFuture.completedFuture(null);
                                            },
                                    jobs)
                            .toCompletableFuture();
            assertTrue(prefix.isDone());
            var prefixTerminal = prefix.join().getFirst();
            CompletableFuture<Void> ordinary;
            try (var scope = ZLinkApplicationJobContext.enter(permit)) {
                ordinary =
                        actor.enqueueRelocatable(
                                        new byte[] {2},
                                        () -> {
                                            ZLinkApplicationJobContext
                                                    .beforeFirstApplicationInstruction();
                                            calls.add(2);
                                            return CompletableFuture.completedFuture(null);
                                        },
                                        () -> {},
                                        null)
                                .toCompletableFuture();
            } finally {
                permit.abandonReservation();
            }
            try {
                assertEquals(
                        List.of(true, true),
                        observations,
                        "every published durable Entry must already have detached its runnable reservation");
                assertEquals(List.of(), calls);
            } finally {
                executor.drain();
                assertTrue(prefixTerminal.isDone());
                prefixTerminal.join();
                assertTrue(ordinary.isDone());
                ordinary.join();
                assertEquals(List.of(1, 2), calls);
                assertEquals(0, jobs.snapshot().permitsInUse());
                assertEquals(0, jobs.snapshot().capacityWaiters());
                actor.close();
            }
        }
    }

    @org.junit.jupiter.api.RepeatedTest(20)
    void sharedExecutorRejectionSettlesAnAlreadyAcceptedDurablePrefix() {
        var executor = new ManualExecutor();
        var spot = new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.spot());
        var gate = new ZLinkSerialExecutionQueue.SharedSpotGate(spot);
        var actor =
                new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.actorDelivery());
        actor.bindSharedSpotGate(gate);
        var release = new CompletableFuture<Void>();
        var accepted =
                new java.util.concurrent.atomic.AtomicReference<List<CompletableFuture<Void>>>();
        var calls = new ArrayList<Integer>();
        try (var jobs =
                new ZLinkApplicationJobQueue(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        OptionalLong.of(1),
                        new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null))) {
            var outer =
                    spot.enqueue(
                                    () -> {
                                        var prefix =
                                                actor.admitDurablePrefix(
                                                                List.of(
                                                                        new ZLinkSerialExecutionQueue
                                                                                .QueuedRecord(
                                                                                1, new byte[] {1})),
                                                                (record, terminal) ->
                                                                        () -> {
                                                                            ZLinkApplicationJobContext
                                                                                    .beforeFirstApplicationInstruction();
                                                                            calls.add(1);
                                                                            return CompletableFuture
                                                                                    .completedFuture(
                                                                                            null);
                                                                        },
                                                                jobs)
                                                        .toCompletableFuture();
                                        assertTrue(prefix.isDone());
                                        accepted.set(prefix.join());
                                        return release;
                                    },
                                    null)
                            .toCompletableFuture();
            try {
                executor.drain();
                assertNotNull(accepted.get());
                executor.reject = true;
                release.complete(null);
                assertTrue(
                        accepted.get().getFirst().isCompletedExceptionally(),
                        "executor rejection must settle an accepted head without acquiring its permit");
                assertEquals(List.of(), calls);
                assertEquals(0, jobs.snapshot().permitsInUse());
                assertEquals(0, jobs.snapshot().capacityWaiters());
            } finally {
                executor.reject = false;
                release.complete(null);
                if (accepted.get() != null)
                    accepted.get().forEach(terminal -> terminal.cancel(false));
                executor.drain();
                var actorQuiet = actor.awaitQuiescence().toCompletableFuture();
                var spotQuiet = spot.awaitQuiescence().toCompletableFuture();
                executor.drain();
                assertTrue(actorQuiet.isDone());
                actorQuiet.join();
                assertTrue(spotQuiet.isDone());
                spotQuiet.join();
                assertTrue(outer.isDone());
                outer.join();
                assertEquals(List.of(), calls);
                assertEquals(0, jobs.snapshot().permitsInUse());
                actor.close();
                spot.close();
            }
        }
    }

    @ParameterizedTest
    @org.junit.jupiter.params.provider.MethodSource("booleanCases")
    void offOwnerIngressSettlesReservationBeforeTheHandlerGate(boolean durablePrefix) {
        var executor = new ManualExecutor();
        var spot = new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.spot());
        var gate = new ZLinkSerialExecutionQueue.SharedSpotGate(spot);
        var actor =
                new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.actorDelivery());
        actor.bindSharedSpotGate(gate);
        var release = new CompletableFuture<Void>();
        try (var jobs =
                new ZLinkApplicationJobQueue(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        OptionalLong.of(1),
                        new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null))) {
            var calls = new ArrayList<Integer>();
            var permit =
                    jobs.acquire(ZLinkApplicationJobQueue.Origin.REMOTE)
                            .toCompletableFuture()
                            .join();
            var outer =
                    spot.enqueue(
                                    () -> {
                                        if (durablePrefix) {
                                            var admitted =
                                                    actor.admitDurablePrefix(
                                                                    List.of(
                                                                            new ZLinkSerialExecutionQueue
                                                                                    .QueuedRecord(
                                                                                    1,
                                                                                    new byte[] {
                                                                                        1
                                                                                    })),
                                                                    (record, terminalResult) ->
                                                                            () -> {
                                                                                ZLinkApplicationJobContext
                                                                                        .beforeFirstApplicationInstruction();
                                                                                calls.add(1);
                                                                                return CompletableFuture
                                                                                        .completedFuture(
                                                                                                null);
                                                                            },
                                                                    jobs)
                                                            .toCompletableFuture();
                                            assertTrue(admitted.isDone());
                                            admitted.join();
                                        }
                                        return release;
                                    },
                                    null)
                            .toCompletableFuture();
            executor.drain();
            CompletableFuture<Void> completed;
            try (var scope = ZLinkApplicationJobContext.enter(permit)) {
                completed =
                        actor.enqueueRelocatable(
                                        new byte[] {2},
                                        () -> {
                                            ZLinkApplicationJobContext
                                                    .beforeFirstApplicationInstruction();
                                            calls.add(2);
                                            return CompletableFuture.completedFuture(null);
                                        },
                                        () -> {},
                                        null)
                                .toCompletableFuture();
            } finally {
                permit.abandonReservation();
            }
            try {
                assertEquals(
                        durablePrefix ? 0 : 1,
                        jobs.snapshot().permitsInUse(),
                        "ingress settles durable ownership before waiting for the occupied handler gate");
                assertEquals(List.of(), calls);
            } finally {
                release.complete(null);
                executor.drain();
                assertTrue(outer.isDone());
                outer.join();
                assertTrue(completed.isDone());
                completed.join();
                assertEquals(durablePrefix ? List.of(1, 2) : List.of(2), calls);
                assertEquals(0, jobs.snapshot().permitsInUse());
                assertEquals(0, jobs.snapshot().capacityWaiters());
                actor.close();
                spot.close();
            }
        }
    }

    @org.junit.jupiter.api.RepeatedTest(20)
    void rejectedSharedOwnerPublicationReturnsTransferredReservation() {
        var executor = new ManualExecutor();
        executor.reject = true;
        var spot = new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.spot());
        var gate = new ZLinkSerialExecutionQueue.SharedSpotGate(spot);
        var actor =
                new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.actorDelivery());
        actor.bindSharedSpotGate(gate);
        try (var jobs =
                new ZLinkApplicationJobQueue(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        OptionalLong.of(1),
                        new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null))) {
            var permit =
                    jobs.acquire(ZLinkApplicationJobQueue.Origin.REMOTE)
                            .toCompletableFuture()
                            .join();
            CompletableFuture<Void> completed;
            try (var scope = ZLinkApplicationJobContext.enter(permit)) {
                completed =
                        actor.enqueueRelocatable(
                                        new byte[] {1},
                                        () -> {
                                            fail(
                                                    "rejected owner publication cannot invoke its handler");
                                            return CompletableFuture.completedFuture(null);
                                        },
                                        () -> {},
                                        null)
                                .toCompletableFuture();
            } finally {
                permit.abandonReservation();
            }
            assertTrue(completed.isCompletedExceptionally());
            assertEquals(0, jobs.snapshot().permitsInUse());
            assertEquals(0, jobs.snapshot().capacityWaiters());
            actor.close();
            spot.close();
        }
    }

    @ParameterizedTest
    @org.junit.jupiter.params.provider.MethodSource("booleanCases")
    void sealOrCancelWinsWhileHeadAcquisitionIsRegistering(boolean move) throws Exception {
        var executor = new ManualExecutor();
        var source =
                new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.actorDelivery());
        var target =
                new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.actorDelivery());
        var entered = new CompletableFuture<Void>();
        var released = new CompletableFuture<Void>();
        var blockNext = new java.util.concurrent.atomic.AtomicBoolean();
        java.util.function.LongSupplier clock =
                () -> {
                    if (blockNext.compareAndSet(true, false)) {
                        entered.complete(null);
                        released.join();
                    }
                    return 1;
                };
        var constructor =
                ZLinkApplicationJobQueue.class.getDeclaredConstructor(
                        ZLinkApplicationJobQueueProfile.class, OptionalLong.class,
                        ZLinkApplicationJobQueue.ProcessorCandidates.class,
                                java.util.function.LongSupplier.class);
        constructor.setAccessible(true);
        try (var jobs =
                constructor.newInstance(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        OptionalLong.of(1),
                        new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null),
                        clock)) {
            var foreign =
                    jobs.acquire(ZLinkApplicationJobQueue.Origin.REMOTE)
                            .toCompletableFuture()
                            .join();
            var calls = new ArrayList<String>();
            var previous =
                    source.admitDurablePrefix(
                                    move
                                            ? List.of(
                                                    new ZLinkSerialExecutionQueue.QueuedRecord(
                                                            1, new byte[] {1}))
                                            : List.of(
                                                    new ZLinkSerialExecutionQueue.QueuedRecord(
                                                            1, new byte[] {1}),
                                                    new ZLinkSerialExecutionQueue.QueuedRecord(
                                                            2, new byte[] {2})),
                                    (record, terminalResult) ->
                                            () -> {
                                                if (record.payload()[0] == 1)
                                                    fail(
                                                            "the old claim lost to seal/cancel before registration completed");
                                                ZLinkApplicationJobContext
                                                        .beforeFirstApplicationInstruction();
                                                calls.add("tail");
                                                return CompletableFuture.completedFuture(null);
                                            },
                                    jobs)
                            .toCompletableFuture()
                            .join();
            blockNext.set(true);
            var worker = CompletableFuture.runAsync(executor::drain);
            List<CompletableFuture<Void>> next = List.of();
            try {
                entered.get(3, TimeUnit.SECONDS);
                if (move) {
                    var seal =
                            source.trySealRelocation().toCompletableFuture().join().orElseThrow();
                    next =
                            target.admitDurablePrefix(
                                            seal.captured(),
                                            (record, terminalResult) ->
                                                    () -> {
                                                        ZLinkApplicationJobContext
                                                                .beforeFirstApplicationInstruction();
                                                        calls.add("next");
                                                        return CompletableFuture.completedFuture(
                                                                null);
                                                    },
                                            jobs)
                                    .toCompletableFuture()
                                    .join();
                    source.commitRelocation(seal).toCompletableFuture().join().orElseThrow();
                } else previous.getFirst().cancel(false);
            } finally {
                released.complete(null);
                worker.get(3, TimeUnit.SECONDS);
            }
            assertEquals(1, jobs.snapshot().capacityWaiters());
            foreign.close();
            executor.drain();
            next.forEach(entry -> assertTrue(entry.isDone()));
            CompletableFuture.allOf(next.toArray(CompletableFuture[]::new)).join();
            assertEquals(move ? List.of("next") : List.of("tail"), calls);
            assertEquals(0, jobs.snapshot().permitsInUse());
            assertEquals(0, jobs.snapshot().capacityWaiters());
            source.close();
            target.close();
        }
    }

    @org.junit.jupiter.api.RepeatedTest(20)
    void cancelledDurableHeadReleaseFailureDoesNotStopFollowingRecord() {
        var executor = new ManualExecutor();
        var serial =
                new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.actorDelivery());
        var calls = new ArrayList<String>();
        var releases = new java.util.concurrent.atomic.AtomicInteger();
        try (var jobs =
                new ZLinkApplicationJobQueue(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        OptionalLong.of(1),
                        new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null))) {
            var permit =
                    jobs.acquire(ZLinkApplicationJobQueue.Origin.REMOTE)
                            .toCompletableFuture()
                            .join();
            CompletableFuture<Void> head;
            try (var scope = ZLinkApplicationJobContext.enter(permit)) {
                head =
                        serial.enqueueRelocatable(
                                        new byte[] {1},
                                        () -> {
                                            fail("cancelled head must not run");
                                            return CompletableFuture.completedFuture(null);
                                        },
                                        () -> {
                                            releases.incrementAndGet();
                                            throw new IllegalStateException(
                                                    "payload release failed");
                                        },
                                        null)
                                .toCompletableFuture();
            } finally {
                permit.abandonReservation();
            }
            var seal = serial.trySealRelocation().toCompletableFuture().join().orElseThrow();
            CompletableFuture<Void> tail;
            try (var scope = ZLinkApplicationJobContext.enterDurable(jobs)) {
                tail =
                        serial.enqueueRelocatable(
                                        new byte[] {2},
                                        () -> {
                                            ZLinkApplicationJobContext
                                                    .beforeFirstApplicationInstruction();
                                            calls.add("tail");
                                            return CompletableFuture.completedFuture(null);
                                        },
                                        () -> {},
                                        null)
                                .toCompletableFuture();
            }
            var foreign =
                    jobs.acquire(ZLinkApplicationJobQueue.Origin.REMOTE)
                            .toCompletableFuture()
                            .join();
            try {
                assertTrue(serial.abortRelocation(seal).toCompletableFuture().join());
                executor.drain();
                assertEquals(1, jobs.snapshot().capacityWaiters());
                assertTrue(head.cancel(false));
                assertEquals(1, releases.get());
                executor.drain();
                assertEquals(
                        1,
                        jobs.snapshot().capacityWaiters(),
                        "cleanup failure must still schedule the following durable head");
                foreign.close();
                executor.drain();
                assertTrue(tail.isDone());
                tail.join();
                assertEquals(List.of("tail"), calls);
            } finally {
                foreign.close();
                executor.drain();
                if (!tail.isDone()) {
                    var cleanup =
                            serial.trySealRelocation().toCompletableFuture().join().orElseThrow();
                    serial.commitRelocation(cleanup).toCompletableFuture().join().orElseThrow();
                }
                executor.drain();
                assertTrue(tail.isDone());
                tail.join();
                assertEquals(0, jobs.snapshot().permitsInUse());
                assertEquals(0, jobs.snapshot().capacityWaiters());
                var quiescent = serial.awaitQuiescence().toCompletableFuture();
                assertTrue(quiescent.isDone());
                quiescent.join();
                serial.close();
            }
        }
    }

    @org.junit.jupiter.api.RepeatedTest(20)
    void propagatedSpotCarrierRetainsItsQueueRecordUntilActorAdmissionCompletes() {
        var executor = new ManualExecutor();
        var carriers = new ArrayDeque<Runnable>();
        var spot = new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.spot());
        var gate = new ZLinkSerialExecutionQueue.SharedSpotGate(spot);
        var actor =
                new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.actorDelivery());
        actor.bindSharedSpotGate(gate);
        var carrierFinished = new CompletableFuture<Void>();
        var accepted = new java.util.concurrent.atomic.AtomicReference<CompletableFuture<Void>>();
        var calls = new ArrayList<String>();
        try (var jobs =
                new ZLinkApplicationJobQueue(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        OptionalLong.of(1),
                        new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null))) {
            var permit =
                    jobs.acquire(ZLinkApplicationJobQueue.Origin.REMOTE)
                            .toCompletableFuture()
                            .join();
            var outer =
                    spot.enqueue(
                                    () -> {
                                        ZLinkSerialExecutionQueue.propagateCurrent(
                                                        carriers::addLast)
                                                .execute(
                                                        () -> {
                                                            assertTrue(
                                                                    spot.isCurrent(),
                                                                    "carrier must resume the owning Spot record");
                                                            try (var scope =
                                                                    ZLinkApplicationJobContext
                                                                            .enter(permit)) {
                                                                accepted.set(
                                                                        actor.enqueueRelocatable(
                                                                                        new byte[] {
                                                                                            1
                                                                                        },
                                                                                        () -> {
                                                                                            ZLinkApplicationJobContext
                                                                                                    .beforeFirstApplicationInstruction();
                                                                                            calls
                                                                                                    .add(
                                                                                                            "actor");
                                                                                            return CompletableFuture
                                                                                                    .completedFuture(
                                                                                                            null);
                                                                                        },
                                                                                        () -> {},
                                                                                        null)
                                                                                .toCompletableFuture());
                                                            } finally {
                                                                permit.abandonReservation();
                                                                carrierFinished.complete(null);
                                                            }
                                                        });
                                        return carrierFinished;
                                    },
                                    null)
                            .toCompletableFuture();
            var following =
                    spot.enqueue(
                                    () -> {
                                        calls.add("spot");
                                        return CompletableFuture.completedFuture(null);
                                    },
                                    null)
                            .toCompletableFuture();
            try {
                executor.drain();
                assertEquals(1, carriers.size());
                assertFalse(outer.isDone());
                assertFalse(following.isDone(), "callback result retains the owning Spot record");
                assertTrue(calls.isEmpty());
                assertDoesNotThrow(() -> carriers.removeFirst().run());
                assertNotNull(accepted.get());
                assertFalse(
                        accepted.get().isCompletedExceptionally(),
                        "carrier admission must use the same existing owner authority as controlAsync");
                executor.drain();
                assertTrue(outer.isDone());
                outer.join();
                assertTrue(accepted.get().isDone());
                accepted.get().join();
                assertTrue(following.isDone());
                following.join();
                assertEquals(List.of("actor", "spot"), calls.stream().sorted().toList());
                assertEquals(0, jobs.snapshot().permitsInUse());
                assertEquals(0, jobs.snapshot().capacityWaiters());
            } finally {
                carrierFinished.complete(null);
                executor.drain();
                permit.close();
                var actorDone = actor.awaitQuiescence().toCompletableFuture();
                var spotDone = spot.awaitQuiescence().toCompletableFuture();
                executor.drain();
                assertTrue(actorDone.isDone());
                assertTrue(spotDone.isDone());
                actorDone.join();
                spotDone.join();
                assertEquals(0, jobs.snapshot().permitsInUse());
                actor.close();
                spot.close();
            }
        }
    }

    @org.junit.jupiter.api.RepeatedTest(20)
    void cancelledOrdinaryResultKeepsItsAcceptedOperationAndFollowingRecordRunnable() {
        var executor = new ManualExecutor();
        var serial =
                new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.actorDelivery());
        var calls = new ArrayList<String>();
        try (var jobs =
                new ZLinkApplicationJobQueue(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        OptionalLong.of(2),
                        new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null))) {
            var terminals = new ArrayList<CompletableFuture<Void>>();
            for (String name : List.of("head", "tail")) {
                var permit =
                        jobs.acquire(ZLinkApplicationJobQueue.Origin.REMOTE)
                                .toCompletableFuture()
                                .join();
                try (var scope = ZLinkApplicationJobContext.enter(permit)) {
                    terminals.add(
                            serial.enqueueRelocatable(
                                            new byte[] {1},
                                            () -> {
                                                ZLinkApplicationJobContext
                                                        .beforeFirstApplicationInstruction();
                                                calls.add(name);
                                                return CompletableFuture.completedFuture(null);
                                            },
                                            () -> {},
                                            null)
                                    .toCompletableFuture());
                } finally {
                    permit.abandonReservation();
                }
            }
            try {
                terminals.getFirst().cancel(false);
                executor.drain();
                assertTrue(terminals.getFirst().isCancelled());
                assertTrue(
                        terminals.getLast().isDone(),
                        "cancelled ordinary result must not block its accepted FIFO operation");
                terminals.getLast().join();
                assertEquals(List.of("head", "tail"), calls);
                assertEquals(0, jobs.snapshot().permitsInUse());
            } finally {
                if (!terminals.getLast().isDone()) {
                    var seal =
                            serial.trySealRelocation().toCompletableFuture().join().orElseThrow();
                    serial.commitRelocation(seal).toCompletableFuture().join().orElseThrow();
                }
                executor.drain();
                assertTrue(terminals.getLast().isDone());
                terminals.getLast().join();
                assertEquals(0, jobs.snapshot().permitsInUse());
                assertEquals(0, jobs.snapshot().queuedApplicationJobs());
                var quiescent = serial.awaitQuiescence().toCompletableFuture();
                assertTrue(quiescent.isDone());
                quiescent.join();
                serial.close();
            }
        }
    }

    @org.junit.jupiter.api.RepeatedTest(20)
    void abortRestoresHeldHeadBeforeItsInitialClaimSettlement() throws Exception {
        var executor = new ManualExecutor();
        var serial =
                new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.actorDelivery());
        var terminal = new CompletableFuture<Void>();
        var calls = new ArrayList<String>();
        var hold =
                ZLinkSerialExecutionQueue.class.getDeclaredMethod(
                        "holdRelocationIngress",
                        byte[].class,
                        java.util.function.Supplier.class,
                        Runnable.class,
                        Object.class,
                        CompletableFuture.class,
                        CompletableFuture.class);
        hold.setAccessible(true);
        try (var jobs =
                new ZLinkApplicationJobQueue(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        OptionalLong.of(1),
                        new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null))) {
            var seal = serial.trySealRelocation().toCompletableFuture().join().orElseThrow();
            var permit =
                    jobs.acquire(ZLinkApplicationJobQueue.Origin.REMOTE)
                            .toCompletableFuture()
                            .join();
            Object entry;
            try (var scope = ZLinkApplicationJobContext.enter(permit)) {
                synchronized (serial) {
                    entry =
                            hold.invoke(
                                    serial,
                                    new byte[] {1},
                                    (java.util.function.Supplier<
                                                    java.util.concurrent.CompletionStage<Void>>)
                                            () -> {
                                                ZLinkApplicationJobContext
                                                        .beforeFirstApplicationInstruction();
                                                calls.add("held");
                                                return CompletableFuture.completedFuture(null);
                                            },
                                    (Runnable) () -> {},
                                    null,
                                    null,
                                    terminal);
                }
            }
            var settle =
                    ZLinkSerialExecutionQueue.class.getDeclaredMethod(
                            "settleOwnership", entry.getClass());
            settle.setAccessible(true);
            try {
                assertEquals(1, jobs.snapshot().permitsInUse());
                assertTrue(serial.abortRelocation(seal).toCompletableFuture().join());
                executor.drain();
                assertFalse(
                        terminal.isDone(), "head is restored while its initial claim is detached");
                assertEquals(0, jobs.snapshot().capacityWaiters());
                settle.invoke(serial, entry);
                executor.drain();
                assertTrue(
                        terminal.isDone(),
                        "settlement must return progress responsibility to the restored FIFO head");
                terminal.join();
                assertEquals(List.of("held"), calls);
            } finally {
                settle.invoke(serial, entry);
                permit.abandonReservation();
                if (!terminal.isDone())
                    serial.enqueuePreviouslyAccepted(() -> CompletableFuture.completedFuture(null));
                executor.drain();
                assertTrue(terminal.isDone());
                terminal.join();
                var quiescent = serial.awaitQuiescence().toCompletableFuture();
                assertTrue(quiescent.isDone());
                quiescent.join();
                assertEquals(0, jobs.snapshot().permitsInUse());
                assertEquals(0, jobs.snapshot().capacityWaiters());
                serial.close();
            }
        }
    }

    @ParameterizedTest
    @org.junit.jupiter.params.provider.MethodSource("booleanCases")
    void failedPrefixPreparationCompletesCreatedTerminalsWithoutPartialPublication(
            boolean closing) {
        var executor = new ManualExecutor();
        var serial =
                new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.actorDelivery());
        if (closing) serial.sealClosingAdmission();
        var observed = new ArrayList<CompletableFuture<Void>>();
        try (var jobs =
                new ZLinkApplicationJobQueue(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        OptionalLong.of(1),
                        new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null))) {
            var admission =
                    serial.admitDurablePrefix(
                                    List.of(
                                            new ZLinkSerialExecutionQueue.QueuedRecord(
                                                    1, new byte[] {1}),
                                            new ZLinkSerialExecutionQueue.QueuedRecord(
                                                    2, new byte[] {2})),
                                    (record, terminal) -> {
                                        observed.add(terminal);
                                        if (!closing && record.sequence() == 2)
                                            throw new IllegalArgumentException(
                                                    "factory failed before publication");
                                        return () -> {
                                            fail("rejected prefix must not run");
                                            return CompletableFuture.completedFuture(null);
                                        };
                                    },
                                    jobs)
                            .toCompletableFuture();
            assertTrue(admission.isCompletedExceptionally());
            assertEquals(2, observed.size());
            observed.forEach(terminal -> assertTrue(terminal.isCompletedExceptionally()));
            executor.drain();
            var quiescent = serial.awaitQuiescence().toCompletableFuture();
            assertTrue(quiescent.isDone());
            quiescent.join();
            assertEquals(0, jobs.snapshot().permitsInUse());
            assertEquals(0, jobs.snapshot().capacityWaiters());
            serial.close();
        }
    }

    @org.junit.jupiter.api.RepeatedTest(20)
    void nonsharedRejectionFinishesUnreservedDurablePrefix() {
        var executor = new ManualExecutor();
        executor.reject = true;
        var serial =
                new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.actorDelivery());
        try (var jobs =
                new ZLinkApplicationJobQueue(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        OptionalLong.of(1),
                        new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null))) {
            var accepted =
                    serial.admitDurablePrefix(
                                    List.of(
                                            new ZLinkSerialExecutionQueue.QueuedRecord(
                                                    1, new byte[] {1}),
                                            new ZLinkSerialExecutionQueue.QueuedRecord(
                                                    2, new byte[] {2})),
                                    (record, terminalResult) ->
                                            () -> {
                                                fail("rejected executor must not start a handler");
                                                return CompletableFuture.completedFuture(null);
                                            },
                                    jobs)
                            .toCompletableFuture();
            assertTrue(accepted.isDone());
            accepted.join().forEach(terminal -> assertTrue(terminal.isCompletedExceptionally()));
            assertEquals(0, jobs.snapshot().permitsInUse());
            assertEquals(0, jobs.snapshot().capacityWaiters());
            var quiescent = serial.awaitQuiescence().toCompletableFuture();
            assertTrue(quiescent.isDone());
            quiescent.join();
            serial.close();
        }
    }

    @org.junit.jupiter.api.RepeatedTest(20)
    void inlineExecutorDrainsDurablePrefixWithoutPerRecordWorkerRecursion() {
        var executor = new ManualExecutor();
        executor.inline = true;
        var serial =
                new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.actorDelivery());
        var opened = new CompletableFuture<Void>();
        serial.enqueueBarrierNext(() -> opened, null);
        try (var jobs =
                new ZLinkApplicationJobQueue(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        OptionalLong.of(1),
                        new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null))) {
            var calls = new ArrayList<Integer>();
            var records =
                    java.util.stream.IntStream.range(0, 2048)
                            .mapToObj(
                                    i ->
                                            new ZLinkSerialExecutionQueue.QueuedRecord(
                                                    i + 1, new byte[] {(byte) i}))
                            .toList();
            var accepted =
                    serial.admitDurablePrefix(
                                    records,
                                    (record, terminalResult) ->
                                            () -> {
                                                ZLinkApplicationJobContext
                                                        .beforeFirstApplicationInstruction();
                                                calls.add((int) record.sequence());
                                                return CompletableFuture.completedFuture(null);
                                            },
                                    jobs)
                            .toCompletableFuture()
                            .join();
            assertTrue(calls.isEmpty());
            opened.complete(null);
            assertEquals(2048, calls.size());
            assertEquals(java.util.stream.IntStream.rangeClosed(1, 2048).boxed().toList(), calls);
            accepted.forEach(terminal -> assertTrue(terminal.isDone()));
            assertEquals(0, jobs.snapshot().permitsInUse());
            assertEquals(0, jobs.snapshot().capacityWaiters());
            var quiescent = serial.awaitQuiescence().toCompletableFuture();
            assertTrue(quiescent.isDone());
            quiescent.join();
            serial.close();
        }
    }

    @ParameterizedTest
    @org.junit.jupiter.params.provider.ValueSource(booleans = {false, true})
    void activationPreparationDoesNotAcquireCapacityForBlockedApplication(boolean shared) {
        var executor = new ManualExecutor();
        var spot = new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.spot());
        if (shared) new ZLinkSerialExecutionQueue.SharedSpotGate(spot);
        var prepared = spot.prepareActivation(() -> CompletableFuture.completedFuture(null));
        executor.drain();
        assertTrue(prepared.toCompletableFuture().isDone());
        var calls = new ArrayList<Long>();
        try (var jobs =
                new ZLinkApplicationJobQueue(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        OptionalLong.of(1),
                        new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null))) {
            var foreign =
                    jobs.acquire(ZLinkApplicationJobQueue.Origin.REMOTE)
                            .toCompletableFuture()
                            .join();
            var admitted =
                    spot.admitDurablePrefix(
                                    List.of(
                                            new ZLinkSerialExecutionQueue.QueuedRecord(
                                                    1, new byte[] {1})),
                                    (record, terminal) ->
                                            () -> {
                                                ZLinkApplicationJobContext
                                                        .beforeFirstApplicationInstruction();
                                                calls.add(record.sequence());
                                                return CompletableFuture.completedFuture(null);
                                            },
                                    jobs)
                            .toCompletableFuture();
            try {
                executor.drain();
                assertTrue(admitted.isDone());
                assertEquals(
                        0,
                        jobs.snapshot().capacityWaiters(),
                        "capacity preparation must not select application work before activation admission");
                assertTrue(calls.isEmpty());
                var finished = spot.finishActivationPreparation().toCompletableFuture();
                executor.drain();
                assertTrue(finished.isDone());
                assertEquals(1, jobs.snapshot().capacityWaiters());
                foreign.close();
                executor.drain();
                assertEquals(List.of(1L), calls);
                admitted.join().forEach(terminal -> terminal.join());
                assertEquals(0, jobs.snapshot().permitsInUse());
                assertEquals(0, jobs.snapshot().capacityWaiters());
            } finally {
                spot.finishActivationPreparation();
                foreign.close();
                executor.drain();
                spot.close();
            }
        }
    }

    private static java.util.stream.Stream<Boolean> booleanCases() {
        return java.util.stream.IntStream.range(0, 20)
                .boxed()
                .flatMap(ignored -> java.util.stream.Stream.of(false, true));
    }

    private static final class ManualExecutor extends AbstractExecutorService {
        private final ArrayDeque<Runnable> pending = new ArrayDeque<>();
        private boolean shutdown;
        private boolean reject;
        private boolean inline;

        public void execute(Runnable command) {
            if (reject)
                throw new java.util.concurrent.RejectedExecutionException(
                        "test owner rejects publication");
            if (inline) command.run();
            else pending.addLast(command);
        }

        void drain() {
            while (!pending.isEmpty()) pending.removeFirst().run();
        }

        public void shutdown() {
            shutdown = true;
        }

        public List<Runnable> shutdownNow() {
            throw new UnsupportedOperationException();
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
