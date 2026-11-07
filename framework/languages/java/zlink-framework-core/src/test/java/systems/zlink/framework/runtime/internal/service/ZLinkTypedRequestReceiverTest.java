package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.RepeatedTest;
import org.junit.jupiter.api.Test;

import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.internal.ZLinkCompletionBridge;
import systems.zlink.framework.runtime.internal.calls.ZLinkRequestStages;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;

import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayList;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ForkJoinTask;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BiConsumer;

final class ZLinkTypedRequestReceiverTest {
    private static final Duration TIMEOUT = Duration.ofHours(1);

    private static final class Payload {
        final AtomicInteger closed = new AtomicInteger();

        void close() {
            assertEquals(1, closed.incrementAndGet());
        }
    }

    private static final class Request {
        final AtomicInteger decoded = new AtomicInteger();
        final AtomicInteger failures = new AtomicInteger();
        final AtomicInteger observed = new AtomicInteger();
        final CompletableFuture<Void> disposed = new CompletableFuture<>();
        final CompletableFuture<Void> failureProcessed = new CompletableFuture<>();
        final CompletableFuture<Void> terminalObserved = new CompletableFuture<>();
        final ZLinkServiceOperationRegistry.Operation<String> result;
        final AtomicReference<BiConsumer<Payload, Throwable>> physical = new AtomicReference<>();

        Request(ZLinkServiceOperationRegistry registry) {
            result =
                    ZLinkServiceOperationRegistry.<Payload, String>prepareOperation(
                            ZLinkServiceOperationIds.next(),
                            payload -> {
                                decoded.incrementAndGet();
                                assertEquals(0, payload.closed.get());
                                assertTrue(ZLinkServiceCompletionDispatcher.isCurrentCompletion());
                                return "typed";
                            },
                            null,
                            failure -> {
                                failures.incrementAndGet();
                                failureProcessed.complete(null);
                                return failure;
                            },
                            (caller, failure) -> {
                                observed.incrementAndGet();
                                terminalObserved.complete(null);
                            });
            registry.<Payload>submitTerminal(
                            result.id(),
                            TIMEOUT,
                            terminal -> {
                                physical.set(terminal);
                                return null;
                            },
                            payload -> {
                                payload.close();
                                disposed.complete(null);
                            },
                            result)
                    .toCompletableFuture()
                    .join();
        }

        void success(Payload payload) {
            physical.get().accept(payload, null);
        }

        void failure(Throwable failure) {
            physical.get().accept(null, failure);
        }
    }

    private static ZLinkServiceOperationRegistry registry() {
        return new ZLinkServiceOperationRegistry(
                command ->
                        new Thread(command) {
                            @Override
                            public synchronized void start() {}
                        });
    }

    @Test
    void convertedOwnedReplyIsDiscardedOnlyWhenPublicResultAlreadyFinished() throws Exception {
        for (boolean earlyFailure : new boolean[] {false, true}) {
            try (var registry = registry()) {
                var raw = new Payload();
                var converted = new Payload();
                var disposed = new CompletableFuture<Void>();
                var receiver =
                        ZLinkServiceOperationRegistry.<Payload, Payload>prepareOperation(
                                ZLinkServiceOperationIds.next(),
                                payload -> {
                                    assertSame(raw, payload);
                                    assertEquals(0, raw.closed.get());
                                    return converted;
                                },
                                value -> {
                                    value.close();
                                    disposed.complete(null);
                                },
                                failure -> failure,
                                null);
                registry.<Payload>submitTerminal(
                                receiver.id(), TIMEOUT, terminal -> null, Payload::close, receiver)
                        .toCompletableFuture()
                        .join();
                var observed = receiver.handle((value, error) -> raw.closed.get());
                if (earlyFailure)
                    receiver.completeExceptionally(new IllegalStateException("early"));
                assertTrue(registry.complete(receiver.id(), raw));
                if (earlyFailure) {
                    disposed.get(5, TimeUnit.SECONDS);
                    assertEquals(1, converted.closed.get());
                } else {
                    assertSame(converted, receiver.get(5, TimeUnit.SECONDS));
                    assertEquals(1, observed.get(5, TimeUnit.SECONDS));
                    assertEquals(0, converted.closed.get());
                    converted.close();
                }
                assertEquals(1, raw.closed.get());
            }
        }
    }

    @Test
    void replyPayloadIsReleasedBeforePublicContinuation() throws Exception {
        try (var registry = registry()) {
            var request = new Request(registry);
            var payload = new Payload();
            var observed = request.result.thenApply(value -> payload.closed.get());
            request.success(payload);
            assertEquals(1, observed.get(5, TimeUnit.SECONDS));
        }
    }

    @RepeatedTest(100)
    void finalReceiverHasNoConversionDependentAndNeverRunsOnPump() throws Exception {
        for (boolean observeAdmission : new boolean[] {false, true}) {
            try (var registry = registry()) {
                var request = new Request(registry);
                assertSame(request.result, registry.pendingReceiverForTest());
                assertTrue(registry.pendingRegisteredForTest());
                assertEquals(0, request.result.getNumberOfDependents());
                assertSame(request.result, ZLinkSerialExecutionQueue.manageCurrent(request.result));
                assertEquals(0, request.result.getNumberOfDependents());
                var admission = ZLinkCompletionBridge.<Void>newCancellationFuture();
                var stage = ZLinkRequestStages.observe(request.result, admission);
                assertEquals(0, admission.getNumberOfDependents());
                var admitted = observeAdmission ? stage.admitted() : null;
                var callback =
                        stage.thenApply(
                                value -> {
                                    assertTrue(ForkJoinTask.inForkJoinPool());
                                    assertTrue(
                                            ZLinkServiceCompletionDispatcher.isCurrentCompletion());
                                    return Thread.currentThread();
                                });
                var payload = new Payload();
                var pump =
                        Thread.ofPlatform()
                                .name("test-receive-pump")
                                .unstarted(
                                        () -> {
                                            admission.complete(null);
                                            request.success(payload);
                                        });
                pump.start();
                assertNotSame(pump, callback.toCompletableFuture().get(5, TimeUnit.SECONDS));
                pump.join();
                request.disposed.get(5, TimeUnit.SECONDS);
                if (admitted != null) admitted.toCompletableFuture().get(5, TimeUnit.SECONDS);
                assertEquals(1, request.decoded.get());
                request.terminalObserved.get(5, TimeUnit.SECONDS);
                assertEquals(1, request.observed.get());
                assertEquals(1, payload.closed.get());
            }
        }
    }

    @RepeatedTest(100)
    void earlyPublicFailureStillProcessesThePhysicalOutcomeOnce() throws Exception {
        for (boolean success : new boolean[] {false, true}) {
            for (boolean submissionFailure : new boolean[] {false, true}) {
                try (var registry = registry()) {
                    var request = new Request(registry);
                    var early = new IllegalStateException("public admission failed");
                    if (submissionFailure) assertTrue(request.result.rejectSubmission(early));
                    else assertTrue(request.result.completeExceptionally(early));
                    assertEquals(1, registry.pendingCount());
                    var payload = new Payload();
                    if (success) request.success(payload);
                    else request.failure(new IllegalArgumentException("physical failure"));
                    // A second terminal report cannot replace the physical outcome.
                    request.failure(new IllegalArgumentException("duplicate terminal"));
                    registry.close();
                    if (success) request.disposed.get(5, TimeUnit.SECONDS);
                    else request.failureProcessed.get(5, TimeUnit.SECONDS);
                    assertSame(
                            early,
                            assertThrows(CompletionException.class, request.result::join)
                                    .getCause());
                    request.terminalObserved.get(5, TimeUnit.SECONDS);
                    assertEquals(1, request.observed.get());
                    assertEquals(success ? 1 : 0, request.decoded.get());
                    assertEquals(success ? 0 : 1, request.failures.get());
                }
            }
        }
    }

    @RepeatedTest(100)
    void cancellationAndTimeoutDiscardLatePayloadWithoutReopeningCorrelation() throws Exception {
        for (boolean cancel : new boolean[] {false, true}) {
            AtomicLong clock = new AtomicLong();
            try (var registry =
                    new ZLinkServiceOperationRegistry(
                            command ->
                                    new Thread(command) {
                                        @Override
                                        public synchronized void start() {}
                                    },
                            new IllegalStateException("closed"),
                            () -> new TimeoutException("expired"),
                            clock::get)) {
                var request = new Request(registry);
                var terminal = request.result.handle((value, failure) -> failure);
                if (cancel) assertTrue(request.result.cancel(false));
                else {
                    clock.set(TIMEOUT.toNanos());
                    assertEquals(1, ZLinkCompletionBridge.await(registry.expire(clock.get())));
                }
                assertNotNull(terminal.get(5, TimeUnit.SECONDS));
                var payload = new Payload();
                request.success(payload);
                request.failure(new IllegalStateException("late physical failure"));
                assertEquals(1, payload.closed.get());
                assertEquals(0, request.decoded.get());
                assertEquals(0, registry.pendingCount());
                request.terminalObserved.get(5, TimeUnit.SECONDS);
                assertEquals(1, request.observed.get());
            }
        }
    }

    @RepeatedTest(100)
    void blockedContinuationDoesNotBlockAnotherCompletionOrReentry() throws Exception {
        try (var registry = registry()) {
            var first = new Request(registry);
            var firstPayload = new Payload();
            var entered = new CountDownLatch(1);
            var release = new CountDownLatch(1);
            var blocked =
                    first.result.thenRun(
                            () -> {
                                assertTrue(first.result.isDone());
                                assertEquals(
                                        1,
                                        firstPayload.closed.get(),
                                        "reply released before caller continuation");
                                entered.countDown();
                                try {
                                    assertTrue(release.await(5, TimeUnit.SECONDS));
                                } catch (InterruptedException error) {
                                    throw new AssertionError(error);
                                }
                            });
            try {
                first.success(firstPayload);
                assertTrue(entered.await(5, TimeUnit.SECONDS));
                var second = new Request(registry);
                var nested =
                        second.result.thenCompose(
                                value -> {
                                    var reentrant = new Request(registry);
                                    reentrant.success(new Payload());
                                    return reentrant.result;
                                });
                second.success(new Payload());
                assertEquals("typed", nested.get(5, TimeUnit.SECONDS));
            } finally {
                release.countDown();
            }
            blocked.get(5, TimeUnit.SECONDS);
            first.disposed.get(5, TimeUnit.SECONDS);
        }
    }

    @RepeatedTest(100)
    void shutdownDeliversEveryAcceptedCompletion() throws Exception {
        var registry = registry();
        try {
            var requests = new ArrayList<Request>();
            var callbacks = new ArrayList<CompletableFuture<?>>();
            for (int i = 0; i < 16; i++) {
                var request = new Request(registry);
                requests.add(request);
                callbacks.add(
                        request.result.handle(
                                (value, failure) -> {
                                    assertTrue(
                                            ZLinkServiceCompletionDispatcher.isCurrentCompletion());
                                    return null;
                                }));
            }
            for (int i = 0; i < 8; i++) requests.get(i).success(new Payload());
            registry.close();
            CompletableFuture.allOf(callbacks.toArray(CompletableFuture[]::new))
                    .get(5, TimeUnit.SECONDS);
            for (int i = 8; i < 16; i++) {
                requests.get(i).success(new Payload());
                requests.get(i).failure(new IllegalStateException("late close failure"));
            }
            for (var request : requests) request.disposed.get(5, TimeUnit.SECONDS);
            assertEquals(0, registry.pendingCount());
        } finally {
            registry.close();
        }
    }

    @RepeatedTest(100)
    void receiverRestoresCapturedFlowAcrossDecodeAndCallerCompletion() throws Exception {
        try (var registry = registry()) {
            var flow = ZLinkFlowContext.create(ZLinkFlowOrigin.APPLICATION);
            ZLinkServiceOperationRegistry.Operation<String> receiver;
            var decoded = new AtomicReference<ZLinkFlowContext.State>();
            try (var scope = ZLinkFlowContext.enter(flow)) {
                receiver =
                        ZLinkServiceOperationRegistry.<String, String>prepareOperation(
                                ZLinkServiceOperationIds.next(),
                                value -> {
                                    decoded.set(ZLinkFlowContext.current());
                                    return value;
                                },
                                null,
                                failure -> failure,
                                null);
            }
            registry.<String>submitTerminal(
                    receiver.id(), TIMEOUT, terminal -> null, ignored -> {}, receiver);
            var resumed = receiver.thenApply(value -> ZLinkFlowContext.current());
            registry.complete(receiver.id(), "reply");
            assertSame(flow, resumed.get(5, TimeUnit.SECONDS));
            assertSame(flow, decoded.get());
            assertNull(ZLinkFlowContext.current());
        }
    }

    @RepeatedTest(100)
    void codecFailureSettlesPublicResultAndReleasesPayload() throws Exception {
        try (var registry = registry()) {
            var error = new IllegalArgumentException("codec rejected reply");
            var disposed = new CompletableFuture<Void>();
            var receiver =
                    ZLinkServiceOperationRegistry.<Payload, String>prepareOperation(
                            ZLinkServiceOperationIds.next(),
                            payload -> {
                                throw error;
                            },
                            null,
                            failure -> failure,
                            null);
            registry.<Payload>submitTerminal(
                    receiver.id(),
                    TIMEOUT,
                    terminal -> null,
                    payload -> {
                        payload.close();
                        disposed.complete(null);
                    },
                    receiver);
            var payload = new Payload();
            assertTrue(registry.complete(receiver.id(), payload));
            assertSame(
                    error,
                    assertThrows(
                                    java.util.concurrent.ExecutionException.class,
                                    () -> receiver.get(5, TimeUnit.SECONDS))
                            .getCause());
            disposed.get(5, TimeUnit.SECONDS);
            assertEquals(1, payload.closed.get());
        }
    }

    @RepeatedTest(100)
    void closedRegistrationDeliversTheFinalReceiverInTheCompletionPool() throws Exception {
        try (var registry = registry()) {
            registry.close();
            var receiver =
                    ZLinkServiceOperationRegistry.<Payload, String>prepareOperation(
                            ZLinkServiceOperationIds.next(),
                            payload -> "unexpected",
                            null,
                            failure -> failure,
                            null);
            var callback =
                    receiver.handle(
                            (value, failure) -> {
                                assertNotNull(failure);
                                assertTrue(ZLinkServiceCompletionDispatcher.isCurrentCompletion());
                                assertTrue(ForkJoinTask.inForkJoinPool());
                                return true;
                            });
            assertFalse(
                    registry.<Payload>submitTerminal(
                                    receiver.id(),
                                    TIMEOUT,
                                    terminal -> {
                                        fail("closed registry submitted a physical request");
                                        return null;
                                    },
                                    Payload::close,
                                    receiver)
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS));
            assertTrue(callback.get(5, TimeUnit.SECONDS));
        }
    }

    @RepeatedTest(100)
    void rejectedUnsubmittedReceiverCannotStartPhysicalWork() throws Exception {
        try (var registry = registry()) {
            var request =
                    ZLinkServiceOperationRegistry.<Payload, String>prepareOperation(
                            ZLinkServiceOperationIds.next(),
                            payload -> fail("rejected receiver decoded"),
                            null,
                            failure -> failure,
                            null);
            var failure = new IllegalStateException("submission rejected before registration");
            assertTrue(request.rejectSubmission(failure));
            assertSame(failure, assertThrows(CompletionException.class, request::join).getCause());
            var submissions = new AtomicInteger();
            assertFalse(
                    registry.<Payload>submitTerminal(
                                    request.id(),
                                    TIMEOUT,
                                    terminal -> {
                                        submissions.incrementAndGet();
                                        return null;
                                    },
                                    Payload::close,
                                    request)
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS));
            assertEquals(0, submissions.get());
            assertEquals(0, registry.pendingCount());
        }
    }

    @Test
    void conversionPathsContainNoFutureOrCompletionObservation() throws Exception {
        String source =
                Files.readString(
                        Path.of(
                                "src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelRouteCalls.java"));
        verifyConversionPaths(source);
        String bad =
                source.replace(
                        "runtime.requestNode(",
                        "result.whenComplete((v, e) -> {}); runtime.requestNode(");
        assertThrows(AssertionError.class, () -> verifyConversionPaths(bad));
    }

    private static void verifyConversionPaths(String source) {
        for (String name :
                new String[] {"submitRouter", "submitNode", "submitClient", "submitMesh"}) {
            int start = source.indexOf("CompletionStage<TReply> " + name + "(");
            assertTrue(start >= 0);
            int end = source.indexOf("\n    private ", start);
            String method = source.substring(start, end);
            assertFalse(method.contains("whenComplete("), name);
            assertFalse(method.contains("forwardCancellation("), name);
            assertFalse(method.contains("new CompletableFuture"), name);
        }
        assertTrue(source.contains("prepareOperation("));
    }
}
