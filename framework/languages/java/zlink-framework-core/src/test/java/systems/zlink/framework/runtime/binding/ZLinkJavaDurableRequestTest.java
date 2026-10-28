package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode.PeerRouteObservation;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.protocol.ServiceWirePilotCodec;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkJavaDurableRequestTest {
    @Test
    void oneThousandReadinessWaitsUseOneEntryEachAndCancellationReleasesAllObservers()
            throws Exception {
        var observers = new AtomicInteger();
        var releasedAll = new CompletableFuture<Void>();
        try (var operations =
                new systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry(
                        new IllegalStateException("closed"))) {
            var requests = new java.util.ArrayList<CompletableFuture<List<byte[]>>>();
            for (int request = 0; request < 1_000; request++) {
                requests.add(
                        ZLinkJavaDurableRequest.<List<byte[]>>request(
                                        () -> List.of(new byte[] {1}),
                                        (frames, remaining, admission, terminal) -> {
                                            throw new AssertionError(
                                                    "unready route cannot start binding");
                                        },
                                        () -> new PeerRouteObservation(1, false),
                                        (changed, ended) -> {
                                            observers.incrementAndGet();
                                            changed.run();
                                            return () -> {
                                                if (observers.decrementAndGet() == 0)
                                                    releasedAll.complete(null);
                                            };
                                        },
                                        action -> {
                                            action.run();
                                            return CompletableFuture.completedStage(null);
                                        },
                                        Duration.ofHours(1),
                                        operations)
                                .toCompletableFuture());
            }
            assertEquals(1_000, operations.pendingCount());
            assertEquals(1_000, observers.get());
            for (var request : requests) assertTrue(request.cancel(false));
            assertEquals(0, operations.pendingCount());
            // §10.1/§11: C1 removal precedes dispatcher delivery and observer cleanup.
            releasedAll.get(1, TimeUnit.SECONDS);
            assertEquals(0, observers.get());
        }
    }

    @ParameterizedTest
    @org.junit.jupiter.params.provider.ValueSource(booleans = {false, true})
    void selectedRouteChangeDoesNotResubmitAStartedBindingRequest(boolean readyAtObservation) {
        var source = new Source();
        var pending = new CompletableFuture<List<byte[]>>();
        var attempts = new AtomicInteger();
        var failure = new ZlinkRequestException(RequestResult.NOT_CONNECTED);
        var result =
                source.request(
                                () -> List.of(new byte[] {1}),
                                (wire, remaining) -> {
                                    attempts.incrementAndGet();
                                    return pending;
                                },
                                () -> false,
                                Duration.ofSeconds(5))
                        .toCompletableFuture();
        source.route = new PeerRouteObservation(2, readyAtObservation);
        source.signal();
        // §5: route replacement does not cancel or restart a started binding operation.
        assertFalse(pending.isCancelled());
        assertEquals(1, attempts.get());
        pending.completeExceptionally(failure);
        assertFailure(result, ZLinkFrameworkErrorKind.UNAVAILABLE, failure);
        source.route = new PeerRouteObservation(2, true);
        source.signal();
        assertEquals(1, attempts.get());
    }

    @Test
    void initialUnreadyRouteDoesNotSubmitEvenWithPreparedFrames() {
        var source = new Source();
        source.route = new PeerRouteObservation(1, false);
        var attempts = new AtomicInteger();
        var terminal = List.of(new byte[] {9});
        var result =
                source.request(
                                () -> List.of(new byte[] {1}),
                                (frames, remaining) -> {
                                    attempts.incrementAndGet();
                                    return CompletableFuture.completedFuture(terminal);
                                },
                                () -> false,
                                Duration.ofSeconds(5))
                        .toCompletableFuture();
        assertEquals(0, attempts.get());
        assertFalse(result.isDone());
        source.route = new PeerRouteObservation(1, true);
        source.signal();
        assertSame(terminal, result.join());
        assertEquals(1, attempts.get());
    }

    @Test
    void bindingTerminalClaimsTheEntryBeforeAQueuedRouteTurn() {
        var source = new Source();
        var turns = new java.util.ArrayDeque<Runnable>();
        var pending = new CompletableFuture<List<byte[]>>();
        var terminal = List.of(new byte[] {9});
        var attempts = new AtomicInteger();
        var result =
                ZLinkJavaDurableRequestTest.request(
                                () -> List.of(new byte[] {1}),
                                (frames, remaining, admission) -> {
                                    admission.complete(null);
                                    attempts.incrementAndGet();
                                    return pending;
                                },
                                () -> source.route,
                                (changed, ended) -> source.observe(changed),
                                action -> {
                                    turns.add(action);
                                    return CompletableFuture.completedStage(null);
                                },
                                Duration.ofSeconds(5),
                                () -> 0)
                        .toCompletableFuture();
        turns.remove().run();
        source.route = new PeerRouteObservation(2, true);
        source.signal();
        pending.complete(terminal);
        while (!turns.isEmpty()) turns.remove().run();
        assertSame(terminal, result.join());
        assertEquals(1, attempts.get());
    }

    @Test
    void receivedReplyWinsOverQueuedLifecycleRemoval() throws Exception {
        lifecycleRemovalAfterReceivedTerminal(null);
    }

    @Test
    void receivedTerminalFailureWinsOverQueuedLifecycleRemoval() throws Exception {
        lifecycleRemovalAfterReceivedTerminal(new IllegalArgumentException("terminal"));
    }

    @Test
    void receivedTransientFailureClaimsTheEntryBeforeLaterLifecycleRemoval() throws Exception {
        lifecycleRemovalAfterReceivedTerminal(
                new ZlinkRequestException(RequestResult.NOT_CONNECTED));
    }

    private void lifecycleRemovalAfterReceivedTerminal(Throwable failure) throws Exception {
        var turns = new java.util.concurrent.LinkedBlockingQueue<Runnable>();
        var removed = new java.util.concurrent.atomic.AtomicReference<Runnable>();
        var pending = new CompletableFuture<List<byte[]>>();
        var terminal = List.of(new byte[] {9});
        var attempts = new AtomicInteger();
        var result =
                ZLinkJavaDurableRequestTest.request(
                                () -> List.of(new byte[] {1}),
                                (frames, remaining, admission) -> {
                                    attempts.incrementAndGet();
                                    admission.complete(null);
                                    return pending;
                                },
                                () -> new PeerRouteObservation(1, true),
                                (changed, ended) -> {
                                    removed.set(ended);
                                    changed.run();
                                    return () -> {};
                                },
                                action -> {
                                    turns.add(action);
                                    return CompletableFuture.completedStage(null);
                                },
                                Duration.ofSeconds(5),
                                () -> 0)
                        .toCompletableFuture();
        turns.remove().run();
        if (failure == null) pending.complete(terminal);
        else pending.completeExceptionally(failure);
        // §10.1: binding notification claims C1 before later lifecycle removal.
        removed.get().run();
        while (!turns.isEmpty()) turns.remove().run();
        if (failure == null) assertSame(terminal, result.join());
        else {
            var ended = assertThrows(CompletionException.class, result::join);
            if (failure instanceof ZlinkRequestException) {
                var error = assertInstanceOf(ZLinkFrameworkException.class, ended.getCause());
                assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE, error.kind());
            } else assertSame(failure, ended.getCause());
        }
        assertEquals(1, attempts.get(), "Lifecycle removal cannot submit another attempt");
    }

    @Test
    void routeChangeKeepsTheOriginalBindingTerminal() {
        var source = new Source();
        var original = new CompletableFuture<List<byte[]>>();
        var attempts = new AtomicInteger();
        var result =
                source.request(
                                () -> List.of(new byte[] {1}),
                                (frames, remaining) -> {
                                    attempts.incrementAndGet();
                                    return original;
                                },
                                () -> false,
                                Duration.ofSeconds(5))
                        .toCompletableFuture();
        source.route = new PeerRouteObservation(2, true);
        source.signal();
        assertFalse(original.isCancelled());
        var terminal = List.of(new byte[] {9});
        original.complete(terminal);
        assertSame(terminal, result.join());
        assertEquals(1, attempts.get(), "§5 prohibits a replacement binding operation");
    }

    enum Operation {
        ACTOR_JOIN,
        ACTOR_CREATE,
        BOUND_SESSION_BIND
    }

    @Test
    void logicalTargetRemovalEndsThePendingBindingAsUnavailable() throws Exception {
        var ended = new AtomicBoolean();
        var disconnected = new ZlinkRequestException(RequestResult.NOT_CONNECTED);
        var submitted = new CompletableFuture<List<byte[]>>();
        var attempts = new AtomicInteger();
        var source = new Source();
        var completion =
                source.request(
                                () -> List.of(new byte[] {1}),
                                (frames, remaining) -> {
                                    attempts.incrementAndGet();
                                    return submitted;
                                },
                                ended::get,
                                Duration.ofSeconds(5))
                        .toCompletableFuture();

        ended.set(true);
        source.signal();
        submitted.completeExceptionally(disconnected);

        var failure =
                assertThrows(
                        java.util.concurrent.ExecutionException.class,
                        () -> completion.get(1, TimeUnit.SECONDS));
        assertEquals(
                ZLinkFrameworkErrorKind.UNAVAILABLE,
                assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
        assertEquals(1, attempts.get(), "removed target must not be resubmitted");
    }

    @Test
    void removedTargetIsNotSubmittedEvenBeforeFirstAdmission() {
        var attempts = new AtomicInteger();
        var completion =
                new Source()
                        .request(
                                () -> List.of(new byte[] {1}),
                                (frames, remaining) -> {
                                    attempts.incrementAndGet();
                                    return CompletableFuture.completedFuture(List.of());
                                },
                                () -> true,
                                Duration.ofSeconds(5));
        assertFailure(completion.toCompletableFuture(), ZLinkFrameworkErrorKind.UNAVAILABLE, null);
        assertEquals(0, attempts.get());
    }

    @ParameterizedTest
    @EnumSource(Operation.class)
    void bindingSubmitFailureEndsWithoutReplay(Operation operation) throws Exception {
        var failure = new ZlinkSubmitException(SubmitResult.NOT_CONNECTED);
        var attempts = new AtomicInteger();
        List<byte[]> header = encode(operation);
        var source = new Source();
        var completion =
                source.request(
                        () -> header,
                        (frames, remaining) -> {
                            assertSame(header, frames);
                            attempts.incrementAndGet();
                            return CompletableFuture.failedFuture(failure);
                        },
                        () -> false,
                        Duration.ofMillis(80));
        assertFailure(
                completion.toCompletableFuture(), ZLinkFrameworkErrorKind.UNAVAILABLE, failure);
        source.signal();
        assertEquals(1, attempts.get(), "§5: a binding submission failure is terminal");
    }

    @ParameterizedTest
    @EnumSource(Operation.class)
    void admittedLostReplyExhaustsAsDeadlineExceeded(Operation operation) throws Exception {
        var failure = new ZlinkRequestException(RequestResult.TIMED_OUT);
        AtomicInteger attempts = new AtomicInteger();
        var clock = new java.util.concurrent.atomic.AtomicLong();
        List<byte[]> header = encode(operation);
        var completion =
                new Source(clock::get)
                        .request(
                                () -> header,
                                (frames, remaining) -> {
                                    attempts.incrementAndGet();
                                    // Binding consumes the whole admitted attempt budget.
                                    clock.addAndGet(remaining.toNanos());
                                    return CompletableFuture.failedFuture(failure);
                                },
                                () -> false,
                                Duration.ofMillis(80));
        assertFailure(
                completion.toCompletableFuture(),
                ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                failure);
        assertEquals(1, attempts.get(), "the attempt owns the whole remaining deadline");
    }

    @ParameterizedTest
    @EnumSource(Operation.class)
    void handoverFailureEndsTheSingleBindingSubmission(Operation operation) throws Exception {
        var preparations = new AtomicInteger();
        var attempts = new AtomicInteger();
        List<byte[]> header = encode(operation);
        byte[] snapshot = header.getFirst().clone();
        Duration timeout = Duration.ofSeconds(3);
        var failure = new ZlinkRequestException(RequestResult.NOT_CONNECTED);
        var source = new Source();
        var completion =
                source.request(
                        () -> {
                            preparations.incrementAndGet();
                            return header;
                        },
                        (frames, remaining) -> {
                            assertSame(header, frames);
                            assertArrayEquals(snapshot, frames.getFirst());
                            assertTrue(remaining.isPositive());
                            assertTrue(remaining.compareTo(timeout) <= 0);
                            attempts.incrementAndGet();
                            return CompletableFuture.failedFuture(failure);
                        },
                        () -> false,
                        timeout);
        assertFailure(
                completion.toCompletableFuture(), ZLinkFrameworkErrorKind.UNAVAILABLE, failure);
        source.route = new PeerRouteObservation(2, true);
        source.signal();
        assertEquals(1, preparations.get());
        assertEquals(1, attempts.get(), "§5 includes lifecycle requests in the no-replay rule");
    }

    @Test
    void preflightDoesNotInventAnAdmittedRequest() {
        AtomicInteger submits = new AtomicInteger();
        var completion =
                new Source()
                        .request(
                                () -> null,
                                (frames, remaining) -> {
                                    submits.incrementAndGet();
                                    return CompletableFuture.completedFuture(List.of());
                                },
                                () -> false,
                                Duration.ofMillis(30));
        assertFailure(
                completion.toCompletableFuture(), ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED, null);
        assertEquals(0, submits.get());
    }

    @Test
    void admittedFailureRemainsTerminalAcrossLaterSourceSignals() {
        var attempts = new AtomicInteger();
        var failure = new ZlinkRequestException(RequestResult.NOT_CONNECTED);
        var source = new Source();
        var completion =
                source.request(
                        () -> List.of(new byte[] {1}),
                        (frames, remaining) -> {
                            attempts.incrementAndGet();
                            return CompletableFuture.failedFuture(failure);
                        },
                        () -> false,
                        Duration.ofSeconds(5));
        assertFailure(
                completion.toCompletableFuture(), ZLinkFrameworkErrorKind.UNAVAILABLE, failure);
        source.route = new PeerRouteObservation(2, true);
        source.signal();
        assertEquals(1, attempts.get());
    }

    @Test
    void synchronousSubmitFailureRetainsItsTypedCause() {
        var failure = new ZlinkSubmitException(SubmitResult.BACKPRESSURED);
        var attempts = new AtomicInteger();
        var completion =
                new Source()
                        .request(
                                () -> List.of(new byte[] {1}),
                                (frames, remaining) -> {
                                    attempts.incrementAndGet();
                                    throw failure;
                                },
                                () -> false,
                                Duration.ofMillis(30));
        assertFailure(
                completion.toCompletableFuture(), ZLinkFrameworkErrorKind.UNAVAILABLE, failure);
        assertEquals(1, attempts.get(), "tokenless capacity rejection must not replay");
    }

    @Test
    void writableWaitTimeoutEndsAsDeadlineExceededWithoutReplay() {
        var failure = new ZlinkRequestException(RequestResult.TIMED_OUT);
        var attempts = new AtomicInteger();
        var pending = new CompletableFuture<List<byte[]>>();
        var completion =
                new Source()
                        .request(
                                () -> List.of(new byte[] {1}),
                                (frames, remaining) -> {
                                    attempts.incrementAndGet();
                                    return pending;
                                },
                                () -> false,
                                Duration.ofSeconds(5));
        pending.completeExceptionally(ZLinkJavaRawMeshNode.requestFailure(failure));
        // Caller publication follows the terminal owner's turn (state-ownership §5).
        assertFailure(
                completion.toCompletableFuture(),
                ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                failure);
        assertTrue(completion.toCompletableFuture().isDone());
        assertEquals(1, attempts.get(), "binding writable timeout is already terminal");
    }

    @Test
    void terminalReplyAndDecodeFailureDoNotReplay() {
        AtomicInteger attempts = new AtomicInteger();
        var malformed = new IllegalArgumentException("malformed terminal");
        var completion =
                new Source()
                        .request(
                                () -> List.of(new byte[] {1}),
                                (frames, remaining) -> {
                                    attempts.incrementAndGet();
                                    return CompletableFuture.completedFuture(
                                            List.of(new byte[] {0}));
                                },
                                () -> false,
                                Duration.ofSeconds(1))
                        .thenApply(
                                frames -> {
                                    throw malformed;
                                });
        assertSame(
                malformed,
                assertThrows(
                                CompletionException.class,
                                () -> completion.toCompletableFuture().join())
                        .getCause());
        assertEquals(1, attempts.get());
    }

    @Test
    void unknownBindingFailureIsPreserved() {
        var failure = new IllegalStateException("unknown binding failure");
        var completion =
                new Source()
                        .request(
                                () -> List.of(new byte[] {1}),
                                (frames, remaining) -> CompletableFuture.failedFuture(failure),
                                () -> false,
                                Duration.ofSeconds(1));
        assertSame(
                failure,
                assertThrows(
                                CompletionException.class,
                                () -> completion.toCompletableFuture().join())
                        .getCause());
    }

    @Test
    void sourceSignalsDoNotDuplicateAnInFlightAttemptAndCancellationReleasesObserver()
            throws Exception {
        var source = new Source();
        var attempts = new AtomicInteger();
        var pending = new CompletableFuture<List<byte[]>>();
        var completion =
                source.request(
                                () -> List.of(new byte[] {1}),
                                (frames, remaining) -> {
                                    attempts.incrementAndGet();
                                    return pending;
                                },
                                () -> false,
                                Duration.ofSeconds(5))
                        .toCompletableFuture();
        source.signal();
        source.signal();
        assertEquals(1, attempts.get());
        completion.cancel(false);
        // §11 delivers cancellation and its physical cleanup on the dispatcher turn.
        pending.handle((reply, failure) -> null).get(1, TimeUnit.SECONDS);
        assertTrue(pending.isCancelled());
        source.observerClosed.get(1, TimeUnit.SECONDS);
        assertNull(source.listener);
    }

    @Test
    void ownerFailureBeforeObservationDoesNotInstallAnObserver() {
        var source = new Source();
        var failure = new IllegalStateException("owner unavailable");
        var completion =
                ZLinkJavaDurableRequestTest.request(
                        () -> List.of(new byte[] {1}),
                        (frames, remaining, admission) ->
                                CompletableFuture.completedFuture(List.of()),
                        () -> source.route,
                        (changed, ended) -> source.observe(changed),
                        action -> CompletableFuture.failedStage(failure),
                        Duration.ofSeconds(5));
        assertSame(
                failure,
                assertThrows(
                                CompletionException.class,
                                () -> completion.toCompletableFuture().join())
                        .getCause());
        // The owner rejected initialization before observe was called; no close is due.
        assertFalse(source.observerClosed.isDone());
        assertNull(source.listener);
    }

    @Test
    void missingRouteWaitsForSourceAndTargetRemovalEndsPendingRequest() throws Exception {
        var source = new Source();
        var route = new AtomicBoolean();
        var ended = new AtomicBoolean();
        var attempts = new AtomicInteger();
        var pending = new CompletableFuture<List<byte[]>>();
        var completion =
                source.request(
                                () -> route.get() ? List.of(new byte[] {1}) : null,
                                (frames, remaining) -> {
                                    attempts.incrementAndGet();
                                    return pending;
                                },
                                ended::get,
                                Duration.ofSeconds(5))
                        .toCompletableFuture();
        assertEquals(0, attempts.get());
        route.set(true);
        source.signal();
        assertEquals(1, attempts.get());
        ended.set(true);
        source.signal();
        assertFailure(completion, ZLinkFrameworkErrorKind.UNAVAILABLE, null);
        pending.handle((reply, failure) -> null).get(1, TimeUnit.SECONDS);
        assertTrue(pending.isCancelled());
        source.observerClosed.get(1, TimeUnit.SECONDS);
        assertNull(source.listener);
    }

    @Test
    void durableNotAdmittedRetainsItsTypedTerminalWithoutReplay() throws Exception {
        var source = new Source();
        var terminal = new ZlinkSubmitException(SubmitResult.NOT_ADMITTED);
        var attempts = new AtomicInteger();
        var completion =
                source.request(
                        () -> List.of(new byte[] {1}),
                        (frames, remaining) -> {
                            attempts.incrementAndGet();
                            return CompletableFuture.failedFuture(terminal);
                        },
                        () -> false,
                        Duration.ofSeconds(5));
        assertSame(
                terminal,
                assertThrows(
                                CompletionException.class,
                                () -> completion.toCompletableFuture().join())
                        .getCause());
        source.signal();
        assertEquals(1, attempts.get());
        source.observerClosed.get(1, TimeUnit.SECONDS);
        assertNull(source.listener);
    }

    @Test
    void typedInternalFailureIsProjectedWithItsOriginalCause() {
        var failure = new ZlinkSubmitException(SubmitResult.INTERNAL_ERROR);
        var completion =
                new Source()
                        .request(
                                () -> List.of(new byte[] {1}),
                                (frames, remaining) -> CompletableFuture.failedFuture(failure),
                                () -> false,
                                Duration.ofSeconds(1));
        assertFailure(
                completion.toCompletableFuture(),
                ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
                failure);
    }

    @Test
    void terminatedBindingFailureIsProjectedAsShuttingDownWithOriginalCause() {
        var failure = new ZlinkSubmitException(SubmitResult.TERMINATED);
        var completion =
                new Source()
                        .request(
                                () -> List.of(new byte[] {1}),
                                (frames, remaining) -> CompletableFuture.failedFuture(failure),
                                () -> false,
                                Duration.ofSeconds(1));
        // Spec: 07-framework-error-model.ko.md:84; 01-submit-and-completion.ko.md:191.
        assertFailure(
                completion.toCompletableFuture(), ZLinkFrameworkErrorKind.SHUTTING_DOWN, failure);
    }

    @FunctionalInterface
    interface TestSubmit {
        java.util.concurrent.CompletionStage<List<byte[]>> apply(
                List<byte[]> frames, Duration remaining, CompletableFuture<Void> admission);
    }

    static java.util.concurrent.CompletionStage<List<byte[]>> request(
            java.util.function.Supplier<List<byte[]>> prepare,
            TestSubmit submit,
            java.util.function.Supplier<PeerRouteObservation> route,
            java.util.function.BiFunction<Runnable, Runnable, AutoCloseable> observe,
            java.util.function.Function<Runnable, java.util.concurrent.CompletionStage<Void>> owner,
            Duration timeout) {
        return request(prepare, submit, route, observe, owner, timeout, System::nanoTime);
    }

    static java.util.concurrent.CompletionStage<List<byte[]>> request(
            java.util.function.Supplier<List<byte[]>> prepare,
            TestSubmit submit,
            java.util.function.Supplier<PeerRouteObservation> route,
            java.util.function.BiFunction<Runnable, Runnable, AutoCloseable> observe,
            java.util.function.Function<Runnable, java.util.concurrent.CompletionStage<Void>> owner,
            Duration timeout,
            java.util.function.LongSupplier nanoTime) {
        var operations =
                new systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry(
                        new IllegalStateException("closed"),
                        () ->
                                new ZLinkFrameworkException(
                                        ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                                        "durable request deadline elapsed"),
                        nanoTime);
        var result =
                ZLinkJavaDurableRequest.<List<byte[]>>request(
                        prepare,
                        (frames, remaining, admission, terminal) -> {
                            var physical = submit.apply(frames, remaining, admission);
                            physical.whenComplete(terminal);
                            return physical;
                        },
                        route,
                        observe,
                        owner,
                        timeout,
                        operations);
        result.whenComplete((reply, failure) -> operations.closeAsync());
        return result;
    }

    private static class Source {
        volatile PeerRouteObservation route = new PeerRouteObservation(1, true);
        private final systems.zlink.framework.runtime.internal.execution.ZLinkStateLane stateLane =
                new systems.zlink.framework.runtime.internal.execution.ZLinkStateLane();
        private final java.util.function.LongSupplier nanoTime;
        private volatile Runnable listener;
        // Each Source fixture observes one logical request; close publishes that one event.
        private final CompletableFuture<Void> observerClosed = new CompletableFuture<>();

        Source() {
            this(System::nanoTime);
        }

        Source(java.util.function.LongSupplier nanoTime) {
            this.nanoTime = nanoTime;
        }

        AutoCloseable observe(Runnable signal) {
            listener = signal;
            signal.run();
            return () -> {
                listener = null;
                observerClosed.complete(null);
            };
        }

        void signal() {
            var callback = listener;
            if (callback != null) callback.run();
        }

        java.util.concurrent.CompletionStage<List<byte[]>> request(
                java.util.function.Supplier<List<byte[]>> prepare,
                java.util.function.BiFunction<
                                List<byte[]>,
                                Duration,
                                java.util.concurrent.CompletionStage<List<byte[]>>>
                        submit,
                java.util.function.BooleanSupplier ended,
                Duration timeout) {
            return ZLinkJavaDurableRequestTest.request(
                    prepare,
                    (frames, remaining, admission) -> {
                        var pending = submit.apply(frames, remaining);
                        if (!pending.toCompletableFuture().isCompletedExceptionally())
                            admission.complete(null);
                        return pending;
                    },
                    () -> route,
                    (changed, removed) ->
                            observe(
                                    () -> {
                                        if (ended.getAsBoolean()) removed.run();
                                        else changed.run();
                                    }),
                    action -> {
                        try {
                            if (stateLane.isOnLane()) action.run();
                            else
                                stateLane
                                        .runNowOrQueue(
                                                () -> {
                                                    action.run();
                                                    return null;
                                                })
                                        .toCompletableFuture()
                                        .join();
                            return CompletableFuture.completedStage(null);
                        } catch (RuntimeException failure) {
                            return CompletableFuture.failedStage(failure);
                        }
                    },
                    timeout,
                    nanoTime);
        }
    }

    private static void assertFailure(
            CompletableFuture<?> completion, ZLinkFrameworkErrorKind kind, Throwable cause) {
        var failure = assertThrows(CompletionException.class, completion::join);
        var framework = assertInstanceOf(ZLinkFrameworkException.class, failure.getCause());
        assertEquals(kind, framework.kind());
        assertSame(cause, framework.getCause());
    }

    private static List<byte[]> encode(Operation operation) throws java.io.IOException {
        RoutingId source = RoutingId.from("source");
        RoutingId target = RoutingId.from("target");
        var codec = new ZLinkServiceM6BWireCodec();
        return switch (operation) {
            case ACTOR_JOIN ->
                    ServiceWirePilotCodec.encodeActorJoin28(
                            new ServiceWirePilotCodec.ActorJoin28(
                                    17,
                                    new ServiceWirePilotCodec.Fence(
                                            "actor", 3, source.toBytes(), 4, 5, 6),
                                    false,
                                    new ServiceWirePilotCodec.Fence(
                                            "spot", 7, target.toBytes(), 8, 9, 10),
                                    new ServiceWirePilotCodec.ApplicationPayloadEnvelopeV1(
                                            "join", "application/json", new byte[] {123, 125})));
            case ACTOR_CREATE ->
                    List.of(
                            codec.encodeActorCreateHeader(
                                    new ZLinkServiceM6BWireCodec.ActorCreate(
                                            17,
                                            18,
                                            19,
                                            source,
                                            4,
                                            "actor",
                                            "player",
                                            new ZLinkServiceM6BWireCodec.ReservationFence(
                                                    "reservation",
                                                    "version",
                                                    9,
                                                    10,
                                                    target,
                                                    8,
                                                    "owner",
                                                    6,
                                                    1),
                                            1_900_000_000_000L)));
            case BOUND_SESSION_BIND ->
                    List.of(
                            codec.encodeBoundSessionBindHeader(
                                    new ZLinkServiceM6BWireCodec.BoundSessionBind(
                                            17,
                                            new ZLinkServiceM6BWireCodec.ActorRouteFence(
                                                    new ZLinkBackendActorRef(target, "actor", 3),
                                                    8,
                                                    9,
                                                    10),
                                            RoutingId.from("session"),
                                            true,
                                            11)));
        };
    }
}
